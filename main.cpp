/*
    #####################################
    #### Real world simulation match ####
*/

// TODO(Sigull): Can there be two transactions which depend on each other.
//               Is that even an issue?

// TODO(Sigull): My first time doing ECS. Should I pass by id more and by how much.

// NOTE(Sigull): Starting to think that a purely wakeup thing, than this tick thing, 
//               would have been easier. Idk.

// TODO(Sigull): Refactor into at least a little cpp. Like constructors and shit.
// TODO(Sigull): Refactor vars to the start of block etc.

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

// ###################### Miner params ################################
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


// ######################## Truck transfer #############################
// Truck transfer: discrete haulage mine → crusher. Loads whatever is on
// the pad, up to TRUCK_CAPACITY — a low pad means a partial load, the
// truck does not wait for a full one. Constant drive time, dumps into the
// crusher's feed stock on arrival.
constexpr double TRUCK_CAPACITY   = 100.0;    // t per load
constexpr i64    TRUCK_DRIVE_TIME = 1 * HOUR; // one way, loaded or empty


// ######################### Conveyor transfer #########################
// Conveyor transfer: granular in-plant flow crusher → flotation. The belt is
// a circular buffer of cells with distinct ends: every tick the discharge
// end dumps its cell into the downstream stock while the feed end
// simultaneously fills the cell behind it from upstream, then the belt
// advances one cell. A load rides BELT_CELLS - 1 ticks. No priming: the belt
// outputs whatever reaches the read head (first discharge one transit after
// the first fill) and keeps advancing while empty (no stop-start pulsing).
// A cell never takes more than BELT_CELL_CAP: if the read cell can't
// discharge (downstream full), the belt tops the cell up to cap and holds.
constexpr i64    BELT_TICK  = 5 * MIN;                         // one iteration
constexpr double BELT_RATE  = 100.0;                          // t/h
constexpr int    BELT_CELLS = 6;                              // τ = 30 min
constexpr double BELT_CELL_CAP  = BELT_RATE * BELT_TICK / (double)HOUR;
constexpr double BELT_CAPACITY  = BELT_CELL_CAP * BELT_CELLS; // in-flight holdup


// ########################### Crusher #################################
// Crusher: rate server — consumes its feed stock at capacity, product lands
// in its out-stock for the belt to meter out. Rates calibrated from the old
// U(1, 1.5) h per ~100 t batch cycle.
constexpr double CRUSHER_MIN_RATE = 100.0 / 1.5; // t/h
constexpr double CRUSHER_MAX_RATE = 100.0 / 1.0; // t/h


// ########################### Flotation ###############################
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
// recovered REO  elem·r     → out-stock (concentrate, grade setpoint)
// lost REO       elem·(1−r) → tailings dam
// conc mass = recovered/CONC_GRADE; the mass above pure REO is
// gangue reporting to the concentrate, the rest to the dam.
constexpr double FLOTATION_MIN_RECOVERY = 0.72;
constexpr double FLOTATION_MID_RECOVERY = 0.80;
constexpr double FLOTATION_MAX_RECOVERY = 0.88;


// ######################## Imported additives ########################
// Imported additives (Dy oxide for the magnet line): memoryless shipping
// gaps. Pools into its own blob — nothing in the ore chain consumes it yet,
// so the source node is built but not scheduled.
constexpr double ADDITIVE_LOT_TONS     = 10.0;        // t per delivery
constexpr double ADDITIVE_ARRIVAL_MEAN = 10.0 * DAY;  // ≈ 1 t/day
constexpr int    ADDITIVE_ELEM         = N_ELEMS - 1; // Dy


// ######################### Chemical supply ##########################
// Bulk H2SO4 for the (future) acid digestion, built FROM SCRATCH: the tank
// starts empty and the string starts at 0 cars — the contract bootstraps
// from the first scrum. Three nodes, one next_tick each — no mini-scheduler:
//
// - N_CHEM_SRC:           the supplier. Acid tonnage is a T_Mass blob on the
//                         node (mass.waste — no REO elements, tons() applies),
//                         tank size = mass_capacity. Runs a FIXED timetable —
//                         one delivery slot every CHEM_DELIVERY_PERIOD — and
//                         each slot unloads chem_cars x 85 t: a string of rail
//                         cars. The timetable is the railway's; the string
//                         length is the contract's (what the scrum flexes).
// - N_CHEM_MONTHLY_SCRUM: the S&OP loop. Reads the supplier's trailing burn
//                         and tank level, sets chem_cars = trailing burn per
//                         slot plus a deficit correction, rounded to WHOLE
//                         cars — the step is one 85 t car per weekly slot
//                         (~12 t/day) and the tank farm absorbs the rounding.
// - N_CHEM_DISRUPTION:    the market/logistics hazard. Onsets are a renewal
//                         process; each suspends the supplier's stream for a
//                         lognormal duration. The supplier slips its slot to
//                         recovery — lost volume is not made up.
//
// One delivery slot per week, ~3 cars per string at the design burn of
// ~33 t/day (85 t cars [W]). Disruptions (the 2022–23 acid squeeze) are
// what the tank farm and the scrum exist to absorb.
constexpr double CHEM_LOT_TONS         = 85.0;      // t per rail car
constexpr double CHEM_TANK_CAP         = 400.0;     // t, tank farm (mass_capacity)
constexpr double CHEM_TARGET           = 200.0;     // t, planning target (~6 d cover)
constexpr double CHEM_CATCHUP_DAYS     = 10.0;      // refill slope: must beat the ~12 t/day
                                                   // car step, or a deficit could never
                                                   // add a car (200/10 = 20 > 12)
