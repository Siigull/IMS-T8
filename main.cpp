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
// Truck transfer: discrete haulage mine → crusher. Waits for a full load,
// constant drive time, dumps into the crusher's feed stock on arrival.
constexpr double TRUCK_CAPACITY   = 100.0;    // t per load
constexpr i64    TRUCK_DRIVE_TIME = 1 * HOUR; // one way, loaded or empty

// #####################################################################
// Conveyor transfer: granular in-plant flow crusher → flotation. The belt is
// a circular buffer of cells; every tick it advances one cell, discharges the
// tail into the downstream stock and pulls a fresh cell from upstream. It
// only starts outputting once primed (filled to capacity), then keeps
// flowing until actually empty (smoother variant — no stop-start pulsing).
constexpr i64    BELT_TICK  = 5 * MIN;                         // one iteration
constexpr double BELT_RATE  = 100.0;                          // t/h
constexpr int    BELT_CELLS = 6;                              // τ = 30 min
constexpr double BELT_CELL_CAP  = BELT_RATE * BELT_TICK / (double)HOUR;
constexpr double BELT_CAPACITY  = BELT_CELL_CAP * BELT_CELLS; // in-flight holdup

// #####################################################################
// Crusher: rate server — consumes its feed stock at capacity, product lands
// in its out-stock for the belt to meter out. Rates calibrated from the old
// U(1, 1.5) h per ~100 t batch cycle.
constexpr double CRUSHER_MIN_RATE = 100.0 / 1.5; // t/h
constexpr double CRUSHER_MAX_RATE = 100.0 / 1.0; // t/h

// #####################################################################
// Flotation: rate server + mass map. Feed capacity calibrated from the old
// Tri(5, 7, 11) h per ~100 t campaign — deliberately the tight server.
// Recovery r is drawn per campaign (chemistry shifts between runs, not
// between 5-min ticks). Concentrate lands at the grade setpoint below.
constexpr double FLOTATION_MIN_CAPACITY = 100.0 / 11.0; // t/h
constexpr double FLOTATION_MID_CAPACITY = 100.0 / 7.0;  // t/h
constexpr double FLOTATION_MAX_CAPACITY = 100.0 / 5.0;  // t/h

// Concentrate grade setpoint — real concentrates run 40–60 % REO [H].
constexpr double CONC_GRADE = 0.55;

// REO recovery into concentrate: Tri(0.72, 0.80, 0.88) — plant-typical
// 70–85 % [M]; element-uniform (recovery is mineral-driven, not element-driven).
constexpr double FLOTATION_MIN_RECOVERY = 0.72;
constexpr double FLOTATION_MID_RECOVERY = 0.80;
constexpr double FLOTATION_MAX_RECOVERY = 0.88;

// #####################################################################
// Imported additives (Dy oxide for the magnet line): memoryless shipping
// gaps. Pools into its own blob — nothing in the ore chain consumes it yet,
// so the source node is built but not scheduled.
constexpr double ADDITIVE_LOT_TONS     = 10.0;        // t per delivery
constexpr double ADDITIVE_ARRIVAL_MEAN = 10.0 * DAY;  // ≈ 1 t/day
constexpr int    ADDITIVE_ELEM         = N_ELEMS - 1; // Dy

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
// grade is derivable: Σ elem / ore tons. No batch identity: a blob is a
// blend of everything that entered it, like a real stockpile.
typedef struct T_Mass {
    double elem[N_ELEMS] = {};
    double waste = 0;
} T_Mass;

static double mass_tons(const T_Mass* m) {
    double t = m->waste;
    for (int el = 0; el < N_ELEMS; el++) t += m->elem[el];
    return t;
}

// Move the whole blob src into dst (src ends empty).
static void mass_move(T_Mass* dst, T_Mass* src) {
    for (int el = 0; el < N_ELEMS; el++) {
        dst->elem[el] += src->elem[el];
        src->elem[el] = 0;
    }
    dst->waste += src->waste;
    src->waste = 0;
}

