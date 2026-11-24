/*
    #####################################
    #### Real world simulation match ####
*/

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <iostream>
#include <algorithm>

#define i64 int64_t
#define b32 int32_t

#define T_ID int64_t

// Base simulation time is in milliseconds.
constexpr i64 MS   =          1;
constexpr i64 SEC  =      1'000;
constexpr i64 MIN  =     60'000;
constexpr i64 HOUR =  3'600'000;
constexpr i64 DAY  = 86'400'000;

// ###################################################################
// Miner params — realistic setup, rationale in zone1-ore-extraction.md §3.2–§3.4.
//
// - Exp arrivals: uncoordinated external supply, memoryless gaps.
//
// - U(80,120) t: truckload mass, simplest bounded draw.
//
// - Grade: AR(1) drift around the deposit mean — real mines feed from
//   different faces over days, so grade wanders slowly (corr. time ~5 d)
//   instead of jumping per batch. Bounds are a physical guarantee (±6σ,
//   essentially never bind).
//
// - Basket: hardcoded deposit constant — the real bastnäsite pattern
//   (Ce>La>Nd≫Pr, Dy trace), fixed by the mineral lattice, so no per-batch
//   draw. Concentration preserves the basket: every ton of Nd drags
//   ~4.2 t Ce + ~2.8 t La through the plant.
//
// - Stock feedback: the mine throttles against its own stockpile (the blob
//   it feeds). Pad near target → nominal pace; pad full → slow down;
//   pad dry → catch up. speed = clamp(TARGET / stock, MIN, MAX).
//
// - Dy is NOT in the ore (real bastnäsite carries <0.1 %). It arrives as
//   imported Dy oxide lots into its own blob (g_additives), outside the ore
//   chain until a consumer exists.
constexpr double TONS_IN_BASKET_MIN = 80;
constexpr double TONS_IN_BASKET_MAX = 120;
constexpr double ARRIVAL_MEAN = 10.0 * HOUR; // time to dig one batch

// Feed grade AR(1): g_{k+1} = MEAN + PHI·(g_k − MEAN) + ε, ε ~ N(0, INNOV).
// PHI = 0.92 per ~10 h batch ⇒ correlation time ≈ 5 days; stationary sd 0.005.
constexpr double GRADE_MEAN          = 0.08;
constexpr double GRADE_STATIONARY_SD = 0.005;
constexpr double GRADE_PHI           = 0.92;
constexpr double GRADE_MIN = 0.05, GRADE_MAX = 0.11;
const    double GRADE_INNOV_SD = GRADE_STATIONARY_SD * std::sqrt(1.0 - GRADE_PHI*GRADE_PHI);

// Basket: fixed deposit constant — real Mountain Pass bastnäsite pattern,
// renormalized over the five tracked elements: La 33 %, Ce 50 %, Pr 4 %,
// Nd 12 %, Dy 0.1 %. Sums to 1; set by the mineral's crystal lattice.
constexpr double BASKET[] = {0.333, 0.505, 0.040, 0.121, 0.001};
// elements: La, Ce, Pr, Nd, Dy
constexpr int    N_ELEMS = sizeof(BASKET) / sizeof(BASKET[0]);

// Stock feedback on mining pace (see miner bullet above).
constexpr double STOCK_TARGET   = 150.0; // t of ore the mine aims to keep piled
constexpr double MINE_SPEED_MIN = 0.5;   // pace multiplier when the pad is full
constexpr double MINE_SPEED_MAX = 2.0;   // pace multiplier when the pad runs dry

// #####################################################################
// Imported additives (Dy oxide for the magnet line): memoryless shipping
// gaps. Lives in its own blob — nothing in the ore chain consumes it yet.
constexpr double ADDITIVE_LOT_TONS     = 10.0;        // t per delivery
constexpr double ADDITIVE_ARRIVAL_MEAN = 10.0 * DAY;  // ≈ 1 t/day
constexpr int    ADDITIVE_ELEM         = N_ELEMS - 1; // Dy

// #####################################################################
// Crusher params
constexpr double CRUSHER_MIN_TIME = 1 * HOUR;
constexpr double CRUSHER_MAX_TIME = 1.5 * HOUR;

// #####################################################################
// Flotation params
constexpr double FLOTATION_MIN_TIME = 5 * HOUR;
constexpr double FLOTATION_MID_TIME = 7 * HOUR;
constexpr double FLOTATION_MAX_TIME = 11 * HOUR;

// REO recovery into concentrate: Tri(0.72, 0.80, 0.88) — plant-typical
// 70–85 % [M]; element-uniform (recovery is mineral-driven, not element-driven).
constexpr double FLOTATION_MIN_RECOVERY = 0.72;
constexpr double FLOTATION_MID_RECOVERY = 0.80;
constexpr double FLOTATION_MAX_RECOVERY = 0.88;

