# Engine design — REE plant simulation

Discrete-event simulation, activity-scanning world view. Processors are state
machines. Material moves as timestamped batches through per-processor input
queues. One sweep function over a fixed plant layout drives time.

Processor = any participant: machines, cascade stages, sources, splitters,
observers.

## Core loop

```
while t < T_END:
    t = min(all queue heads' available_at, all processor timers)
    if t == INF: stop                     # no future events
    repeat:                               # quiescence loop
        changed = false
        for each processor in fixed order: step(p)   # sets changed
    until not changed                     # cap 1000, assert (livelock)
```

1. Queues carry **material only**. Control lives in processor state as
   timestamps: completion, failure, repair, next arrival, sampling.
2. Every queued batch carries `available_at`. A processor consumes only
   batches with `available_at <= t`.
3. Every transfer has τ > 0 (pipes, pumps). Emitted batch gets
   `available_at = t + τ`. Same-timestamp material chains are impossible.
4. Clock advances to the minimum over all queue heads and all timers.
5. Sweep order = the call order of the step functions in `sweep()`, written
   in flow order; queues are FIFO. This total order is the tie-break for
   simultaneous events. Observers run last, after all mutators.
6. Quiescence loop exists for capacity-release chains (blocked stages
   discharging as space frees). Iteration-cap assert = zero-delay-cycle
   detector.
7. Buffer space frees instantly when a consumer starts consuming.
8. Event set empty while material is stuck in blocked processors →
   gridlock; report loudly.

## Processor states

```
RUNNING         busy, completion timer
DONE_BLOCKED    finished product held inside, output buffer full
STARVED         idle, input buffer empty
DOWN            failed, repair timer
STANDBY         optional hot idle, hourly cost (furnaces)
```

Start precondition: eligible input, output space, not DOWN. On fault, fate
of in-progress material is a per-type parameter: `lost | held | downgraded`.
Permanent removal = `dead` flag, not rewiring.

## Fault propagation

- Upstream of a fault: run until output buffer fills → DONE_BLOCKED.
  Spread speed = buffer capacity / production rate.
- Downstream: run down input buffer → STARVED.
  Spread speed = buffer content / consumption rate.
- Cascade stages cannot be skipped: any stage DOWN → whole train halts,
  stages hold liquor. Restart adds re-equilibration time + off-spec window;
  off-spec output routes to recycle.

## Cascade

- Flat array of Stage PODs; neighbors are `i±1` (links implicit in order).
- Aqueous flows i→i+1, organic i+1→i, both with τ > 0.
- Per stage: holdup vectors per element, efficiency (0 when down),
  separation factor β; purity update per exchange.

## Memory layout

- One `Plant` struct = the whole world. Plain data, no pointers.
- Irregular links (feeds, split, recycles): `uint16_t` indices.
- Batch POD: `available_at`, tons, `double elem[8]` composition.
- Queues: ring buffers sized to a `constexpr` max; effective capacity is a
  runtime field ≤ max.
- No heap allocation during a run.
- World is memcpy-able → snapshots, branch experiments, golden traces.

## Determinism & validation

- Single seeded RNG (`std::mt19937_64`); draw order follows sweep order.
- Re-run from (seed, params) is bit-identical.
- Invariant, checked every N sweeps: per element,
  tons in == tons out + storage + in-transit.
- Golden trace: hash world struct every K sweeps, compare on regression.
- Port Zone 1 first; validate its distributions against the SIMLIB run
  (`ore_concentration.dat`).

## Runtime parameters

Compile-time structure, runtime values — do not template the topology.
The experiment harness sweeps: N stages, purity target, dead stage id,
buffer capacities, rates, failure/repair distributions, seed, T_END.