constexpr i64    CHEM_DELIVERY_PERIOD  = 7 * DAY;   // fixed timetable: one slot per week
constexpr i64    CHEM_MAX_CARS         = 6;         // string length cap (~2.2x design burn)
constexpr i64    CHEM_SCRUM_PERIOD     = 30 * DAY;   // S&OP cadence
constexpr i64    CHEM_EVENT_MEAN       = 120 * DAY;  // mean gap between disruptions
constexpr i64    CHEM_DISRUPT_MEDIAN   = 6 * DAY;    // median disruption length
constexpr double CHEM_DISRUPT_SIGMA    = 0.6;       // lognormal sigma (right-skew)


// ############## Digest. Combination chemicals and flotation. ##############
constexpr double DIGEST_MIN_CAMPAIGN = 8; // [tons]. Min to start digest batch.
constexpr double DIGEST_MAX_CAMPAIGN = 40; // [tons]

constexpr double DIGEST_MAX_PLS = 30; // [tons]. Pregnant leach solution.

constexpr double DIGEST_RATE_MIN = 1.0;
constexpr double DIGEST_RATE_MID = 1.6;
constexpr double DIGEST_RATE_MAX = 2.4;

constexpr double DIGEST_A_MIN = 0.9;
constexpr double DIGEST_A_MID = 1.2;
constexpr double DIGEST_A_MAX = 1.5;

constexpr double DIGEST_R_MIN = 0.9;
constexpr double DIGEST_R_MID = 0.945;
constexpr double DIGEST_R_MAX = 0.98;


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

        i64 time = -1;

        // Chemical disruption
        i64 chem_disrupted_until = -1;
        double chem_burned = 0;

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



// One aggregate material mass — "the blob". elem[e] is the contained REO
// tonnage of element e; waste is the rest. Ore tons = Σ elem + waste; head
// grade is derivable: Σ elem / ore tons. No batch identity: a blob is a
// blend of everything that entered it, like a real stockpile.
class T_Mass {
    public:
        double elem[N_ELEMS] = {};
        double waste = 0;

        double max_tons = HUGE_VAL;

        double tons() {
            double t = this->waste;
            for (int el = 0; el < N_ELEMS; el++) t += this->elem[el];

            return t;
        }
};

class T_Chem_Mass {
    public:
        double chem = 0; // H_2SO_4

        double max_tons = HUGE_VAL;

        double tons() {
            return chem;
        }
};

enum E_Node_Type : u64 {
    N_MINER,
    N_CHEM_SRC, // Imported chemicals (acid for digestion) — the supplier.
    N_CHEM_DISRUPTION,      // market/logistics event suspending N_CHEM_SRC's stream
    N_CHEM_MONTHLY_SCRUM,   // S&OP: sets N_CHEM_SRC's contracted rate, wakes it
    N_ADDITIVE_SRC, // imported additives (e.g. Dy oxide) — pools into g_additives
    N_TRUCK,
    N_CRUSHER,
    N_CONVEYOR,
    N_PIPE,
    N_FLOTATION,
    N_DIGEST,
    N_TAILINGS,     // passive sink: the dam (never scheduled)
};

enum E_Processor_State : u64 {
    S_NULL = 0, // For nodes which don't use state.
    S_IDLE,
    S_FINISH,
};

enum E_Conveyor_State : u64 {
    C_MOVING,
    C_LOADING,
};

// TODO(Sigull): Maybe I am just too sleep deprived but these wrods are too hard. 
enum E_Truck_State : u64 {
    TRUCK_LOAD,
    TRUCK_HAUL,
    TRUCK_TRAVEL_TO_MINE,
    TRUCK_TRAVEL_TO_EXTRACTION,  
};

// TODO(Sigull): Need better node docs, like I did in IFJ project for AST.

// TODO(Sigull): Thinking about having an ordered queue for ticks. Not just next tick.
// That was my original idea, but AI did it differently and I kept it.
typedef struct T_Node {
    T_ID id = -1;
    E_Node_Type type;

    T_ID input = -1;
    T_ID output = -1;
    T_ID waste_output = -1; // Used for example in in N_FLOTATION to connect dam.

    i64 next_tick = -1; // next scheduled event; -1 = idle

    // Owned stock: feed-side holdup, material in processing, product side.
    T_Mass mass;
    T_Mass wip;
    T_Mass out;

    T_Chem_Mass chem_mass; // When gets used, it is just burned.

    // Cap of a destination stock on this node, checked against the dst
    // T_Mass' live content at enactment time. NOTE for N_CONVEYOR: moves
    // into the belt target individual cells, so for the belt this is the
    // PER-CELL cap (set to BELT_CELL_CAP in main).
    double mass_capacity = HUGE_VAL;


    // N_TRUCK
    E_Truck_State truck_state = TRUCK_LOAD;

    // N_CONVEYOR
    E_Conveyor_State conv_state = C_LOADING;
    T_Mass belt[BELT_CELLS];
    i64 belt_size = BELT_CELLS;
    i64 belt_head = 1;
    
    // N_PIPE
    T_Chem_Mass belt_chem[BELT_CELLS];

    // N_TRANSFER
    i64 transfer_length = 0;
    
    // N_FLOTATION && N_CRUSHER
    E_Processor_State proc_state = S_NULL;

    // N_CHEM_SRC — the acid tonnage itself is the node's T_Mass blob
    i64 chem_cars = 0; // cars per delivery string (set by the scrum)
    T_ID owned_chem_scrum = -1; // N_CHEM_MONTHLY_SCRUM 

    // N_CHEM_MONTHLY_SCRUM
    double chem_burned_at_scrum = 0; // chem_burned snapshot at the last scrum
    i64    chem_last_scrum      = 0; // when the last scrum happened

    // N_DIGEST
    T_ID chem_input = -1; // digest has chem and ore input.

} T_Node;