// Concentrate grade setpoint — real concentrates run 40–60 % REO [H].
constexpr double CONC_GRADE = 0.55;

// Ground simulation params.
constexpr i64 SEED  = 'm' + 'e' + 'o' + 'w';

[[noreturn]] void assert_fail(const char* why) {
    std::cerr << "[Assert]: " << why;
    exit(1);
}

// Macro (not function) so the optimizer can prune the impossible path after a
// failed check — keeps -Warray-bounds quiet.
#define assert(cond, why) ((cond) ? (void)0 : assert_fail(why))

// Standard normal CDF: Phi(x) = 0.5 * (1 + erf(x / sqrt(2))).
static double normal_cdf(double x) {
    return 0.5 * (1.0 + std::erf(x * 0.70710678118654752));
}

// Inverse error function (Abramowitz & Stegun 7.1.26-based approximation).
static double erfinv(double y) {
    constexpr double A  = 0.147;
    constexpr double PI = 3.14159265358979323846;
    double ln = std::log(1.0 - y * y);
    double t  = 2.0 / (PI * A) + ln / 2.0;
    return std::copysign(std::sqrt(-t + std::sqrt(t * t - ln / A)), y);
}

class C_World {
    public:
        const i64 END = 5 * 365 * DAY; // assignment experiment: several years

        i64 time = 0;

        // Tailings sink (flotation reject): unrecovered REO per element + gangue.
        double tailings[N_ELEMS] = {};
        double tailings_gangue = 0;

        // Single global RNG
        std::mt19937_64 rng;

        double uniform(double lo, double hi) {
            return std::uniform_real_distribution<double>(lo, hi)(rng);
        }

        double exponential(double mean) {
            return std::exponential_distribution<double>(1.0 / mean)(rng);
        }

        double normal_distribution(double mean, double deviation) {
            return std::normal_distribution<double>(mean, deviation)(rng);
        }

        double normal_clamped(double mean, double deviation, std::pair<double, double>clamp) {
            // Exact truncated normal: draw u uniformly between the boundary CDF
            // values, then map back through the inverse normal CDF.
            double lo = normal_cdf((clamp.first  - mean) / deviation);
            double hi = normal_cdf((clamp.second - mean) / deviation);
            double u  = uniform(lo, hi);
            return mean + deviation * std::sqrt(2.0) * erfinv(2.0 * u - 1.0);
        }

        double triangular(double lo, double mode, double hi) {
            double u = uniform(0, 1);
            if (u < (mode - lo) / (hi - lo)) return lo + std::sqrt(u * (hi - lo) * (mode - lo));
            return hi - std::sqrt((1 - u) * (hi - lo) * (hi - mode));
        }

        double gamma(double n) {
            return std::gamma_distribution<double>(n)(rng);
        }

        C_World() {
            rng.seed(SEED);
        }
};

C_World g_w;

constexpr int MAX_RING_BUFFER_SIZE = 100;

typedef struct T_Ring_Buffer {
    i64 buf[MAX_RING_BUFFER_SIZE];
    
    i64 size       = MAX_RING_BUFFER_SIZE;
    i64 read_head  = 0;
    i64 write_head = 0;

} T_Ring_Buffer;

T_Ring_Buffer init_ring_buf(i64 size) {
    T_Ring_Buffer t;
    t.size = size;
    
    return t;
}

// One aggregate material mass — "the blob". elem[e] is the contained REO
// tonnage of element e; waste is the gangue. Ore tons = Σ elem + waste; head
// grade is derivable: Σ elem / ore tons. No batch identity: a node's blob is
// a blend of everything it received, like a real stockpile.
typedef struct T_Mass {
    double elem[N_ELEMS] = {};
    double waste = 0;
} T_Mass;

enum E_Node_Type {
    E_MINER,
    E_TRANSFER, // Can also split based on rules.
    E_ADDITIVE_SRC, // imported additives (e.g. Dy oxide) — pools into g_additives
    E_CRUSHER,
    E_FLOTATION,
};

typedef struct T_Node {
    T_ID id;
    E_Node_Type type;
    T_Ring_Buffer buf;

    T_ID input = -1;
    T_ID output = -1;

    i64 done_at = -1;

    T_Mass mass; // the node's blob

    // E_TRANSFER
    i64 transfer_length = 0;

} T_Node;

constexpr int MAX_NODE_COUNT = 100;
T_Node g_nodes[MAX_NODE_COUNT];
int g_node_count = 0;

// Imported additive stock — its own blob, outside the ore chain (see params).
// Deliveries arrive on memoryless shipping gaps and just pool up here until
// a consumer (the magnet line) exists. Nothing in the ore chain consumes it.
T_Mass g_additives;

