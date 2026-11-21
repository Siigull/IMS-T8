// main.cpp — simulation loop.
//
// How it works, top down:
//   - The world is one struct of plain data (World). No pointers between
//     parts, so the whole thing can be copied byte-for-byte later
//     (snapshots, re-running from a saved state).
//   - Time is whole milliseconds in an i64: comparing times is exact, so
//     "the same moment" really is the same number.
//   - Every processor gets a turn at the current time, in a fixed order.
//     Turns repeat until a full round changes nothing — one processor
//     acting can unstick another at the same moment (e.g. it frees space
//     in a buffer someone was full on).
//   - Only then does the clock jump to the next moment someone needs it.
//   - No processors exist yet. They plug in at the two marked spots:
//     next_event_time() and sweep().

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#define i64 int64_t

// Time is integer milliseconds, signed — bugs then show up as negative
// times, not wrapped-around huge ones (this model subtracts times everywhere:
// durations, transit times, time-until-timer).
constexpr i64 MS   = 1;
constexpr i64 SEC  = 1000 * MS;
constexpr i64 MIN  = 60 * SEC;
constexpr i64 HOUR = 60 * MIN;
constexpr i64 DAY  = 24 * HOUR;

// --- everything you might want to change lives here ---
constexpr i64    T_END    = 90 * DAY; // Simulation max length (90 days)
constexpr i64    SEED     = 42;
constexpr i64    TURN_CAP = 1000;      // Same-moment turns before we call it a bug

constexpr i64 NEVER = INT64_MAX; // "nothing will ever happen again"

struct World {              // all simulation state, plain data
    i64 t = 0;              // clock [ms]
    i64 t_end = T_END;
    i64 clock_jumps = 0;
    i64 turns_taken = 0;
    std::mt19937_64 rng;    // the only RNG; draw order is fixed by the turn order.
};

static void die(const char *why) {
    fprintf(stderr, "simulation bug: %s\n", why);
    exit(1);
}

static i64 next_event_time(World &w) {
    i64 next = NEVER;
    // scan queue heads + processor timers here (only times > w.t count)
    return next;
}

// Processes one event at a timestamp.
// Needs to be called more times, there can be more events at a timestep.
// If a timestamp doesn't have an event, this isn't even called.
static bool sweep(World &w) {
    bool changed = false;
    // processor turns go here, in flow order
    return changed;
}

static World world_init(i64 seed) {
    World w;
    w.rng.seed(seed);

    return w;
}

int main() {
    World w = world_init(SEED);

    for (;;) {
        int guard = 0;
        while (sweep(w)) {
            if (++guard >= TURN_CAP)
                die("kept changing at the same moment, never settled");
        }


        // Init for next loop iteration.
        i64 next = next_event_time(w);
        if (next == NEVER) break;
        if (next > w.t_end) break;
        
        w.t = next;
        w.clock_jumps++;
    }

    printf("run finished: t = %lld / %lld ms, %lld clock jumps, %lld turns\n",
           (long long)w.t, (long long)w.t_end,
           (long long)w.clock_jumps,
           (long long)w.turns_taken);
    return 0;
}