enum E_Transaction_Type : u64 {
    T_MOVE = 1 << 0,
    T_HEAD_INCREMENT = 1 << 1, // Increment head on conveyor
    T_STATE = 1 << 2,
    T_TICK = 1 << 3, // Probably something with event planning.
    T_MOVE_CHEM = 1 << 4,
};

// ALL PASSED IN ONE TRANSACTION HAVE SUCCEED TO HAPPEN. ATOMIC.
typedef struct T_Transaction {
    u64 type; // FLAGS
    b32 not_done = true; // Don't do transaction twice.

    // T_MOVE
    T_Mass* mass_src;
    T_Mass* mass_dst;
    T_Node* src;
    T_Node* dst;
    double amt;

    // T_MOVE_CHEM
    T_Chem_Mass* chem_mass_src;
    T_Chem_Mass* chem_mass_dst;
    T_Node* chem_src;
    T_Node* chem_dst;
    double chem_amt;

    // T_HEAD_INCREMENT
    T_Node* conveyor;
    
    // T_STATE
    E_Conveyor_State* conv_state;
    E_Conveyor_State new_conv_state;

    E_Truck_State* truck_state;
    E_Truck_State new_truck_state;

    // T_TICK
    i64* tick_var;
    i64 timestamp;

} T_Transaction;


C_World g_w;

constexpr int MAX_NODE_COUNT = 100;
T_Node g_nodes[MAX_NODE_COUNT];
int g_node_count = 0;

// Memory helpers
T_ID g_transfer_nodes[MAX_NODE_COUNT];
int g_transfer_node_count = 0;

constexpr int MAX_TRANSACTIONS = 100;
T_Transaction g_transactions[MAX_TRANSACTIONS];
i64 g_transaction_count = 0;

// Imported additive stock — its own blob, outside the ore chain (see params).
T_Mass g_additives;

T_Transaction* make_transaction(u64 type) {
    assert(g_transaction_count < MAX_TRANSACTIONS, "Increase transaction capacity.");
    // Check enact_transaction comment for reason for these asserts.
    // They can be implemented, just not worth it for now.
    assert(!(type & T_HEAD_INCREMENT) || !(type & T_MOVE), 
            "Can't have move and head increment in single transaction.");
    assert(!(type & T_HEAD_INCREMENT) || !(type & T_MOVE_CHEM), 
            "Can't have move and head increment in single transaction.");
    assert(!(type & T_MOVE_CHEM) || !(type & T_MOVE), 
            "Can't move chem and materials in the same transaction.");

    T_Transaction* t = &g_transactions[g_transaction_count++];
    *t = T_Transaction();
    
    t->type = type;
    t->not_done = true;

    return t;
}

T_Transaction* get_transaction(T_ID id) {
    assert(id < g_transaction_count, "Transaction id out of bounds.");
    
    T_Transaction* t = &g_transactions[id];

    assert(!(t->type & T_HEAD_INCREMENT) || !(t->type & T_MOVE), 
            "Can't have move and head increment in single transaction.");
    
    if (!t->not_done) {
        return NULL;
    }

    return t;
}

void trans_add_move(T_Transaction* t, T_Mass* mass_src, T_Node* src, T_Mass* mass_dst, T_Node* dst, double amt) {
    debug_assert(t->type & T_MOVE, "Move 1");
    assert(((size_t)mass_src - (size_t)src) < sizeof(T_Node), "add_move: src mass not in src");
    assert(((size_t)mass_dst - (size_t)dst) < sizeof(T_Node), "add_move: dst mass not in dst");

    t->mass_src = mass_src;
    t->mass_dst = mass_dst;
    t->src = src;
    t->dst = dst;

    t->amt = amt;
}

void trans_add_move_chem(T_Transaction* t, T_Chem_Mass* chem_src, T_Node* src, T_Chem_Mass* chem_dst, T_Node* dst, double amt) {
    debug_assert(t->type & T_MOVE_CHEM, "Chem 1");
    assert(((size_t)chem_src - (size_t)src) < sizeof(T_Node), "add_move: src mass not in src");
    assert(((size_t)chem_dst - (size_t)dst) < sizeof(T_Node), "add_move: dst mass not in dst");

    t->chem_mass_src = chem_src;
    t->chem_mass_dst = chem_dst;
    t->chem_src = src;
    t->chem_dst = dst;

    t->chem_amt = amt;
}

void trans_add_head(T_Transaction* t, T_Node* conveyor) {
    debug_assert(t->type & T_HEAD_INCREMENT, "Var 1");

    assert(conveyor->type == N_CONVEYOR || 
           conveyor->type == N_PIPE, "trans_add_head passed param is not conveyor.");

    t->conveyor = conveyor;
}

void trans_add_truck_state(T_Transaction* t, E_Truck_State* state_var, E_Truck_State new_state) {
    debug_assert(t->type & T_STATE, "State 1");

    t->truck_state = state_var;
    t->new_truck_state = new_state;
}

