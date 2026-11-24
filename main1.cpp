// main.cpp — simulation loop + Zone 1 (ore extraction).
//
// The plant is a graph held in two arrays: nodes[] (the processors) and
// edges[] (the connections between them, which say where material goes).
// Every node has an input buffer: a ring buffer holding up to `cap` batches.
// Moving material takes no time on an edge: anything that takes time,
// transport included, is a node. So every batch is always either waiting
// in a node's input buffer or being worked on inside a node.
//
// A node takes the oldest batch from its input buffer, works on it, and
// pushes it into the input buffer of the emptiest node it has an edge to.
// If every one of those is full, the finished batch stays inside and the
// node is blocked; that spreads back up the chain, all the way to the mine.
// A mine makes its own batches; at a digest node batches leave the plant.
//
// Time is whole milliseconds. At each moment every node gets a turn, in
// array order, until a full round changes nothing. Then the clock jumps to
// the next moment someone needs it.
//
// A batch holds its total tons and the tons of each element's oxide (REO -
// rare earths oxide).
// Flotation is the only place that changes them: each element keeps the
// fraction r, the rest is tailings, and the batch becomes concentrate.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#define i64 int64_t

constexpr i64 MS   = 1;
constexpr i64 SEC  = 1000 * MS;
constexpr i64 MIN  = 60 * SEC;
constexpr i64 HOUR = 60 * MIN;
constexpr i64 DAY  = 24 * HOUR;

// --- run settings; each machine's own parameters live in its case in step() ---
constexpr i64 T_END    = 90 * DAY;
constexpr i64 SEED     = 42;
constexpr i64 TURN_CAP = 1000; // same-moment turns before we call it a bug

constexpr int N = 5; // elements: La, Ce, Pr, Nd, Dy
constexpr int QMAX = 32; // ring buffer slots; a node's cap can't be larger

struct Batch {
    double tons;
    double reo[N];   // tons of each element's oxide
};

enum Kind { MINE, TRANSFER, CRUSHER, FLOTATION, DIGEST };

struct Node {            // one machine: works on one batch at a time
    const char *name;
    Kind   kind;
    int    cap;             // input buffer size [batches]
    double travel_h = 0;    // TRANSFER only: how long the trip takes [h]

    Batch  in[QMAX] = {};   // input buffer, ring: oldest at in_head
    int    in_head = 0, in_count = 0;

    bool   full    = false; // holds a batch
    i64    done_at = 0;     // when that batch is finished
    Batch  b       = {};
};

struct Edge {
    int from, to;           // node ids
};

// --- the plant ---
enum { MINE_1, HAUL_1, CRUSHER_1, BELT_1, FLOTATION_1, PIPE_1, DIGEST_1, N_NODES };

Node nodes[N_NODES] = { // same order as the ids above
    //                      cap  travel [h]
    {"mine",      MINE,      0},
    {"haul",      TRANSFER,  1,  30.0 / 60}, // a truck to the crusher
    {"crusher",   CRUSHER,  10},             // ore pad
    {"belt",      TRANSFER,  1,  15.0 / 60}, // conveyor to flotation
    {"flotation", FLOTATION, 10},            // crushed ore stockpile
    {"pipe",      TRANSFER,  1,  10.0 / 60}, // slurry pipe to the concentrate store
    {"digest",    DIGEST,   13},             // Q_CONC (~150 t of concentrate); stub for Zone 2
};

Edge edges[] = {
    {MINE_1,      HAUL_1},
    {HAUL_1,      CRUSHER_1},
    {CRUSHER_1,   BELT_1},
    {BELT_1,      FLOTATION_1},
    {FLOTATION_1, PIPE_1},
    {PIPE_1,      DIGEST_1},
};

struct World {
    i64 t = 0;
    i64 clock_jumps = 0;
    i64 turns_taken = 0;
    std::mt19937_64 rng; // the only RNG; draw order is fixed by the turn order
    
    // Ore extraction stats. 
    double mined[N] = {}, tailings[N] = {}, digested[N] = {};
};

World w;


static void die(const char *why) {
    fprintf(stderr, "simulation bug: %s\n", why);
    exit(1);
}

static void die(bool cond, const char *why) {
    if (cond) {
        fprintf(stderr, "simulation bug: %s\n", why);
        exit(1);
    }
}

static i64 hours_to_ms(double h) { return (i64)std::llround(h * HOUR); }

static double uniform(double lo, double hi) {
    return std::uniform_real_distribution<double>(lo, hi)(w.rng);
}

static double exponential(double mean) {
    return std::exponential_distribution<double>(1.0 / mean)(w.rng);
}

static double triangular(double lo, double mode, double hi) {
    double u = uniform(0, 1);
    if (u < (mode - lo) / (hi - lo)) return lo + std::sqrt(u * (hi - lo) * (mode - lo));
    return hi - std::sqrt((1 - u) * (hi - lo) * (hi - mode));
}