T_Node* init_node(E_Node_Type type, i64 buf_size) {
    // direct if (not the assert macro): GCC's -Warray-bounds only prunes the
    // out-of-bounds path when the guard is a plain statement in this function.
    if (g_node_count >= MAX_NODE_COUNT) assert_fail("Too many nodes. Increase MAX_NODE_COUNT.");
    T_Node* n = &g_nodes[g_node_count];
    n->id = g_node_count++;
    n->type = type;
    n->buf = init_ring_buf(buf_size);

    return n;
}

T_Node* init_transfer_node(i64 length) {
    T_Node* n = init_node(E_TRANSFER, 0);
    n->transfer_length = length;

    return n;
}

void link_nodes(T_ID from, T_ID to) {
    T_Node* from_n = &g_nodes[from];
    T_Node* to_n   = &g_nodes[to];

    if(to_n->type != E_TRANSFER) {
        assert(false, "Output node isn't transfer.");
    }

    from_n->output = to_n->id;
    to_n->input    = from_n->id;
}

b32 step(T_ID id) {
    T_Node* n = &g_nodes[id];
    assert(n->output != -1, "Node has no output.");
    if (n->type == E_TRANSFER) assert(n->input != -1, "Transfer has no input.");

    switch(n->type) {
        case E_MINER:
        {
            // Grade of received ore. Changes over time.
            static double grade_state = GRADE_MEAN;
            grade_state = GRADE_MEAN
                        + GRADE_PHI * (grade_state - GRADE_MEAN)
                        + g_w.normal_distribution(0.0, GRADE_INNOV_SD);
            if (grade_state < GRADE_MIN) grade_state = GRADE_MIN;
            if (grade_state > GRADE_MAX) grade_state = GRADE_MAX;

            // Total amount of raw ore received
            double tons = g_w.uniform(TONS_IN_BASKET_MIN, TONS_IN_BASKET_MAX);

            double reo = 0;
            for (int el = 0; el < N_ELEMS; el++) {
                reo += n->mass.elem[el] += tons * grade_state * BASKET[el];
            }
            n->mass.waste += tons - reo;

            // Mine throttles against its own stockpile: pad near target runs at
            // nominal pace, a full pad slows the mine, a dry pad speeds it up.
            double stock = n->mass.waste;
            for (int el = 0; el < N_ELEMS; el++) stock += n->mass.elem[el];
            double speed = std::clamp(STOCK_TARGET / stock, MINE_SPEED_MIN, MINE_SPEED_MAX);

            n->done_at = g_w.time + g_w.exponential(ARRIVAL_MEAN / speed);
        } break;


        case E_TRANSFER:
        {
            n->done_at = g_w.time + n->transfer_length;
        } break;


        case E_ADDITIVE_SRC:
        {
            // imported additive delivery on memoryless shipping gaps — pools
            // into its own blob; nothing in the ore chain consumes it yet
            g_additives.elem[ADDITIVE_ELEM] += ADDITIVE_LOT_TONS;
            n->done_at = g_w.time + g_w.exponential(ADDITIVE_ARRIVAL_MEAN);
        } break;


        case E_CRUSHER:
        {
            n->done_at = g_w.time + g_w.uniform(CRUSHER_MIN_TIME, CRUSHER_MAX_TIME);
        } break;


        case E_FLOTATION:
        {
            // Mass map on the holdup blob: recover r of the REO into a
            // concentrate at the grade setpoint, the rest goes to tailings.
            //   conc mass = recovered/CONC_GRADE; the mass above pure REO is
            //   gangue reporting to the concentrate, the rest to tailings.
            double r = g_w.triangular(FLOTATION_MIN_RECOVERY,
                                      FLOTATION_MID_RECOVERY,
                                      FLOTATION_MAX_RECOVERY);

            double rec_reo = 0;
            for (int el = 0; el < N_ELEMS; el++) {
                g_w.tailings[el] += n->mass.elem[el] * (1.0 - r);
                n->mass.elem[el] *= r;
                rec_reo += n->mass.elem[el];
            }

            double conc_mass = rec_reo / CONC_GRADE;
            double gangue_to_conc = conc_mass - rec_reo;
            g_w.tailings_gangue += n->mass.waste - gangue_to_conc;
            n->mass.waste = gangue_to_conc;

            n->done_at = g_w.time + g_w.triangular(FLOTATION_MIN_TIME,
                                                   FLOTATION_MID_TIME,
                                                   FLOTATION_MAX_TIME);

        } break;


        default:
        {
            assert(false, "Unhandled/Wrong node type.");
        } break;
    }

    return true;
}


int main(void) {
    // element shares: fixed deposit constant
    for (int el = 0; el < N_ELEMS; el++) std::cout << BASKET[el] << " ";
    std::cout << "\n";

    return 0;
}
