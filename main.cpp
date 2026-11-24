/*
    #####################################
    #### Real world simulation match ####
*/

// TODO(Sigull): Can there be two transactions which depend on each other.
// Is that even an issue?

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <iostream>
#include <algorithm>
#include <cstring>

#define i64 int64_t
#define u64 uint64_t
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

void assert(bool cond, const char* why) {
    if(!cond) {
        std::cout << "[Assert]: " << why;
        exit(1);
    }
}

void debug_assert(bool cond, const char* why) {
    if(!cond) {
        std::cout << "[Assert]: " << why;
        exit(1);
    }
}

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

enum E_Node_Type : u64 {
    N_MINER,
    N_TRANSFER,     // Can also split based on rules.
    N_ADDITIVE_SRC, // imported additives (e.g. Dy oxide) — pools into g_additives
    N_TRUCK,
    N_CRUSHER,
    N_CONVEYOR,
    N_FLOTATION,
    N_TAILINGS,     // passive sink: the dam (never scheduled)
};

enum E_Truck_State : u64 {
    TRUCK_LOADING,   // parked at the mine, waiting for a full load
    TRUCK_HAULING,   // loaded, driving to the crusher
    TRUCK_RETURNING, // empty, driving back to the mine
};

typedef struct T_Node {
    T_ID id;
    E_Node_Type type;
    T_Ring_Buffer buf;

    T_ID input = -1;
    T_ID output = -1;

    i64 next_tick = -1; // next scheduled event; -1 = idle

    // Owned stock: feed-side holdup, material in processing, product side.
    T_Mass mass;
    T_Mass wip;
    T_Mass out;

    i64 mass_capacity = INT64_MAX;

    // E_TRUCK
    E_Truck_State truck_state = TRUCK_LOADING;

    // E_CONVEYOR
    T_Mass belt[BELT_CELLS];
    i64 belt_size = BELT_CELLS;
    i64 belt_head = 0;

    // E_TRANSFER
    i64 transfer_length = 0;

} T_Node;

enum E_Transaction_Type : u64 {
    T_MOVE = 1 << 0,
    T_VAR = 1 << 1,
    T_STATE = 1 << 2,
    T_TICK = 1 << 3, // Probably something with event planning.
};

// All have to succeed to accept transaction.
typedef struct T_Transaction {
    u64 type; // FLAGS
    b32 not_done = true; // Don't do transaction twice.

    // T_MOVE
    T_Mass* mass_src;
    T_Mass* mass_dst;
    T_Node* dst;
    double amt;

    // T_VAR
    i64* var;
    i64 new_var;

    // T_STATE
    E_Truck_State* state_var;
    E_Truck_State new_state;

    // T_TICK
    i64* tick_var;
    i64 timestamp;

} T_Transaction;

constexpr int MAX_TRANSACTIONS = 100;
T_Transaction g_transactions[MAX_TRANSACTIONS];
i64 g_transaction_count = 0;

T_Transaction* make_transaction(u64 type) {
    assert(g_transaction_count < MAX_TRANSACTIONS, "Increase transaction capacity.");
    
    T_Transaction* t = &g_transactions[g_transaction_count++];
    std::memset(t, 0, sizeof(T_Transaction));
    
    t->type = type;
    t->not_done = true;

    return t;
}

void trans_add_move(T_Transaction* t, T_Mass* mass_src, T_Mass* mass_dst, T_Node* dst, double amt) {
    debug_assert(t->type & T_MOVE, "Move 1");

    t->mass_src = mass_src;
    t->mass_dst = mass_dst;
    t->dst = dst;
    t->amt = amt;

    debug_assert(((size_t)mass_dst - (size_t)dst) < sizeof(T_Node), "Move 2");
}

void trans_add_var(T_Transaction* t, i64* var, i64 val) {
    debug_assert(t->type & T_VAR, "Var 1");

    t->var = var;
    t->new_var = val;
}

void trans_add_truck_state(T_Transaction* t, E_Truck_State* state_var, E_Truck_State new_state) {
    debug_assert(t->type & T_STATE, "State 1");

    t->state_var = state_var;
    t->new_state = new_state;
}

void trans_add_tick(T_Transaction* t, i64* tick_var, i64 timestamp) {
    debug_assert(timestamp > g_w.time, "Tick 1");

    t->tick_var = tick_var;
    t->timestamp = timestamp;
}

static void clear_transactions() {
    g_transaction_count = 0;
}