static bool step(int id) {
    Node &n = nodes[id];
    bool changed = false;

    // 1. Finished: push the batch into the emptiest next node that has room.
    //    If none has room, the batch stays inside (the machine is blocked).
    //    At a digest node the batch leaves the plant instead.
    const bool finished_at_digest = n.full && n.done_at <= w.t && n.kind == DIGEST;
    if (finished_at_digest) {
        for (int el = 0; el < N; el++) w.digested[el] += n.b.reo[el];
        n.full = false;
        changed = true;
    }

    // re-checked after digest: digesting empties the node
    const bool finished = n.full && n.done_at <= w.t;
    if (finished) {
        Node *next = nullptr;
        for (Edge &e : edges) {
            const bool not_outgoing = e.from != id;
            if (not_outgoing) continue;
            Node &to = nodes[e.to];
            const bool has_room = to.in_count < to.cap;
            const bool emptier_than_best = !next || to.in_count < next->in_count;
            if (has_room && emptier_than_best) next = &to;
        }
        const bool found_destination = next != nullptr;
        if (found_destination) {
            const bool buffer_cap_overflows_ring = next->in_count == QMAX;
            if (buffer_cap_overflows_ring) die("input buffer cap larger than QMAX");
            next->in[(next->in_head + next->in_count) % QMAX] = n.b;
            next->in_count++;
            n.full = false;
        }
        changed = !n.full;
    }
    const bool holding_batch = n.full;
    if (holding_batch) return changed;

    // 2. Idle: take the oldest batch from the input buffer
    //    (a mine makes its own, in its case below).
    const bool not_a_mine = n.kind != MINE;
    if (not_a_mine) {
        const bool buffer_empty = n.in_count == 0;
        if (buffer_empty) return changed;
        n.b = n.in[n.in_head]; // its slot frees as soon as work starts
        n.in_head = (n.in_head + 1) % QMAX;
        n.in_count--;
    }

    // 3. Work on it: what happens to the batch and for how long.
    switch (n.kind) {
    case MINE: {
        constexpr double TONS_IN_BASKET_MIN = 80;
        constexpr double TONS_IN_BASKET_MAX = 120;
        constexpr double ARRIVAL_MEAN = 10.0; // [h] time to dig one batch
        constexpr double BASKET_ALPHA[N] = {2.4, 5.0, 0.55, 1.7, 0.18}; // Dirichlet

        double tons = uniform(TONS_IN_BASKET_MIN, TONS_IN_BASKET_MAX);
        double grade;
        do {
            grade = std::normal_distribution<double>(0.08, 0.01)(w.rng);
        } while (grade < 0.05 || grade > 0.11);

        // element basket: Dirichlet = normalized Gamma draws
        double basket[N], sum = 0;
        for (int el = 0; el < N; el++) sum += basket[el] = std::gamma_distribution<double>(BASKET_ALPHA[el])(w.rng);

        n.b = {tons, {}};
        for (int el = 0; el < N; el++) {
            n.b.reo[el] = tons * grade * basket[el] / sum;
            w.mined[el] += n.b.reo[el];
        }
        n.done_at = w.t + hours_to_ms(exponential(ARRIVAL_MEAN));
        break;
    }
    
    case TRANSFER: {
        n.done_at = w.t + hours_to_ms(n.travel_h);
        break;
    }

    case CRUSHER: {
        n.done_at = w.t + hours_to_ms(uniform(1.0, 1.5));
        break;
    }

    case FLOTATION: {
        constexpr double CONC_GRADE = 0.55; // REO fraction of concentrate

        double r = triangular(0.72, 0.80, 0.88); // recovery
        double reo = 0;
        for (int el = 0; el < N; el++) {
            w.tailings[el] += n.b.reo[el] * (1 - r);
            n.b.reo[el] *= r;
            reo += n.b.reo[el];
        }
        n.b.tons = reo / CONC_GRADE;
        n.done_at = w.t + hours_to_ms(triangular(5, 7, 11));
        break;
    }

    case DIGEST: {
        constexpr double DIGEST_RATE = 1.0; // [t/h] Zone 2 stub consumption rate

        n.done_at = w.t + hours_to_ms(exponential(n.b.tons / DIGEST_RATE));
        break;
    }
    }

    n.full = true;
    return true;
}

static i64 next_event_time() {
    i64 next = (w.t / DAY + 1) * DAY;
    
    for (Node &n : nodes)
        if (n.full && n.done_at > w.t) next = std::min(next, n.done_at);

    return next;
}

// One round of turns at the current moment: every node, in array order.
// Returns whether anything changed. Once a round changes nothing the moment
// is settled, and the observers look at it.
static bool sweep() {
    w.turns_taken++;
    bool changed = false;
    for (int id = 0; id < N_NODES; id++) changed |= step(id);
    
    return changed;
}

int main() {
    w.rng.seed(SEED);

    for (;;) {
        int guard = 0;
        while (sweep()) {
            die(++guard >= TURN_CAP,
                "kept changing at the same moment, never settled");
        }

        // A sanity check.
        // mass balance: every ton of mined REO is in tailings, digested, or still in the plant
        for (int el = 0; el < N; el++) {
            double where = w.tailings[el] + w.digested[el];
        
            for (Node &n : nodes) {
                for (int i = 0; i < n.in_count; i++) where += n.in[(n.in_head + i) % QMAX].reo[el];
                if (n.full) where += n.b.reo[el];
            }

            constexpr double SIM_TOLERANCE = 1e-9;
            die(std::fabs(w.mined[el] - where) > SIM_TOLERANCE * w.mined[el],
                "mass balance broken");
        }

        // Daily logging.
        if (w.t % DAY == 0) {
            if (w.t == 0) {
                printf("day");
                for (Node &n : nodes) printf(" %9s", n.name);
                printf("  blocked\n");
            }
            printf("%3lld", (long long)(w.t / DAY));
            for (Node &n : nodes) printf("     %2d/%-2d", n.in_count, n.cap);
            printf(" ");
            for (Node &n : nodes)
                if (n.full && n.done_at <= w.t) printf(" %s", n.name);
            printf("\n");
        }

        // Next iter prep.
        i64 next = next_event_time();
        if (next > T_END) break;
        w.t = next;
        w.clock_jumps++;
    }

    printf("run finished: %lld clock jumps, %lld turns\n", (long long)w.clock_jumps, (long long)w.turns_taken);
    return 0;
}
