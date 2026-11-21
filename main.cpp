// main.cpp — simulation loop.
//
// How it works, top down:
//   - The world is one struct of plain data (World). No pointers between
//     parts, so the whole thing can be copied byte-for-byte later
//     (snapshots, re-running from a saved state).
//   - The clock never steps; it jumps to the next moment someone needs it.
//   - After each jump, every processor gets a turn, in a fixed order.
//     Turns repeat until a full round changes nothing — one processor
//     acting can unstick another at the same moment (e.g. it frees space
//     in a buffer someone was full on).
//   - No processors exist yet. They plug in at the two marked spots:
//     next_event_time() and sweep().

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

// --- everything you might want to change lives here ---
constexpr double   T_END    = 24.0 * 90; // horizon [hours] (90 days)
constexpr uint64_t SEED     = 42;        // whole run is reproducible from this
constexpr int      TURN_CAP = 1000;      // same-moment turns before we call it a bug

constexpr double NEVER = INFINITY; // "nothing will ever happen again"

struct World {              // all simulation state, plain data
    double t = 0;           // clock [hours]
    double t_end = T_END;
    uint64_t clock_jumps = 0;
    uint64_t turns_taken = 0;
    std::mt19937_64 rng;    // the only RNG; draw order is fixed by the turn
};                          // order, so a seed replays the run exactly

static void die(const char *why) {
    fprintf(stderr, "simulation bug: %s\n", why);
    exit(1);
}

// The earliest moment AFTER now that the clock is needed: the smallest of
// every queue's next batch time and every processor's timer.
// Things scheduled for right now don't count — they already had their
// turns; if they didn't act, they were busy or stuck, and someone else's
// future event will give them another chance.
static double next_event_time(World &w) {
    double next = NEVER;
    // scan queue heads + processor timers here (only times > w.t count)
    return next;
}

// One turn for every processor, in fixed flow order — this order is the
// tie-break for anything that happens at the same moment.
// Returns true if anyone did anything.
// Stats/logging goes last in here, after everyone who changes things.
static bool sweep(World &w) {
    bool changed = false;
    // processor turns go here, in flow order
    return changed;
}

int main() {
    World w;
    w.rng.seed(SEED);

    while (w.t < w.t_end) {
        double next = next_event_time(w);
        if (next == NEVER) break;  // nothing left that can ever happen
        if (next > w.t_end) break; // next event is past the horizon
        w.t = next;
        w.clock_jumps++;

        int guard = 0;
        while (sweep(w)) {
            if (++guard >= TURN_CAP)
                die("kept changing at the same moment, never settled");
        }
    }

    printf("run finished: t = %.2f / %.2f h, %llu clock jumps, %llu turns\n",
           w.t, w.t_end, (unsigned long long)w.clock_jumps,
           (unsigned long long)w.turns_taken);
    if (w.t < w.t_end)
        printf("stopped early: nothing was left that could ever happen\n");
    return 0;
}
