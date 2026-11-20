////////////////////////////////////////////////////////////////////////////
// Model: ore_concentration.cc          SIMLIB/C++   (IMS project)
//
// Zone 1 of the REE -> NdFeB magnet supply chain:
//
//   ORE_SRC  (stochastic ore source: Exponential inter-arrivals,
//             random batch mass, truncated-normal REO grade,
//             Dirichlet element basket)
//       |
//       v
//   CRUSHER (Facility)  -- crushing
//       |
//       v
//   FLOTLINE (Facility) -- flotation + magnetic separation,
//                          Triag cycle time, mass balance ore->concentrate
//       |
//       v
//   ( Q_CONC ) (Store)   -- WIP buffer, capacity in tons;
//                          batches BLOCK here when the buffer is full
//       |
//       v
//   DigestStub           -- PLACEHOLDER for Zone 2 (acid digestion),
//                            removes concentrate batches;
//                            delete this class when Zone 2 is real
//
// Output: ore_concentration.dat (time series + statistics)
// Plot:   gnuplot ore_concentration.plt   (or see .dat columns)
//
#include "simlib.h"

//////////////////////////// parameters ///////////////////////////////////

static const double T_END    = 24.0 * 90; // simulation horizon [h] = 90 days
static const double T_SAMPLE = 1.0;       // time-series sampling interval [h]

// --- stochastic ore source (ORE_SRC) ---
static const double ARRIVAL_MEAN = 10.0;  // mean inter-arrival time [h]
static const double ORE_MIN = 80.0, ORE_MAX = 120.0;    // ROM batch mass [t]
static const double GRADE_MI = 0.08, GRADE_SIGMA = 0.01; // REO grade ~ N(mi,s)
static const double GRADE_LO = 0.05, GRADE_HI = 0.11;    // ... truncated

// --- beneficiation line (PREP) ---
static const double T_CRUSH_LO = 1.0, T_CRUSH_HI = 1.5;      // crushing [h]
static const double T_FLOT_LO = 5.0, T_FLOT_MOD = 7.0, T_FLOT_HI = 11.0; // flot.
static const double RECOV_MOD = 0.80, RECOV_LO = 0.72, RECOV_HI = 0.88;  // recovery
static const double CONC_GRADE = 0.55;                      // concentrate grade

// --- WIP buffer (Q_CONC) ---
static const unsigned long Q_CONC_CAP = 150; // concentrate capacity [t]

// --- PLACEHOLDER for Zone 2: acid digestion stub ---
static const unsigned long DIGEST_BATCH = 20; // concentrate per batch [t]
static const double DIGEST_MEAN = 20.0;      // digestion cycle time [h]

// --- element "basket" of the ore (Dirichlet alphas, bastnasite-like) ---
enum { N_ELEM = 5 };
static const char *const ELEM_NAME[N_ELEM]  = { "La", "Ce", "Pr", "Nd", "Dy" };
static const double BASKET_ALPHA[N_ELEM]   = { 2.4, 5.0, 0.55, 1.7, 0.18 };

///////////////////////// model objects ///////////////////////////////////

Facility  Crusher("Crusher");                         // crushing
Facility  FlotLine("Flotation + magnetic separation"); // main line
Store     QConc("Q_CONC - concentrate WIP buffer", Q_CONC_CAP);

Stat      SGrade("REO grade of ROM ore [-]");
Stat      SConc("concentrate batch [t]");
Stat      SFlow("batch flow time: ROM -> WIP [h]");
Histogram HConc("concentrate batch [t]", 0.0, 2.0, 13);

static double ElemCum[N_ELEM]; // cumulative REO captured per element [t]
static double CumConc = 0;      // cumulative concentrate into Q_CONC [t]
static double CumOut  = 0;      // cumulative concentrate consumed by stub [t]

///////////////////////// helper functions /////////////////////////////////

static unsigned long Tons(double t) { // round mass to integer tons (Store units)
    return (unsigned long)(t + 0.5);
}

static double TruncNormal(double mi, double sigma, double lo, double hi) {
    double x;
    do { x = Normal(mi, sigma); } while (x < lo || x > hi);
    return x;
}

// Gamma(a, 1) variate for any shape a > 0 (Marsaglia-Tsang method).
// NOTE: SIMLIB's own Gamma() is broken for non-integer shape parameters
// (its accept/reject test can never reject -> huge outliers), so we build
// the sampler from SIMLIB's correct Normal()/Uniform() primitives instead.
static double GamShape(double a) {
    if (a < 1.0) { // boost small shapes: G(a) = G(a+1) * U^(1/a)
        return GamShape(a + 1.0) * pow(Uniform(0, 1), 1.0 / a);
    }
    const double d = a - 1.0 / 3.0;
    const double c = 1.0 / sqrt(9.0 * d);
    for (;;) {
        double x, v;
        do {
            x = Normal(0, 1);
            v = 1.0 + c * x;
        } while (v <= 0);
        v = v * v * v;
        double u = Uniform(0, 1);
        if (u < 1.0 - 0.0331 * x * x * x * x)
            return d * v;
        if (log(u) < 0.5 * x * x + d * (1.0 - v + log(v)))
            return d * v;
    }
}