// Pull up to `want` tons out of src, proportionally (blend-preserving),
// into dst. Takes less when src holds less.
static void mass_take(T_Mass* dst, T_Mass* src, double want) {
    double total = mass_tons(src);
    if (total <= 0.0) return;
    if (want > total) want = total;
    double f = want / total;
    for (int el = 0; el < N_ELEMS; el++) {
        dst->elem[el] += src->elem[el] * f;
        src->elem[el] *= (1.0 - f);
    }
    dst->waste += src->waste * f;
    src->waste *= (1.0 - f);
}

enum E_Node_Type {
    E_MINER,
    E_TRANSFER,     // Can also split based on rules.
    E_ADDITIVE_SRC, // imported additives (e.g. Dy oxide) — pools into g_additives
    E_TRUCK,
    E_CRUSHER,
    E_CONVEYOR,
    E_FLOTATION,
    E_TAILINGS,     // passive sink: the dam (never scheduled)
};

enum E_Truck_State {
    T_LOADING,   // parked at the mine, waiting for a full load
    T_HAULING,   // loaded, driving to the crusher
    T_RETURNING, // empty, driving back to the mine
};

typedef struct T_Node {
    T_ID id;
    E_Node_Type type;
    T_Ring_Buffer buf;

    T_ID input = -1;
    T_ID output = -1;

    i64 done_at = -1; // next scheduled event; -1 = idle

    // Owned stock: feed-side holdup, material in processing, product side.
    T_Mass mass_unchecked;
    b32 is_mass_unchecked = false;
    T_Mass mass;
    T_Mass wip;
    T_Mass out;

    // E_TRUCK
    int truck_state = T_LOADING;

    // E_CONVEYOR
    T_Mass belt[BELT_CELLS];
    i64 belt_size = BELT_CELLS;
    i64 belt_head = 0;

    // E_TRANSFER
    i64 transfer_length = 0;

} T_Node;

constexpr int MAX_NODE_COUNT = 100;
T_Node g_nodes[MAX_NODE_COUNT];
int g_node_count = 0;

// Memory helpers
T_ID g_send_nodes[MAX_NODE_COUNT];
int g_send_node_count = 0;
T_ID g_receive_nodes[MAX_NODE_COUNT];
int g_receive_node_count = 0;

// Imported additive stock — its own blob, outside the ore chain (see params).
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
    for(int i=0; i<g_send_node_count; i++) {
        assert(g_send_nodes[i] != from, "Send to only one node for now.");
    }
    g_send_nodes[g_send_node_count++] = from;

    for(int i=0; i<g_receive_node_count; i++) {
        assert(g_receive_nodes[i] != from, "Receive from only one node for now.");
    }
    g_receive_nodes[g_receive_node_count++] = from;

    T_Node* from_n = &g_nodes[from];
    T_Node* to_n   = &g_nodes[to];

    from_n->output = to_n->id;
    to_n->input    = from_n->id;
}

