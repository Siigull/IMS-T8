# Simulation trace + viewer

A game-style desktop app (raylib, C++, 60 FPS, immediate-mode UI — no web
framework) that visualizes a run of the simulation. While the sim runs, it
records the full state of every node at every tick into a compressed binary
trace; the viewer replays that trace with a scrubable timeline, a live
process-graph view and per-node inspection.

```
make            # builds ./main and ./viewer
./main          # runs the sim, writes trace.bin.gz (~10 MB for the 5y run)
./viewer        # opens the viewer on trace.bin.gz
```

`./main -` runs the sim without tracing.

## What gets recorded

One record per **settled tick** (the sim is event-driven — a "tick" is one
scheduled event time, after all transactions have settled). For every tick,
every node field that changed since the previous tick is delta-encoded:

- every material stock (mass / wip / out / belt cells / acid tank), per element
  (La, Ce, Pr, Nd, Dy + waste), as f32,
- machine states (`proc_state`, `truck_state`, `conv_state`), `next_tick`,
  belt head, chem contract (`cars`, `burned`, scrum bookkeeping),
- the world-global `chem_disrupted_until` (on the market node),
- which nodes' `tick()` bodies ran at that time (activity info),
- `g_additives` as a pseudo node ("Additive stock").

A 5-year run (~574k ticks) traces to ~10 MB gzipped, ~37 MB raw; the sim slows
from ~0.1 s to ~2 s. The trace file is flushed periodically, so the viewer's
`R` (reload) can tail a sim that is still running.

File format: see `trace_format.h` (the single source of truth, included by both
the writer in `sim_trace.h` and the viewer). Layout in short: gzip of
`[TraceFileHeader][NodeInfo × N]` followed by a stream of
`[TickHeader][NodeDelta × dirty]` records, all little-endian, packed.
`tools/validate_trace.py` is an independent Python reference parser.

## The viewer

```
┌──────────────────────────────────────────────────────────────────────┐
│ top bar     transport · speed · clock · KPIs (mined/conc/dam/acid)    │
├──────────────────────────────────────────────────────────────────────┤
│ timeline    whole-run scrubber                                        │
│             · every node's stock as an envelope, each normalized to   │
│               its own max (shapes, not absolute values — you see the  │
│               mine's sawtooth, the acid tank's staircase, the dam)     │
│             · mark lanes underneath: pad empty / acid low / flotation │
│               starved / acid disruption / S&OP scrum / campaign start│
│             · playhead; hover for a crosshair + all values tooltip    │
├───────────────────────────────────────┬──────────────────────────────┤
│ graph (the main view)                 │ inspector                     │
│  nodes as live boxes: state chip,      │  full state at the current    │
│  stock/wip/out bars, element split,   │  tick, history charts, state  │
│  next-event countdown, belt cells,     │  over time strip, jump to    │
│  acid tank, the truck driving the      │  prev/next state change       │
│  mine→plant road, flow particles       │                              │
├───────────────────────────────────────┴──────────────────────────────┤
│ table        one row per node: state · stocks · grade · next event ·  │
│              Δ24h                                                     │
└──────────────────────────────────────────────────────────────────────┘
```

### Controls

| where      | action                                                        |
|-----------|---------------------------------------------------------------|
| timeline  | **drag** = scrub the playhead (with inertia on release)        |
|          | **wheel** = zoom at cursor · **shift+wheel** = pan             |
|          | **double-click** = fit whole run · drag the minimap when zoomed|
|          | hover = crosshair + every node's value at that time            |
| graph    | click a node = inspect · drag a node = move it                 |
|          | drag background = pan · wheel = zoom                          |
| table    | click a row = inspect that node                                 |
| keys     | `space` play/pause · `←`/`→` step one tick · `shift+←/→` a day |
|          | `PgUp/PgDn` jump to prev/next notable event (also `evt` buttons)|
|          | `[` `]` playback speed · `R` reload trace · `F11` fullscreen  |
|          | `H` help/legend · `Esc` quit                                    |

Playback speeds: 1 h/s … 30 d/s of simulated time per real second.

### Finding things in the run

The mark lanes under the timeline are the "search engine":

- **amber** — mine pad empty (< 20 t): the plant is feed-starved,
- **blue** — acid tank below 50 t,
- **orange** — flotation idle with no feed (starved),
- **red bands** — acid market disruption windows,
- **cyan ticks** — S&OP scrums (contract revisions),
- **green ticks** — flotation campaign starts.

`PgUp/PgDn` (or the `< evt` / `evt >` buttons) cycle through these moments.
The **evt**-jump, crosshair tooltips and the Δ24h column are usually enough to
find when/why anything happened; clicking a node gives its full history.

### Automation / testing flags

```
./viewer [trace] --at 900d            # start at day 900 (also 5h, 1.5y, raw ms, "end")
./viewer [trace] --at end --sel Flotation
./viewer [trace] --at 300d --dump     # print every node's state at that time
./viewer [trace] --at 300d --shot out.png [--sel "Acid supplier"]
```

`--dump` output is byte-for-byte comparable with `tools/validate_trace.py`
(both were cross-checked against the sim's console report).

## Extending

- **New node kinds**: nothing to do — the trace is generic. Unknown nodes get
  an auto grid position in the graph view (drag them where you want), and
  their stocks/state appear in the table, timeline and inspector
  automatically. Node count is capped at 64 by the u64 bitmasks.
- **New per-tick fields**: add a scalar bit in `trace_format.h`
  (`TR_SC_*`), emit it in `sim_trace.h` (compare-and-delta), bump
  `TRACE_VERSION`, read it in `viewer.cpp::apply_delta`, and expose it in the
  inspector.
- **Format changes** are versioned; the viewer refuses mismatched versions.

### raylib

Vendored under `third_party/raylib` (raylib 5.5, built from source, X11 +
OpenGL 33, no audio). To rebuild it from a pristine checkout:

```
cd third_party/raylib
# compile src/{rcore,rglfw,rshapes,rtextures,rtext,rmodels,utils}.c with
#   -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 -D_GLFW_X11
#   -I src -I src/external/glfw/include
# and ar rcs lib/libraylib.a
```

Link line (see Makefile): `-lGL -lm -lpthread -ldl -lrt -lX11`.