// !! have to be careful !!
// IF ANY PART OF A TRANSACTION FAILS, THE WHOLE THING FAILS
static b32 enact_transaction(T_ID id) {
    assert(id < g_transaction_count, "Transaction id out of bounds.");
    T_Transaction* t = &g_transactions[id];
    if(!t->not_done) return false;

    // Move some materials from one processor to another.
    if(t->type & T_MOVE) {
        T_Mass* dst = t->mass_dst;
        T_Mass* src = t->mass_src;
        T_Node* dst_node = t->dst;
        double want_amt = t->amt;

        double from_total = mass_tons(src);
        double to_total = mass_tons(dst);
        double to_space_left = dst_node->mass_capacity - to_total;

        // Don't do transaction at all
        if (to_space_left <= 0.0) return false;
        if (from_total <= 0.0) return false;

        if (want_amt > to_space_left) want_amt = to_space_left;
        if (want_amt > from_total)    want_amt = from_total;

        double from_frac = want_amt / from_total;
        double to_frac   = want_amt / to_total;
        for (int el = 0; el < N_ELEMS; el++) {
            dst->elem[el] += dst->elem[el] * to_frac;
            src->elem[el] -= src->elem[el] * from_frac;
        }
        dst->waste += dst->waste * to_frac;
        src->waste -= src->waste * from_frac;

    } else if(t->mass_dst || t->mass_src || t->dst || t->amt) {
        debug_assert(false, "Enact Move");
    }

    // Change variable value. 
    // (Might be outdated, now only for conveyor->head++)
    if(t->type & T_VAR) {
        *t->var = t->new_var;

    } else if(t->var || t->new_var) {
        debug_assert(false, "Enact Var");
    }

    // Change truck state.
    if(t->type & T_STATE) {
        *t->state_var = t->new_state;

    } else if(t->state_var || t->new_state) {
        debug_assert(false, "Enact State");
    }

    // Set next tick to a timestamp.
    if(t->type & T_TICK) {
        *t->tick_var = t->timestamp;

    } else if(t->tick_var || t->timestamp) {
        debug_assert(false, "Enact Tick");
    }

    t->not_done = false;
    return true;
}

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
    assert(g_node_count < MAX_NODE_COUNT, "Too many nodes. Increase MAX_NODE_COUNT.");

    T_Node* n = &g_nodes[g_node_count];
    n->id = g_node_count++;
    n->type = type;
    n->buf = init_ring_buf(buf_size);

    return n;
}

T_Node* init_transfer_node(i64 length) {
    T_Node* n = init_node(N_TRANSFER, 0);
    n->transfer_length = length;

    return n;
}

void link_nodes(T_ID from, T_ID to) {
    for(int i=0; i<g_send_node_count; i++) {
        assert(g_send_nodes[i] != from, "Send to only one node for now.");
    }
    g_send_nodes[g_send_node_count++] = from;

    for(int i=0; i<g_receive_node_count; i++) {
        assert(g_receive_nodes[i] != to, "Receive from only one node for now.");
    }
    g_receive_nodes[g_receive_node_count++] = to;

    T_Node* from_n = &g_nodes[from];
    T_Node* to_n   = &g_nodes[to];

    from_n->output = to_n->id;
    to_n->input    = from_n->id;
}

void tick(T_ID id) {
    T_Node* n = &g_nodes[id];
    if (n->next_tick != g_w.time) return;

}