// Fired when a node's scheduled event comes due.
b32 send(T_ID id) {
    assert(id < g_node_count && id >= 0, "Code problem. Send ID out of range.");
    T_Node* n = &g_nodes[id];

    if(n->done_at != g_w.time) return;

    assert(n->output != -1, "Mistake in initing g_send_nodes.");
    T_Node* dst = &g_nodes[n->output];
    // T_Node* src = &g_nodes[n->input];

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
            double stock = mass_tons(&n->mass);
            double speed = std::clamp(STOCK_TARGET / stock, MINE_SPEED_MIN, MINE_SPEED_MAX);

            n->done_at = g_w.time + g_w.exponential(ARRIVAL_MEAN / speed);
        } break;


        case E_TRANSFER:
        {
            assert(n->input != -1, "Transfer has no input.");
            n->done_at = g_w.time + n->transfer_length;
        } break;


        case E_ADDITIVE_SRC:
        {
            // imported additive delivery on memoryless shipping gaps — pools
            // into its own blob; nothing in the ore chain consumes it yet
            g_additives.elem[ADDITIVE_ELEM] += ADDITIVE_LOT_TONS;
            n->done_at = g_w.time + g_w.exponential(ADDITIVE_ARRIVAL_MEAN);
        } break;


        case E_TRUCK:
        {
            if (n->truck_state == T_HAULING) {
                // Arrived at the crusher: dump the cargo into its feed stock.
                // (A real dump pocket can refuse when full — the truck then
                // waits parked right next to the crusher. That refusal needs a
                // stock capacity, which does not exist yet, so for now the
                // dump always succeeds.)
                mass_move(&g_nodes[n->output].mass, &n->mass, &dst->is_mass_unchecked);
                n->truck_state = T_RETURNING;
            } else {
                // Back at the mine, empty. Loading happens in the pull sweep.
                n->truck_state = T_LOADING;
                n->done_at = -1;
                return true;
            }
            n->done_at = g_w.time + TRUCK_DRIVE_TIME;
        } break;


        case E_CRUSHER:
        {
            // Run done: crushed product moves to the out-stock the belt
            // meters from. The sweep starts the next run if stock remains.
            mass_move(&n->out, &n->wip);
            n->done_at = -1;
        } break;


        case E_CONVEYOR:
        {
            i64 head = n->belt_head / n->belt_size;

            mass_move(&dst->mass, &n->belt[head]);

            // double level = 0;
            // for (int i = 0; i < BELT_CELLS; i++) level += mass_tons(&n->belt[i]);
            // double want = std::min(BELT_CELL_CAP, BELT_CAPACITY - level);
            // mass_take(&n->belt[head], &src->out, BELT_CELL_CAP);

            // 4) priming: full belt starts flowing; fully drained belt stops
            // level = 0;
            // for (int i = 0; i < BELT_CELLS; i++) level += mass_tons(&n->belt[i]);
            // if (level >= BELT_CAPACITY - 1e-9) n->belt_primed = true;
            // if (level <= 1e-9) n->belt_primed = false;

            // stay scheduled while there is anything to move, else go idle
            // (the pull sweep wakes the belt when product appears upstream)
            n->done_at = g_w.time + BELT_TICK;
        } break;


        case E_FLOTATION:
        {
            // Campaign done: apply the mass map to the campaign feed (wip).
            // Same algebra as ever, just per campaign slice at its own pace:
            //   recovered REO  elem·r     → out-stock (concentrate, grade setpoint)
            //   lost REO       elem·(1−r) → tailings dam
            //   conc mass = recovered/CONC_GRADE; the mass above pure REO is
            //   gangue reporting to the concentrate, the rest to the dam.
            double r = g_w.triangular(FLOTATION_MIN_RECOVERY,
                                      FLOTATION_MID_RECOVERY,
                                      FLOTATION_MAX_RECOVERY);

            T_Node* dam = &g_nodes[n->output];
            double rec = 0;
            for (int el = 0; el < N_ELEMS; el++) {
                dam->mass.elem[el] += n->wip.elem[el] * (1.0 - r);
                n->wip.elem[el] *= r;
                rec += n->wip.elem[el];
            }

            double conc_mass = rec / CONC_GRADE;
            double gangue_to_conc = conc_mass - rec;
            dam->mass.waste += n->wip.waste - gangue_to_conc;
            n->wip.waste = gangue_to_conc;

            mass_move(&n->out, &n->wip);
            n->done_at = -1;
        } break;


        default:
        {
            assert(false, "Unhandled/Wrong node type.");
        } break;
    }

    return true;
}