// Dirichlet draw: normalized independent Gamma(a_j, 1) variates
static void DrawBasket(double *x) {
    double sum = 0;
    for (int j = 0; j < N_ELEM; j++) {
        x[j] = GamShape(BASKET_ALPHA[j]);
        sum += x[j];
    }
    for (int j = 0; j < N_ELEM; j++)
        x[j] /= sum;
}

///////////////////////// model entities //////////////////////////////////

// One run-of-mine ore batch = "object of many materials until separated":
// carries its mass, grade and element basket through the plant.
class OreBatch : public Process {
    double ore_mass;         // [t] run-of-mine ore
    double grade;            // REO fraction in ore
    double recovery;         // REE recovery of beneficiation
    double basket[N_ELEM];  // element shares of captured REO
    double t_start;         // arrival time
    double conc_mass;        // [t] concentrate produced
  public:
    OreBatch() :
        ore_mass(Uniform(ORE_MIN, ORE_MAX)),
        grade(TruncNormal(GRADE_MI, GRADE_SIGMA, GRADE_LO, GRADE_HI)),
        recovery(Triag(RECOV_MOD, RECOV_LO, RECOV_HI)),
        t_start(Time)
    {
        DrawBasket(basket);
        SGrade(grade);
    }
    void Behavior() {
        // 1) crushing
        Seize(Crusher);
        Wait(Uniform(T_CRUSH_LO, T_CRUSH_HI));
        Release(Crusher);
        // 2) flotation + magnetic separation
        Seize(FlotLine);
        Wait(Triag(T_FLOT_MOD, T_FLOT_LO, T_FLOT_HI));
        Release(FlotLine);
        // 3) beneficiation mass balance (concentrate keeps basket ratio)
        double reo_mass = ore_mass * grade * recovery;
        conc_mass = reo_mass / CONC_GRADE;
        for (int j = 0; j < N_ELEM; j++)
            ElemCum[j] += reo_mass * basket[j];
        // 4) push into WIP buffer --- blocks here when buffer is full
        Enter(QConc, Tons(conc_mass));
        CumConc += conc_mass;
        SConc(conc_mass);
        HConc(conc_mass);
        SFlow(Time - t_start);
    }
};

// Stochastic ore source (bastnasite / monazite basket vector + variance)
class OreSource : public Event {
    void Behavior() {
        (new OreBatch)->Activate();
        Activate(Time + Exponential(ARRIVAL_MEAN));
    }
};

// PLACEHOLDER for Zone 2 (acid digestion): consumes concentrate batches.
// Remove this class when the real DIGEST processor is added.
class DigestStub : public Process {
    void Behavior() {
        for (;;) {
            while (QConc.Used() < DIGEST_BATCH)   // starvation: poll
                Wait(1.0);                       // check hourly
            Leave(QConc, DIGEST_BATCH);
            CumOut += DIGEST_BATCH;
            Wait(Exponential(DIGEST_MEAN));      // batch cycle time
        }
    }
};

///////////////////////// experiment control //////////////////////////////

void Sample() { // time-series output
    Print("%9.3f %5lu %3u %3u %3u %9.1f %9.1f\n",
          Time, QConc.Used(), QConc.QueueLen(),
          Crusher.QueueLen(), FlotLine.QueueLen(), CumConc, CumOut);
}
Sampler S(Sample, T_SAMPLE);

int main() {
    SetOutput("ore_concentration.dat");
    Print("# Zone 1: ORE_SRC -> CRUSHER -> FLOTATION -> Q_CONC\n");
    Print("# time[h] Q_CONC[t] q_wip q_crush q_flot cum_conc[t] cum_out[t]\n");
    RandomSeed(42);                // reproducible experiment
    Init(0, T_END);
    (new OreSource)->Activate();
    (new DigestStub)->Activate();
    Run();
    SIMLIB_statistics.Output();
    // results:
    Crusher.Output();
    FlotLine.Output();
    QConc.Output();
    SGrade.Output();
    SConc.Output();
    SFlow.Output();
    HConc.Output();
    double total = 0;
    for (int j = 0; j < N_ELEM; j++)
        total += ElemCum[j];
    Print("# captured REO per element (basket split):\n");
    for (int j = 0; j < N_ELEM; j++)
        Print("#   %-2s %9.1f t  %5.1f %%\n", ELEM_NAME[j], ElemCum[j],
               total > 0 ? 100.0 * ElemCum[j] / total : 0.0);
}