// Fired when a node's scheduled event comes due.
b32 send(T_ID id) {
    assert(id < g_node_count && id >= 0, "Code problem. Send ID out of range.");
    T_Node* n = &g_nodes[id];

    assert(n->output != -1, "Mistake in initing g_send_nodes.");
    T_Node* dst = &g_nodes[n->output];
    // T_Node* src = &g_nodes[n->input];

    switch(n->type) {
        case N_MINER:
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

            n->next_tick = g_w.time + g_w.exponential(ARRIVAL_MEAN / speed);
        } break;


        case N_TRANSFER:
        {
            assert(n->input != -1, "Transfer has no input.");
            n->next_tick = g_w.time + n->transfer_length;
        } break;


        case N_ADDITIVE_SRC:
        {
            g_additives.elem[ADDITIVE_ELEM] += ADDITIVE_LOT_TONS;
            n->next_tick = g_w.time + g_w.exponential(ADDITIVE_ARRIVAL_MEAN);
        } break;


        case N_TRUCK:
        {
            if (n->truck_state == TRUCK_HAULING) {
                // Arrived at the crusher: dump the cargo into its feed stock.
                // (A real dump pocket can refuse when full — the truck then
                // waits parked right next to the crusher. That refusal needs a
                // stock capacity, which does not exist yet, so for now the
                // dump always succeeds.)
                T_Transaction* t = make_transaction(T_MOVE | T_STATE);
                trans_add_move(t, &n->mass, &dst->mass, dst, mass_tons(&n->mass));
                trans_add_truck_state(t, &n->truck_state, TRUCK_RETURNING);
                // trans_add_tick(t, &n->next_tick, g_w.time + TRUCK_DRIVE_TIME);

            } else {
                // Back at the mine, empty. Loading happens in the pull sweep.
                n->truck_state = TRUCK_LOADING;
                n->next_tick = -1;
                return true;
            }
        } break;


        case N_CRUSHER:
        {
            // Run done: crushed product moves to the out-stock the belt
            // meters from. The sweep starts the next run if stock remains.
            T_Transaction* t = make_transaction(T_MOVE);
            trans_add_move(t, &n->out, &dst->belt[n->belt_head], dst, mass_tons(&n->out));
        } break;


        case N_CONVEYOR:
        {
            i64 head = n->belt_head % n->belt_size;

            T_Transaction* t;
            if(mass_tons(&n->belt[head]) > 0.0) {
                t = make_transaction(T_MOVE | T_VAR);
                trans_add_move(t, &n->belt[head], dst->mass, dst, mass_tons(&n->belt[head]));
                trans_add_var(t, &n->belt_head, n->belt_head + 1);
            
            } else {
                t = make_transaction(T_VAR);
                trans_add_var(t, &n->belt_head, n->belt_head + 1);
            } 

            n->next_tick = g_w.time + BELT_TICK;
        } break;


        case N_FLOTATION:
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
            n->next_tick = -1;
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
        case N_TRUCK:
        {
            // Wait-for-full: depart only when the pad can fill the truck.
            if (n->truck_state != T_LOADING || n->next_tick != -1) return false;
            T_Node* pad = &g_nodes[n->input];
            if (mass_tons(&pad->mass) < TRUCK_CAPACITY) return false;

            mass_take(&n->mass, &pad->mass, TRUCK_CAPACITY);
            n->truck_state = T_HAULING;
            n->next_tick = g_w.time + TRUCK_DRIVE_TIME;
            return true;
        }

        case N_CRUSHER:
        {
            if (n->next_tick != -1) return false;
            double tons = mass_tons(&n->mass);
            if (tons <= 0.0) return false;

            double rate = g_w.uniform(CRUSHER_MIN_RATE, CRUSHER_MAX_RATE);
            mass_take(&n->wip, &n->mass, tons); // campaign eats all stocked feed
            n->next_tick = g_w.time + (i64)std::llround(tons / rate * HOUR);
            return true;
        }

        case N_FLOTATION:
        {
            if (n->next_tick != -1) return false;
            double tons = mass_tons(&n->mass);
            if (tons <= 0.0) return false;

            double cap = g_w.triangular(FLOTATION_MIN_CAPACITY,
                                        FLOTATION_MID_CAPACITY,
                                        FLOTATION_MAX_CAPACITY);
            mass_take(&n->wip, &n->mass, tons); // campaign eats all stocked feed
            n->next_tick = g_w.time + (i64)std::llround(tons / cap * HOUR);
            return true;
        }

        case N_CONVEYOR:
        {
            T_Node* src = &g_nodes[n->input];

            b32 blocked = dst->is_mass_unchecked;
            if(blocked) return false;

            i64 head = n->belt_head/n->belt_size;
            n->belt[head] = n->mass_unchecked;
            n->mass_unchecked = T_Mass();

            n->next_tick = g_w.time + BELT_TICK;
            return true;
        }

        default:
            return false;
    }
}

static void schedule_tick(T_ID id) {
    T_Node* n = &g_nodes[id];
}

int main(void) {
    //      miner ──truck──> crusher ──belt──> flotation ──rejects──> dam
    T_Node* miner   = init_node(N_MINER, 0);
    T_Node* truck   = init_node(N_TRUCK, 0);
    T_Node* crusher = init_node(N_CRUSHER, 0);
    T_Node* belt    = init_node(N_CONVEYOR, 0);
    T_Node* flot    = init_node(N_FLOTATION, 0);
    T_Node* dam     = init_node(N_TAILINGS, 0);
    // The additive source stays unbuilt/unscheduled for now: its step case
    // exists, but nothing consumes g_additives yet.

    link_nodes(miner->id,   truck->id);
    link_nodes(truck->id,   crusher->id);
    link_nodes(crusher->id, belt->id);
    link_nodes(belt->id,    flot->id);
    link_nodes(flot->id,    dam->id); // reject stream; product waits in flot->out

    // Prime the only exogenous clock; everything else wakes via the sweep.
    miner->next_tick = (i64)g_w.exponential(ARRIVAL_MEAN);

    i64 next_report = 30 * DAY;
    while (g_w.time < g_w.END) {
        clear_transactions();

        // 1) advance to the earliest due event
        int next = -1;
        i64 best = 0;
        for (int i = 0; i < g_node_count; i++) {
            if (g_nodes[i].next_tick == -1) continue;
            if (next == -1 || g_nodes[i].next_tick < best) {
                best = g_nodes[i].next_tick;
                next = i;
            }
        }
        if (next == -1) {
            std::cout << "[engine] stalled: no events scheduled at t = "
                      << g_w.time / DAY << " d\n";
            break;
        }

        g_w.time = best;
        for (int i=0; i<g_node_count; i++) {
            tick(i);
        }

        for (int i=0; i<g_send_node_count; i++) {
            send(g_send_nodes[i]);
        }

        b32 changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < g_transaction_count; i++)
                // TODO(Sigull): When finished program check if precedence really isn't necessary.
                // Maybe do it, by resending more again.
                changed |= enact_transaction(i); // If all remaining transactions don't do shit.
        }

        for (int i=0; i<g_node_count; i++) {
            schedule_tick(i);
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