// Pull sweep: lets a node act on state changes (not on time). Returns true
// when it did something, so the engine scans again until quiescence. This is
// what wakes idle trucks, servers and belts when stock appears for them.
static b32 pull(T_ID id) {
    assert(id < g_node_count && id >= 0, "Code problem. Pull ID out of range.");
    T_Node* n = &g_nodes[id];
    T_Node* dst = &g_nodes[n->output];

    switch (n->type) {
        case E_TRUCK:
        {
            // Wait-for-full: depart only when the pad can fill the truck.
            if (n->truck_state != T_LOADING || n->done_at != -1) return false;
            T_Node* pad = &g_nodes[n->input];
            if (mass_tons(&pad->mass) < TRUCK_CAPACITY) return false;

            mass_take(&n->mass, &pad->mass, TRUCK_CAPACITY);
            n->truck_state = T_HAULING;
            n->done_at = g_w.time + TRUCK_DRIVE_TIME;
            return true;
        }

        case E_CRUSHER:
        {
            if (n->done_at != -1) return false;
            double tons = mass_tons(&n->mass);
            if (tons <= 0.0) return false;

            double rate = g_w.uniform(CRUSHER_MIN_RATE, CRUSHER_MAX_RATE);
            mass_take(&n->wip, &n->mass, tons); // campaign eats all stocked feed
            n->done_at = g_w.time + (i64)std::llround(tons / rate * HOUR);
            return true;
        }

        case E_FLOTATION:
        {
            if (n->done_at != -1) return false;
            double tons = mass_tons(&n->mass);
            if (tons <= 0.0) return false;

            double cap = g_w.triangular(FLOTATION_MIN_CAPACITY,
                                        FLOTATION_MID_CAPACITY,
                                        FLOTATION_MAX_CAPACITY);
            mass_take(&n->wip, &n->mass, tons); // campaign eats all stocked feed
            n->done_at = g_w.time + (i64)std::llround(tons / cap * HOUR);
            return true;
        }

        case E_CONVEYOR:
        {
            T_Node* src = &g_nodes[n->input];

            b32 blocked = dst->is_mass_unchecked;
            if(blocked) return false;

            i64 head = n->belt_head/n->belt_size;
            n->belt[head] = n->mass_unchecked;
            n->mass_unchecked = T_Mass();

            n->done_at = g_w.time + BELT_TICK;
            return true;
        }

        default:
            return false;
    }
}

static b32 clear(T_ID id) {
    T_Node* n = &g_nodes[id];

    n->mass_unchecked = T_Mass();
    n->is_mass_unchecked = false;
}

int main(void) {
    //      miner ──truck──> crusher ──belt──> flotation ──rejects──> dam
    T_Node* miner   = init_node(E_MINER, 0);
    T_Node* truck   = init_node(E_TRUCK, 0);
    T_Node* crusher = init_node(E_CRUSHER, 0);
    T_Node* belt    = init_node(E_CONVEYOR, 0);
    T_Node* flot    = init_node(E_FLOTATION, 0);
    T_Node* dam     = init_node(E_TAILINGS, 0);
    // The additive source stays unbuilt/unscheduled for now: its step case
    // exists, but nothing consumes g_additives yet.

    link_nodes(miner->id,   truck->id);
    link_nodes(truck->id,   crusher->id);
    link_nodes(crusher->id, belt->id);
    link_nodes(belt->id,    flot->id);
    link_nodes(flot->id,    dam->id); // reject stream; product waits in flot->out

    // Prime the only exogenous clock; everything else wakes via the sweep.
    miner->done_at = (i64)g_w.exponential(ARRIVAL_MEAN);

    i64 next_report = 30 * DAY;
    while (g_w.time < g_w.END) {
        // 1) advance to the earliest due event
        int next = -1;
        i64 best = 0;
        for (int i = 0; i < g_node_count; i++) {
            if (g_nodes[i].done_at == -1) continue;
            if (next == -1 || g_nodes[i].done_at < best) {
                best = g_nodes[i].done_at;
                next = i;
            }
        }
        if (next == -1) {
            std::cout << "[engine] stalled: no events scheduled at t = "
                      << g_w.time / DAY << " d\n";
            break;
        }

        g_w.time = best;
        for (int i=0; i<g_send_node_count; i++) {
            send(g_send_nodes[i]);
        }

        b32 changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < g_receive_node_count; i++)
                if (pull(g_receive_nodes[i])) changed = true;
        }

        for (int i = 0; i < g_node_count; i++) {
            clear(i);
        }

        // 3) periodic report
        if (g_w.time >= next_report) {
            next_report += 30 * DAY;
            std::cout << "day " << g_w.time / DAY
                      << "  pad "    << mass_tons(&miner->mass)
                      << "  crush "  << mass_tons(&crusher->mass) << "/" << mass_tons(&crusher->out)
                      << "  belt "   << [belt]{ double s = 0; for (int i = 0; i < BELT_CELLS; i++) s += mass_tons(&belt->belt[i]); return s; }()
                      << "  flot "   << mass_tons(&flot->mass) << "/" << mass_tons(&flot->out)
                      << "  dam "    << mass_tons(&dam->mass)
                      << "  add "    << mass_tons(&g_additives)
                      << "\n";
        }
    }

    std::cout << "done at day " << g_w.time / DAY << "\n";
    return 0;
}