void trans_add_conveyor_state(T_Transaction* t, E_Conveyor_State* state_var, E_Conveyor_State new_state) {
    debug_assert(t->type & T_STATE, "State 1");

    t->conv_state = state_var;
    t->new_conv_state = new_state;
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
//
// Move and Head_Increment can't be together, because Atomicity stuff.
// Both can fail.
//
// return true only at the end
static b32 enact_transaction(T_ID id) {
    T_Transaction* t = get_transaction(id);
    if (t == NULL) return false; // Already successfully processed before.

    // MOVE BLOB OF MATERIALS BETWEEN NODES.
    if(t->type & T_MOVE) {
        T_Mass* dst = t->mass_dst;
        T_Mass* src = t->mass_src;
        T_Node* dst_node = t->dst;
        double want_amt = t->amt;

        double from_total = src->tons();
        double to_total = dst->tons();
        double to_space_left = dst_node->mass_capacity - to_total;

        // Don't do transaction at all
        if (to_space_left <= 0.0) return false;
        if (from_total <= 0.0) return false;

        if (want_amt > to_space_left) want_amt = to_space_left;
        if (want_amt > from_total)    want_amt = from_total;

        // Blend by the source's composition: the tons landing in dst are a
        // proportional slice of src, so per-element tonnage is conserved.
        // (dst's own composition only describes what's already there — a
        // fresh-empty dst is fine, no to_frac / to_total division.)
        double from_frac = want_amt / from_total;
        for (int el = 0; el < N_ELEMS; el++) {
            double moved = src->elem[el] * from_frac;
            dst->elem[el] += moved;
            src->elem[el] -= moved;
        }
        double moved_waste = src->waste * from_frac;
        dst->waste += moved_waste;
        src->waste -= moved_waste;

    } else if(t->mass_dst || t->mass_src || t->dst || t->amt) {
        debug_assert(false, "Uninitialized enact Move");
    }

    if(t->type & T_MOVE_CHEM) {
        T_Chem_Mass* dst = t->chem_mass_dst;
        T_Chem_Mass* src = t->chem_mass_src;
        T_Node* dst_node = t->chem_dst;
        double want_amt = t->chem_amt;

        double from_total = src->tons();
        double to_total = dst->tons();
        double to_space_left = dst_node->mass_capacity - to_total;

        // Don't do transaction at all
        if (to_space_left <= 0.0) return false;
        if (from_total <= 0.0) return false;

        if (want_amt > to_space_left) want_amt = to_space_left;
        if (want_amt > from_total)    want_amt = from_total;

        double from_frac = want_amt / from_total;
        double moved = src->chem * from_frac;
        dst->chem += moved;
        src->chem -= moved;

    } else if(t->chem_mass_dst || t->chem_mass_src || t->chem_dst || t->chem_amt) {
        debug_assert(false, "Uninitialized enact Move (chem)");
    }

    // INCREMENT CONVEYOR HEAD. 
    if(t->type & T_HEAD_INCREMENT) {
        T_Node* c = t->conveyor;
        i64 head = c->belt_head % c->belt_size;
        double head_value = HUGE_VAL;
        if (c->type == N_CONVEYOR) {
            head_value = c->belt[head].tons();
        
        } else if (c->type == N_PIPE) {
            head_value = c->belt_chem[head].tons();
        
        } else {
            assert(false, "Head increment.");
        }


        if (head_value <= 0.0) {
            c->belt_head++;
        } else {
            return false;
        }

    } else if(t->conveyor) {
        debug_assert(false, "Uninitialized enact Head");
    }

    // CHANGE (TRUCK | CONV) = TRANSFERs STATE
    if(t->type & T_STATE) {
        assert(t->truck_state == NULL || t->conv_state == NULL, 
            "Can't change state on truck and conv in the same transaction.");

        if (t->truck_state != NULL) {
            *t->truck_state = t->new_truck_state;
        
        } else if (t->conv_state != NULL) {
            *t->conv_state = t->new_conv_state;

        } else {
            assert(false, "State change improperly init.");
        }

    } else if(t->truck_state || t->new_truck_state) {
        debug_assert(false, "Uninitialized enact State");
    }

    // SET TICK
    // NOTE(Sigull): Can rewrite previous tick. Not deterministic.
    if(t->type & T_TICK) {
        *t->tick_var = t->timestamp;

    } else if(t->tick_var || t->timestamp) {
        debug_assert(false, "Uninitialized enact Tick");
    }


    // WAKE UP SRC AND DST OF MOVE IF THEY ARE ASLEEP.
    // Kind of a hack. Wake up floation tank when move and tank is idle.
    // Reference comment in N_FLOATION in tick().
    if (t->type & T_MOVE || t->type & T_MOVE_CHEM) {
        T_Node* dst = t->type & T_MOVE ? t->dst : t->chem_dst;
        T_Node* src = t->type & T_MOVE ? t->src : t->chem_src;
        
        // DST
        // proc_state only on nodes, which can be woken up.
        bool proc_state_idle = dst->proc_state == S_IDLE;
        bool is_tick_in_past = dst->next_tick < g_w.time;
        bool not_transfer = dst->type != N_TRUCK && dst->type != N_CONVEYOR && dst->type != N_PIPE;

        if (proc_state_idle && is_tick_in_past && not_transfer) {
            dst->next_tick = g_w.time + 5 * MIN;
        }

        // SRC
        is_tick_in_past = src->next_tick < g_w.time;
        not_transfer = src->type != N_TRUCK && src->type != N_CONVEYOR && src->type != N_PIPE;

        if (is_tick_in_past && not_transfer) {
            src->next_tick = g_w.time + 5 * MIN;
        }
    }

    t->not_done = false;
    return true;
}

// T_Node* find_node(E_Node_Type type) {
//     for (int i = 0; i < g_node_count; i++) {
//         if (g_nodes[i].type == type) return &g_nodes[i];
//     }

//     assert(false, "Node of this type not found.");
//     return NULL;
// }

// TODO(Sigull): This should have a better abstraction, but don't know what yet.
void link_nodes(T_Node* from, T_Node* to) {
    assert(from->output == -1, "From output already linked.");
    assert(to->input    == -1, "To input already linked.");

    from->output = to->id;
    to->input    = from->id;
}

void link_waste(T_Node* from, T_Node* to) {
    assert(from->waste_output == -1, "Waste from output already linked.");
    assert(to->input    == -1, "Waste to input already linked.");

    from->waste_output = to->id;
    to->input          = from->id;
}

void link_chems(T_Node* from, T_Node* to) {
    assert(from->output   == -1, "Chem from output already set.");
    assert(to->chem_input == -1, "Chem to input already linked.");

    from->output   = to->id;
    to->chem_input = from->id;

    if (to->type == N_PIPE) {
        to->input = from->id;
    }
}

T_Node* init_node(E_Node_Type type) {
    assert(g_node_count < MAX_NODE_COUNT, "Too many nodes. Increase MAX_NODE_COUNT.");

    T_Node* n = &g_nodes[g_node_count];

    n->id = g_node_count++;
    n->type = type;
    n->next_tick = -1;
    
    if (type == N_MINER) {
        n->next_tick = (i64)g_w.exponential(ARRIVAL_MEAN);
    }

    if (type == N_CHEM_DISRUPTION) {
        n->next_tick = (i64)g_w.exponential(CHEM_EVENT_MEAN);
    }

    // Transfer nodes
    if (type == N_CONVEYOR || type == N_TRUCK || type == N_PIPE) {
        g_transfer_nodes[g_transfer_node_count++] = n->id;
    }

    if (type == N_CONVEYOR || type == N_PIPE) {
        n->mass_capacity = BELT_CELL_CAP;
    }

    // Nodes with state, which can be woken up by getting a transfer.
    if (type == N_CRUSHER || type == N_FLOTATION || type == N_DIGEST) {
        n->proc_state = S_IDLE;
    }

    // The chem SUBSYSTEM constructor: creating the supplier builds its
    // whole chain — the delivery pipe, the S&OP scrum and the market
    // disruption clock — wired statically right here. The supplier's output
    // goes to the pipe; the scrum reads the supplier directly, so it must
    // NOT go through link_nodes (that would need the output again). The
    // pipe's far end dangles until a digest links itself onto it (see
    // N_DIGEST) — until then the pipe idles.
    if (type == N_CHEM_SRC) {
        n->mass_capacity = CHEM_TANK_CAP;
        n->next_tick = 1 * DAY;

        T_Node* scrum = init_node(N_CHEM_MONTHLY_SCRUM);
        scrum->input = n->id; // the scrum reads the supplier's tank + burn

        n->owned_chem_scrum = scrum->id;

        init_node(N_CHEM_DISRUPTION); // free-floating clock, writes the global
    }

    if (type == N_DIGEST) {
        n->mass_capacity = DIGEST_MAX_PLS;
        n->next_tick = 1 * DAY;
    }

    if (type == N_CHEM_MONTHLY_SCRUM) {
        n->next_tick = 1 * DAY;
    }

    return n;
}

static void inner_node_mass_move(T_Node* n, T_Mass* src, T_Mass* dst, double max_amt) {
    assert(((size_t)src - (size_t)n) < sizeof(T_Node), "Supposedly inner src mass not inside of node.");
    assert(((size_t)dst - (size_t)n) < sizeof(T_Node), "Supposedly inner dst mass not inside of node.");
    
    double total     = src->tons();
    double dst_space = dst->max_tons - dst->tons(); 
    if (max_amt > total)     max_amt = total;
    if (dst_space < max_amt) max_amt = dst_space;
    if (max_amt <= 0.0)      return;

    double frac = max_amt / total;
    for (int el = 0; el < N_ELEMS; el++) {
        double moved = src->elem[el] * frac;
        dst->elem[el] += moved;
        src->elem[el] -= moved;
    }
    double moved_waste = src->waste * frac;
    dst->waste += moved_waste;
    src->waste -= moved_waste;
}

// Contained REO — the PLS currency (everything the digest dissolves; the
// gangue reporting to the residue is not part of it).
static double reo_tons(const T_Mass* m) {
    double t = 0;
    for (int el = 0; el < N_ELEMS; el++) t += m->elem[el];
    return t;
}

T_Node* get_node(i64 id) {
    assert(id < g_node_count && id >= 0, "Code problem. Send ID out of range.");

    return &g_nodes[id];
}

void tick(T_ID id) {
    T_Node* n = get_node(id);
    if (n->next_tick != g_w.time) return;

     switch(n->type) {
        case N_MINER:
        {
            // Total amount of raw ore received
            double tons = g_w.uniform(TONS_IN_BASKET_MIN, TONS_IN_BASKET_MAX);
            
            // Grade of received ore. Changes over time.
            static double grade_state = GRADE_MEAN;
            grade_state = GRADE_MEAN
                        + GRADE_PHI * (grade_state - GRADE_MEAN)
                        + g_w.normal_distribution(0.0, GRADE_INNOV_SD);

            if (grade_state < GRADE_MIN) grade_state = GRADE_MIN;
            if (grade_state > GRADE_MAX) grade_state = GRADE_MAX;

            // Get REO from grade_state and load it into mass.
            double reo = 0;
            for (int el = 0; el < N_ELEMS; el++) {
                double add = tons * grade_state * BASKET[el];
                n->mass.elem[el] += add;
                reo += add;
            }
            n->mass.waste += tons - reo;

            // Mine throttles against its own stockpile: pad near target runs at
            // nominal pace, a full pad slows the mine, a dry pad speeds it up.
            double stock = n->mass.tons();
            double speed = std::clamp(STOCK_TARGET / stock, MINE_SPEED_MIN, MINE_SPEED_MAX);

            // TODO(Sigull): Maybe add slower modulation, but like this is overcooking.
            n->next_tick = g_w.time + g_w.exponential(ARRIVAL_MEAN / speed);
        } break;

        
        case N_CHEM_DISRUPTION:
        {
            double len = (double)CHEM_DISRUPT_MEDIAN *
                         std::exp(CHEM_DISRUPT_SIGMA * g_w.normal_distribution(0.0, 1.0));
            g_w.chem_disrupted_until = g_w.time + (i64)len;

            n->next_tick = g_w.time + (i64)g_w.exponential(CHEM_EVENT_MEAN);
        } break;

        
        case N_CHEM_MONTHLY_SCRUM:
        {
            T_Node* src = get_node(n->input);

            double window = std::max(1.0, (double)(g_w.time - n->chem_last_scrum));
            double burn = (g_w.chem_burned - n->chem_burned_at_scrum) * DAY / window;
            double want = burn + (CHEM_TARGET - src->chem_mass.chem) / CHEM_CATCHUP_DAYS;

            i64 cars = (i64)std::llround(want * ((double)CHEM_DELIVERY_PERIOD / DAY) / CHEM_LOT_TONS);
            i64 lo   = (want > 0.0) ? 1 : 0; // a live contract ships at least one car
            src->chem_cars = std::clamp(cars, lo, CHEM_MAX_CARS);

            n->chem_burned_at_scrum = g_w.chem_burned;
            n->chem_last_scrum = g_w.time;
            n->next_tick = g_w.time + CHEM_SCRUM_PERIOD;
        } break;


        case N_CHEM_SRC:
        {
            if (g_w.time < g_w.chem_disrupted_until) {
                n->next_tick = g_w.chem_disrupted_until;
                break;
            }

            if (n->chem_cars <= 0) {
                n->next_tick = g_w.time + CHEM_DELIVERY_PERIOD;
                break;
            }

            double tons = n->chem_cars * CHEM_LOT_TONS;
            if (n->chem_mass.chem + tons > n->mass_capacity) {
                // TODO(Sigull): AI handled this like fking shit.
                n->next_tick = g_w.time + CHEM_DELIVERY_PERIOD;
                break;
            }

            n->chem_mass.chem += tons;
            n->next_tick = g_w.time + CHEM_DELIVERY_PERIOD;
        } break;


        case N_DIGEST:
        {
            if (n->proc_state == S_FINISH) {
                if (n->out.tons() + reo_tons(&n->wip) <= n->mass_capacity) {
                    double r = g_w.triangular(DIGEST_R_MIN, DIGEST_R_MID, DIGEST_R_MAX);

                    T_Node* pond = get_node(n->waste_output);
                    for (int el = 0; el < N_ELEMS; el++) {
                        pond->mass.elem[el] += n->wip.elem[el] * (1.0 - r);
                        n->wip.elem[el]     *= r;
                    }

                    pond->mass.waste += n->wip.waste;
                    n->wip.waste = 0;

                    inner_node_mass_move(n, &n->wip, &n->out, HUGE_VAL);

                    n->proc_state = S_IDLE;
                    n->next_tick = g_w.time + 5 * MIN;
                }

            } else if (n->proc_state == S_IDLE) {
                double feed = n->mass.tons();

                double a = g_w.triangular(DIGEST_A_MIN, DIGEST_A_MID, DIGEST_A_MAX);
                double tons = std::min(std::min(feed, DIGEST_MAX_CAMPAIGN), n->chem_mass.tons() / a);

                // Proceed to batch and digest.
                if (feed >= DIGEST_MIN_CAMPAIGN && tons >= DIGEST_MIN_CAMPAIGN) {
                    inner_node_mass_move(n, &n->mass, &n->wip, tons);
                    n->chem_mass.chem -= a * tons;
                    // The burn bookkeeping lives on the supplier — that is
                    // what the scrum reads for its trailing-burn term.
                    g_w.chem_burned += a * tons;

                    double rate = g_w.triangular(DIGEST_RATE_MIN, DIGEST_RATE_MID, DIGEST_RATE_MAX);
                    n->proc_state = S_FINISH;
                    n->next_tick = g_w.time + (i64)std::llround(tons / rate * HOUR);
                }
            }
        } break;


        case N_ADDITIVE_SRC:
        {
            g_additives.elem[ADDITIVE_ELEM] += ADDITIVE_LOT_TONS;
            n->next_tick = g_w.time + g_w.exponential(ADDITIVE_ARRIVAL_MEAN);
        } break;


        case N_TRUCK:
        {            
            if (n->truck_state == TRUCK_TRAVEL_TO_EXTRACTION) {
                n->truck_state = TRUCK_HAUL;

            } else if (n->truck_state == TRUCK_TRAVEL_TO_MINE) {
                n->truck_state = TRUCK_LOAD;
            
            } else {
                assert(false, "Truck bad tick.");
            }
        } break;


        case N_CRUSHER:
        {
            if (n->proc_state == S_FINISH) {
                if (n->out.tons() + n->wip.tons() < n->mass_capacity) {
                    inner_node_mass_move(n, &n->wip, &n->out, HUGE_VAL);

                    n->proc_state = S_IDLE;
                    n->next_tick = g_w.time + 5 * MIN;
                }

            } else if (n->proc_state == S_IDLE) {
                double tons = n->mass.tons();
                if (tons > 0.0) {
                    double rate = g_w.uniform(CRUSHER_MIN_RATE, CRUSHER_MAX_RATE);
                    inner_node_mass_move(n, &n->mass, &n->wip, HUGE_VAL);
        
                    n->proc_state = S_FINISH;
                    n->next_tick = g_w.time + (i64)std::llround(tons / rate * HOUR);
                }
            } 

        } break;


        case N_CONVEYOR:
        {
            if (n->conv_state != C_MOVING) {
                assert(false, "Bad conveyor tick");
            }

            n->conv_state = C_LOADING;
        } break;


        case N_PIPE:
        {
            if (n->conv_state != C_MOVING) {
                assert(false, "Bad pipe tick");
            }

            n->conv_state = C_LOADING;
        } break;


        // TODO(Sigull): This whole code can't really map to real life.
        //               Also I remember it should have a failure state.
        
        // TODO(Sigull): Has no capacity limit on output.
        case N_FLOTATION:
        {
            // CASE S_FINISH
            if (n->proc_state == S_FINISH) {
                double r = g_w.triangular(FLOTATION_MIN_RECOVERY,
                                    FLOTATION_MID_RECOVERY,
                                    FLOTATION_MAX_RECOVERY);

                T_Node* dam = get_node(n->waste_output);
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

                inner_node_mass_move(n, &n->wip, &n->out, HUGE_VAL);

                // TODO(Sigull): How long does this take irl.
                n->proc_state = S_IDLE;
                n->next_tick = g_w.time + 5 * MIN;
            
            // CASE S_IDLE
            } else if(n->proc_state == S_IDLE) {
                double tons = n->mass.tons();
                
                // NOTE(Sigull):
                //   If it can't start now. It will be woken up with tick from
                //   enact_transaction. Not great, I know. Don't want to think.
                if (tons <= 0.0) break;

                double cap = g_w.triangular(FLOTATION_MIN_CAPACITY,
                                            FLOTATION_MID_CAPACITY,
                                            FLOTATION_MAX_CAPACITY);

                inner_node_mass_move(n, &n->mass, &n->wip, HUGE_VAL);
                n->proc_state = S_FINISH;
                n->next_tick = g_w.time + (i64)((tons / cap) * HOUR);

            // DEFAULT
            } else {
                assert(false, "Unhandled/Wrong floation state.");
            }
        } break;


        default:
        {
            assert(false, "Unhandled/Wrong node type.");
        } break;
    }
}

// Send and receive. Mostly for Transfer processors.
// NOTE(Sigull): Two transferers after each other probably not handled well.
b32 transfer(T_ID id) {
    T_Node* n = get_node(id);

    // The chem pipe is built eagerly with its supplier and only gets its far
    // end when a digest links itself onto it — until then there is nobody to
    // feed and the pipe idles.
    if (n->type == N_PIPE && n->output == -1) return true;

    assert(n->output != -1, "Mistake in initing g_send_nodes.");
    T_Node* dst = get_node(n->output);
    T_Node* src = get_node(n->input);

    switch(n->type) {
        // Maybe better to have a multiway wake up model.
        // NOTE(Sigull): Yep, can't keep my eyes open. See the mistakes,
        //               but I just can't get a hold of them. It's like 
        //               the thought immediately after it was seen falls
        //               into a dark bottomless pit behind and trying to 
        //               get it back is warping the space to throw the 
        //               from the top back into the mind. And I can't meow.
        case N_TRUCK:
        {
            if (n->truck_state == TRUCK_HAUL) {
                T_Transaction* t = make_transaction(T_MOVE | T_STATE | T_TICK);
                trans_add_move(t, &n->mass, n, &dst->mass, dst, n->mass.tons());
                trans_add_truck_state(t, &n->truck_state, TRUCK_TRAVEL_TO_MINE);
                
                trans_add_tick(t, &n->next_tick, g_w.time + TRUCK_DRIVE_TIME);

            } else if(n->truck_state == TRUCK_LOAD) {
                T_Transaction* t = make_transaction(T_MOVE | T_STATE | T_TICK);
                trans_add_move(t, &src->mass, src, &n->mass, n, TRUCK_CAPACITY);
                trans_add_truck_state(t, &n->truck_state, TRUCK_TRAVEL_TO_EXTRACTION);

                trans_add_tick(t, &n->next_tick, g_w.time + TRUCK_DRIVE_TIME);
            }
        } break;


        case N_CONVEYOR:
        {
            if (n->conv_state != C_MOVING){
                // Sending
                i64 read_head  = n->belt_head % n->belt_size;
                T_Transaction* t1;
                t1 = make_transaction(T_MOVE);
                trans_add_move(t1, &n->belt[read_head], n, &dst->mass, dst, n->belt[read_head].tons());
    
                // Receiving
                i64 write_head = (n->belt_head - 1) % n->belt_size;
                T_Transaction* t2 = make_transaction(T_MOVE);
                trans_add_move(t2, &src->out, src, &n->belt[write_head], n, BELT_CELL_CAP);
    
                // Increment head
                T_Transaction* t3 = make_transaction(T_HEAD_INCREMENT | T_STATE | T_TICK);
                trans_add_head(t3, n); // Takes care of the fact, that it can only increment when at read head 0 tons.
                trans_add_tick(t3, &n->next_tick, g_w.time + BELT_TICK);
                trans_add_conveyor_state(t3, &n->conv_state, C_MOVING);
                //#############################
            }
        } break;


        case N_PIPE:
        {
            if (n->conv_state != C_MOVING){
                // Sending
                i64 read_head  = n->belt_head % n->belt_size;
                T_Transaction* t1;
                t1 = make_transaction(T_MOVE_CHEM);
                trans_add_move_chem(t1, &n->belt_chem[read_head], n, &dst->chem_mass, dst, n->belt_chem[read_head].tons());

                // Receiving
                i64 write_head = (n->belt_head - 1) % n->belt_size;
                T_Transaction* t2 = make_transaction(T_MOVE_CHEM);
                trans_add_move_chem(t2, &src->chem_mass, src, &n->belt_chem[write_head], n, BELT_CELL_CAP);

                // Increment head
                T_Transaction* t3 = make_transaction(T_HEAD_INCREMENT | T_STATE | T_TICK);
                trans_add_head(t3, n); // Takes care of the fact, that it can only increment when at read head 0 tons.
                trans_add_tick(t3, &n->next_tick, g_w.time + BELT_TICK);
                trans_add_conveyor_state(t3, &n->conv_state, C_MOVING);
                //#############################
            }
        } break;


        default:
        {
            assert(false, "Unhandled/Wrong node type.");
        } break;
    }

    return true;
}

#include "sim_trace.h"

// g_w.END if there are no ticks in the future.
i64 get_closest_node_tick() {
    i64 best = g_w.END;

    for (int i = 0; i < g_node_count; i++) {
        i64 tick_i = g_nodes[i].next_tick;

        if (tick_i < best && tick_i >= g_w.time) {
            best = g_nodes[i].next_tick;
        }
    }

    return best;
}

int main(int argc, char** argv) {
    // miner -> truck ─> crusher -> belt ─> flotation -> rejects ─> dam
    T_Node* miner = init_node(N_MINER);
    // Creating the supplier builds the whole chem subsystem — the delivery
    // pipe, the S&OP scrum and the disruption clock (see init_node).
    T_Node* chem  = init_node(N_CHEM_SRC);

    T_Node* truck   = init_node(N_TRUCK);
    T_Node* crusher = init_node(N_CRUSHER);
    T_Node* belt    = init_node(N_CONVEYOR);
    T_Node* flot    = init_node(N_FLOTATION);
    T_Node* dam     = init_node(N_TAILINGS);

    T_Node* belt1  = init_node(N_CONVEYOR); 
    T_Node* pipe   = init_node(N_PIPE); 
    T_Node* digest = init_node(N_DIGEST); // links itself onto the chem pipe
    T_Node* pond   = init_node(N_TAILINGS);
    
    T_Node* temp_belt2 = init_node(N_CONVEYOR);
    T_Node* temp_digest_out = init_node(N_TAILINGS);

    // The additive source stays unbuilt/unscheduled for now: its step case
    // exists, but nothing consumes g_additives yet.
    
    link_waste(flot, dam);

    link_nodes(flot, belt1);
    link_nodes(belt1, digest);

    link_chems(chem, pipe);
    link_chems(pipe, digest);

    link_waste(digest, pond);

    link_nodes(digest, temp_belt2);
    link_nodes(temp_belt2, temp_digest_out);

    link_nodes(miner,   truck);
    link_nodes(truck,   crusher);
    link_nodes(crusher, belt);
    link_nodes(belt,    flot);

    // Trace every tick for the viewer app ("-" disables, see sim_trace.h).
    trace_set_name(miner->id,              "Mine");
    trace_set_name(truck->id,              "Haul truck");
    trace_set_name(crusher->id,            "Crusher");
    trace_set_name(belt->id,               "Belt conveyor");
    trace_set_name(flot->id,               "Flotation");
    trace_set_name(dam->id,                "Tailings dam");
    trace_set_name(chem->id,               "Acid supplier");
    trace_set_name(chem->owned_chem_scrum, "Acid S&OP");
    trace_set_name(pipe->id,               "Acid pipe");
    trace_set_name(digest->id,             "Acid digest");
    trace_set_name(pond->id,               "Residue pond");
    trace_open(argc > 1 ? argv[1] : "trace.bin.gz");

    i64 next_report = 30 * DAY;
    while (g_w.time < g_w.END) {
        g_w.time++;

        // Clean slate of transactions.
        // Transactions are only mid tick.
        clear_transactions();


        i64 next_tick = get_closest_node_tick();
        if (next_tick == g_w.END) {
            if (g_w.END - g_w.time >= 1 * DAY) {
                std::cout << "[engine] stalled: no events scheduled at t = "
                          << g_w.time / DAY << " d\n";
            }

            break;
        }
        g_w.time = next_tick;

        // Which nodes actually get their tick() body run at this time.
        u64 ticked_mask = 0;
        for (int i = 0; i < g_node_count; i++) {
            if (g_nodes[i].next_tick == g_w.time) ticked_mask |= 1ull << i;
        }

        for (int i=0; i<g_node_count; i++) {
            tick(i);
        }

        // On every tick, if in correct state, transfer nodes try to send/receive.
        for (int i=0; i<g_transfer_node_count; i++) {
            transfer(g_transfer_nodes[i]);
        }

        b32 changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < g_transaction_count; i++)
                // TODO(Sigull): When finished program check if precedence really isn't necessary.
                // Maybe do it, by resending more again.
                changed |= enact_transaction(i); // If all remaining transactions don't do shit, break.
        }

        // State after everything settled — this is what the viewer sees.
        trace_tick(ticked_mask);


        if (g_w.time >= next_report) {
            next_report += 30 * DAY;
            std::cout << "day " << g_w.time / DAY
                      << "  pad "    << miner->mass.tons()
                      << "  crush "  << crusher->mass.tons() << "/" << crusher->out.tons()
                      << "  belt "   << [belt]{ double s = 0; for (int i = 0; i < BELT_CELLS; i++) s += belt->belt[i].tons(); return s; }()
                      << "  flot "   << flot->mass.tons() << "/" << flot->out.tons()
                      << "  dam "    << dam->mass.tons()
                      << "  dig "    << digest->wip.tons() << "/" << digest->out.tons()
                      << "  pond "   << pond->mass.tons()
                      << "  acid "   << chem->chem_mass.tons() << "t " << chem->chem_cars << "c"
                      << "  add "    << g_additives.tons()
                      << "\n";
        }
    }

    trace_close();

    std::cout << "done at day " << g_w.time / DAY << "\n";
    return 0;
}
