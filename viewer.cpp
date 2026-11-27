// #############################################################################
// ## IMS-T8 trace viewer — a small game-style app (raylib), not a webpage.   ##
// ##                                                                          ##
// ##   ./viewer [trace.bin.gz] [--shot out.png] [--at 300d]                  ##
// ##                                                                          ##
// ## Layout:                                                                  ##
// ##   [ top bar    ] transport, speed, clock, KPIs                           ##
// ##   [ timeline   ] whole-run scrubber: stock envelopes, event lanes,      ##
// ##                   playhead. Drag = scrub (with inertia), wheel = zoom.  ##
// ##   [ graph      ] the plant: nodes as live boxes, animated flows,        ##
// ##                   clickable for the inspector.                          ##
// ##   [ inspector  ] selected node: full state, history chart, state strip. ##
// ##   [ table      ] every node's current values.                            ##
// ##                                                                          ##
// ## Keys: space play/pause · ←/→ step tick · shift+←/→ step day ·            ##
// ##        PgUp/PgDn prev/next notable event · [ ] speed · R reload trace ·  ##
// ##        F11 fullscreen · H help · Esc quit                                ##
// #############################################################################

#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include "trace_format.h"

#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

// ============================================================================
//                              small utilities
// ============================================================================

typedef int64_t i64;
typedef uint64_t u64;

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static double lerpd(double a, double b, double u)    { return a + (b - a) * u; }

static const i64 MS_MIN = 60000, MS_HOUR = 3600000, MS_DAY = 86400000;

// "Y2 D147 08:35"
static std::string fmt_time(i64 t) {
    if (t < 0) t = 0;
    int days = (int)(t / MS_DAY);
    int year = days / 365 + 1;
    int doy   = days % 365 + 1;
    int rem   = (int)(t % MS_DAY);
    char buf[64];
    snprintf(buf, sizeof buf, "Y%d D%03d  %02d:%02d", year, doy, rem / 3600000, (rem / 60000) % 60);
    return buf;
}
// "3.2 d" / "5.4 h" / "12 min"
static std::string fmt_dur(i64 t) {
    double a = fabs((double)t);
    char buf[48];
    if (a >= 2 * MS_DAY)        snprintf(buf, sizeof buf, "%.1f d", t / (double)MS_DAY);
    else if (a >= 2 * MS_HOUR)  snprintf(buf, sizeof buf, "%.1f h", t / (double)MS_HOUR);
    else if (a >= 2 * MS_MIN)   snprintf(buf, sizeof buf, "%.0f min", t / (double)MS_MIN);
    else if (a >= 0)            snprintf(buf, sizeof buf, "%.0f s", t / 1000.0);
    else                        snprintf(buf, sizeof buf, "-");
    return buf;
}
// ============================================================================
//                                  theme
// ============================================================================

static const Color COL_BG      = { 13, 17, 23, 255 };
static const Color COL_PANEL   = { 22, 27, 34, 255 };
static const Color COL_PANEL2  = { 28, 34, 43, 255 };
static const Color COL_BORDER  = { 48, 54, 61, 255 };
static const Color COL_TEXT    = { 201, 209, 217, 255 };
static const Color COL_DIM     = { 139, 148, 158, 255 };
static const Color COL_FAINT   = { 110, 118, 129, 255 };
static const Color COL_ACCENT  = { 88, 166, 255, 255 };
static const Color COL_PLAYHEAD= { 255, 216, 102, 255 };
static const Color COL_GOOD    = { 63, 185, 80, 255 };
static const Color COL_WARN    = { 219, 171, 40, 255 };
static const Color COL_BAD     = { 248, 81, 73, 255 };

// element palette: La, Ce, Pr, Nd, Dy, waste
static const Color COL_ELEMS[6] = {
    { 249, 117, 131, 255 }, { 255, 166, 87, 255 }, { 255, 216, 102, 255 },
    { 126, 231, 135, 255 }, { 121, 192, 255, 255 }, { 110, 118, 129, 255 },
};
static const char* ELEM_NAMES[6] = { "La", "Ce", "Pr", "Nd", "Dy", "waste" };

// ============================================================================
//                            trace data model
// ============================================================================

static const int NB   = 4096;   // timeline buckets
static const int CKPT = 128;    // node state checkpoints every N changes

struct NodeState {
    float mass[TRACE_MASS_SLOTS];                 // trace mass-slot layout
    i64   next_tick = 0, chem_last_scrum = 0, disrupted_until = -1;
    int32_t chem_cars = 0, belt_head = 0;
    float chem_burned = 0, chem_burned_at_scrum = 0;
    float flow_in_tot = 0, flow_out_tot = 0;   // cumulative tons moved (monotonic counters)
    uint8_t proc_state = 0, truck_state = 0, conv_state = 0;

    void zero() { memset(this, 0, sizeof(*this)); disrupted_until = -1; }

    float  m(int e) const  { return mass[e]; }                    // mass.elem / waste(e=5)
    float  w(int e) const  { return mass[6 + e]; }
    float  o(int e) const  { return mass[12 + e]; }
    float  belt(int c, int e) const { return mass[18 + c * 6 + e]; }
    float  chem() const { return mass[54]; }

    double mass_t() const { double s = 0; for (int e = 0; e < 6; e++) s += mass[e]; return s; }
    double wip_t()   const { double s = 0; for (int e = 0; e < 6; e++) s += mass[6 + e]; return s; }
    double out_t()   const { double s = 0; for (int e = 0; e < 6; e++) s += mass[12 + e]; return s; }
    double belt_t()  const { double s = 0; for (int e = 0; e < 36; e++) s += mass[18 + e]; return s; }
    double level()   const { return mass_t() + wip_t() + out_t() + belt_t() + chem(); }
};

struct InflowEvent  { i64 t; float tons; };
struct StateChange  { i64 t; int field; uint8_t value; };  // field 0=proc 1=truck 2=conv
struct Series       { float mn = 1e30f, mx = -1e30f, last = 0; };

struct NodeSeries {
    Series mass, wip, out, chem;
    float out_elem[TRACE_MAX_ELEMS] = {};   // "last" per bucket (stacked charts)
    float inflow = 0;                        // tons arrived within the bucket
};

struct Node {
    TraceNodeInfo info = {};
    Color  color;
    Vector2 pos, size;                        // graph layout (world units)
    bool   draggable = true;

    // delta stream
    std::vector<uint8_t>  dbuf;               // concatenated deltas
    std::vector<uint32_t> dtick, doff, dlen;  // per change: tick, offset, len
    std::vector<NodeState> ckpt;              // ckpt[k] = state after change k*CKPT

    NodeSeries series[NB];
    std::vector<InflowEvent> inflow;
    std::vector<double> inflow_prefix;        // prefix sum of tons, aligned with inflow[]
    std::vector<StateChange> state_changes;
    double cum_in = 0;                        // total tons ever arrived
};

enum NodeTypeId {                             // mirrors E_Node_Type in the sim
    T_MINER = 0, T_CHEM_SRC, T_CHEM_DISRUPTION, T_CHEM_SCRUM, T_ADDITIVE_SRC,
    T_TRUCK, T_CRUSHER, T_CONVEYOR, T_FLOTATION, T_TAILINGS
};

// timeline marker: a segment [t0,t1] (point if t0==t1)
enum { N_PAD_EMPTY, N_CHEM_LOW, N_FLOT_STARVED, N_DISRUPTED, N_SCRUM, N_CAMPAIGN, N_KINDS };
struct Mark { i64 t0, t1; int kind; };

struct Trace {
    std::vector<Node> nodes;
    std::vector<i64>  ticks;                  // tick times, strictly increasing
    std::vector<u64>  ticked;                 // ticked_mask per tick
    std::vector<Mark> marks;                  // sorted by t0
    i64 t_end = 0;                            // sim horizon
    char elem_names[TRACE_MAX_ELEMS][8];
    bool truncated = false;
    char path[512] = "";
} TR;

// ============================================================================
//                              trace loading
// ============================================================================

// apply one delta record, return bytes consumed
static size_t apply_delta(NodeState& s, const uint8_t* p) {
    uint32_t smask; memcpy(&smask, p, 4); p += 4;
    u64 mmask;     memcpy(&mmask, p, 8); p += 8;
    size_t n = 12;
    if (smask & (1u << TR_SC_PROC_STATE))           { s.proc_state  = *p++; n += 1; }
    if (smask & (1u << TR_SC_TRUCK_STATE))          { s.truck_state = *p++; n += 1; }
    if (smask & (1u << TR_SC_CONV_STATE))           { s.conv_state  = *p++; n += 1; }
    if (smask & (1u << TR_SC_NEXT_TICK))            { memcpy(&s.next_tick, p, 8); p += 8; n += 8; }
    if (smask & (1u << TR_SC_CHEM_CARS))            { memcpy(&s.chem_cars, p, 4); p += 4; n += 4; }
    if (smask & (1u << TR_SC_CHEM_BURNED))          { memcpy(&s.chem_burned, p, 4); p += 4; n += 4; }
    if (smask & (1u << TR_SC_CHEM_BURNED_AT_SCRUM)) { memcpy(&s.chem_burned_at_scrum, p, 4); p += 4; n += 4; }
    if (smask & (1u << TR_SC_CHEM_LAST_SCRUM))      { memcpy(&s.chem_last_scrum, p, 8); p += 8; n += 8; }
    if (smask & (1u << TR_SC_DISRUPTED_UNTIL))      { memcpy(&s.disrupted_until, p, 8); p += 8; n += 8; }
    if (smask & (1u << TR_SC_BELT_HEAD))            { memcpy(&s.belt_head, p, 4); p += 4; n += 4; }
    if (smask & (1u << TR_SC_FLOW_IN))              { memcpy(&s.flow_in_tot, p, 4); p += 4; n += 4; }
    if (smask & (1u << TR_SC_FLOW_OUT))             { memcpy(&s.flow_out_tot, p, 4); p += 4; n += 4; }
    for (int i = 0; i < TRACE_MASS_SLOTS; i++)
        if (mmask & (1ull << i)) { memcpy(&s.mass[i], p, 4); p += 4; n += 4; }
    return n;
}

static Color type_color(uint32_t t) {
    switch (t) {
        case T_MINER:           return { 227, 179, 65, 255 };
        case T_CHEM_SRC:        return { 57, 197, 207, 255 };
        case T_CHEM_DISRUPTION: return { 188, 140, 255, 255 };
        case T_CHEM_SCRUM:      return { 88, 166, 255, 255 };
        case T_ADDITIVE_SRC:
        case TRACE_NODE_ADDITIVE_STOCK: return { 219, 97, 162, 255 };
        case T_TRUCK:           return { 240, 136, 62, 255 };
        case T_CRUSHER:         return { 255, 123, 114, 255 };
        case T_CONVEYOR:        return { 234, 197, 79, 255 };
        case T_FLOTATION:       return { 63, 185, 80, 255 };
        case T_TAILINGS:        return { 168, 135, 164, 255 };
        default:                return { 150, 150, 150, 255 };
    }
}

static bool load_trace(const char* path) {
    double t0 = GetTime();
    gzFile f = gzopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }

    std::vector<uint8_t> raw;
    raw.reserve(48 << 20);
    uint8_t chunk[1 << 16];
    int n;
    while ((n = gzread(f, chunk, sizeof chunk)) > 0) raw.insert(raw.end(), chunk, chunk + n);
    gzclose(f);

    const uint8_t* p = raw.data();
    const uint8_t* end = raw.data() + raw.size();

    TraceFileHeader h;
    if (end - p < (ptrdiff_t)sizeof h) return false;
    memcpy(&h, p, sizeof h); p += sizeof h;
    if (memcmp(h.magic, TRACE_MAGIC, 8) != 0 || h.version != TRACE_VERSION) {
        fprintf(stderr, "bad trace magic/version\n"); return false;
    }
    memcpy(TR.elem_names, h.elem_names, sizeof TR.elem_names);
    TR.t_end = h.t_end;

    TR.nodes.resize(h.node_count);
    for (uint32_t i = 0; i < h.node_count; i++) {
        TraceNodeInfo ni;
        if (end - p < (ptrdiff_t)sizeof ni) return false;
        memcpy(&ni, p, sizeof ni); p += sizeof ni;
        TR.nodes[i].info = ni;
        TR.nodes[i].color = type_color(ni.type);
    }

    // ---- stream the ticks ------------------------------------------------
    std::vector<NodeState> run(h.node_count);
    for (auto& s : run) s.zero();
    double prev_level[TRACE_MAX_NODES] = {};
    uint8_t prev_state3[TRACE_MAX_NODES][3] = {};
    i64 prev_disrupt_until = -1;
    int cur_bucket = -1;
    double bucket_tons[TRACE_MAX_NODES] = {};
    int pred[4] = { -1, -1, -1, -1 };          // predicate segment starts (tick idx)
    int mi = -1, ci = -1, fi = -1;             // miner / chem src / flotation ids
    for (uint32_t i = 0; i < h.node_count; i++) {
        uint32_t t = TR.nodes[i].info.type;
        if (t == T_MINER) mi = i;
        if (t == T_CHEM_SRC) ci = i;
        if (t == T_FLOTATION) fi = i;
    }

    while (p < end) {
        if (end - p < (ptrdiff_t)sizeof(TraceTickHeader)) { TR.truncated = true; break; }
        TraceTickHeader th; memcpy(&th, p, sizeof th); p += sizeof th;
        i64 time = th.time;
        u64 ticked = th.ticked_mask, dirty = th.dirty_mask;

        int tick_idx = (int)TR.ticks.size();
        TR.ticks.push_back(time);
        TR.ticked.push_back(ticked);
        if (time > TR.t_end) TR.t_end = time;

        int b = (int)clampd((double)time / TR.t_end * NB, 0, NB - 1);
        if (b != cur_bucket) {
            if (cur_bucket >= 0)
                for (uint32_t i = 0; i < h.node_count; i++)
                    TR.nodes[i].series[cur_bucket].inflow = (float)bucket_tons[i];
            memset(bucket_tons, 0, sizeof bucket_tons);
            cur_bucket = b;
        }

        bool tick_bad = false;
        for (uint32_t i = 0; i < h.node_count; i++) {
            if (!((dirty >> i) & 1)) continue;
            const uint8_t* dstart = p;
            if (end - p < 12) { tick_bad = true; break; }
            NodeState before = run[i];
            (void)before;
            size_t consumed = apply_delta(run[i], p);
            p += consumed;
            if (p > end) { tick_bad = true; break; }

            Node& nd = TR.nodes[i];
            nd.dbuf.insert(nd.dbuf.end(), dstart, p);
            nd.dtick.push_back(tick_idx);
            nd.doff.push_back((uint32_t)(nd.dbuf.size() - (p - dstart)));
            nd.dlen.push_back((uint32_t)(p - dstart));
            if (nd.dtick.size() % CKPT == 0) nd.ckpt.push_back(run[i]);

            // ---- events ---------------------------------------------------
            // flow counters are cumulative: per-tick flow is the diff. For stocks
            // that fill without transactions (acid deliveries) fall back to the
            // net level jump.
            double lv = run[i].level();
            double jump = lv - prev_level[i];
            double tick_in = run[i].flow_in_tot - before.flow_in_tot;
            double tick_out = run[i].flow_out_tot - before.flow_out_tot;
            // transfers in > production in (sources) > net level jump (tank fills)
            double arrived = tick_in > 0.01 ? tick_in : (jump > 1.0 ? jump : 0);
            if (arrived > 1.0) {
                nd.inflow.push_back({ time, (float)arrived });
                nd.cum_in += arrived;
                bucket_tons[i] += arrived;
            }
            prev_level[i] = lv;

            uint8_t st3[3] = { run[i].proc_state, run[i].truck_state, run[i].conv_state };
            for (int fl = 0; fl < 3; fl++)
                if (st3[fl] != prev_state3[i][fl])
                    nd.state_changes.push_back({ time, fl, st3[fl] });
            memcpy(prev_state3[i], st3, 3);

            // ---- buckets --------------------------------------------------
            NodeSeries& s = nd.series[b];
            auto upd = [&](Series& se, double v) {
                se.mn = (float)std::min((double)se.mn, v);
                se.mx = (float)std::max((double)se.mx, v);
                se.last = (float)v;
            };
            upd(s.mass, run[i].mass_t());
            upd(s.wip,  run[i].wip_t());
            upd(s.out,  run[i].out_t());
            upd(s.chem, run[i].chem());
            for (int e = 0; e < TRACE_MAX_ELEMS; e++) s.out_elem[e] = run[i].o(e);

            // ---- chem market disruption windows -----------------------------
            if (nd.info.type == T_CHEM_DISRUPTION && run[i].disrupted_until != prev_disrupt_until) {
                if (run[i].disrupted_until > time)
                    TR.marks.push_back({ time, run[i].disrupted_until, N_DISRUPTED });
                prev_disrupt_until = run[i].disrupted_until;
            }
            if (nd.info.type == T_CHEM_SCRUM && before.chem_last_scrum != run[i].chem_last_scrum)
                TR.marks.push_back({ time, time, N_SCRUM });
            if (nd.info.type == T_FLOTATION && st3[0] == 2 && prev_state3[i][0] == 1)
                TR.marks.push_back({ time, time, N_CAMPAIGN });
        }
        if (tick_bad) { TR.ticks.pop_back(); TR.ticked.pop_back(); TR.truncated = true; break; }

        // ---- notable predicate segments (evaluated per tick) ---------------
        auto seg = [&](int idx, bool active) {
            if (active && pred[idx] < 0) pred[idx] = (int)TR.ticks.size() - 1;
            if (!active && pred[idx] >= 0) {
                TR.marks.push_back({ TR.ticks[pred[idx]], time, idx });
                pred[idx] = -1;
            }
        };
        seg(N_PAD_EMPTY,    mi >= 0 && run[mi].mass_t() < 20.0);
        seg(N_CHEM_LOW,     ci >= 0 && run[ci].chem() < 50.0);
        seg(N_FLOT_STARVED, fi >= 0 && run[fi].proc_state == 1 && run[fi].mass_t() < 1.0);
    }

    if (cur_bucket >= 0)
        for (uint32_t i = 0; i < h.node_count; i++)
            TR.nodes[i].series[cur_bucket].inflow = (float)bucket_tons[i];

    // fill empty buckets forward (carry last known value)
    for (auto& nd : TR.nodes)
        for (int b = 0; b < NB; b++) {
            NodeSeries& s = nd.series[b];
            if (s.mass.mn > s.mass.mx) {           // no sample in this bucket
                if (b > 0) { s = nd.series[b - 1]; s.inflow = 0; }
                else { s.mass = s.wip = s.out = s.chem = {0, 0, 0}; s.inflow = 0; }
            }
        }

    // close dangling predicate segments at the end of time
    for (int k = 0; k < 4; k++)
        if (pred[k] >= 0 && !TR.ticks.empty())
            TR.marks.push_back({ TR.ticks[pred[k]], TR.t_end, k });

    // inflow prefix sums (for "arrived to date" values at any playhead)
    for (auto& nd : TR.nodes) {
        nd.inflow_prefix.resize(nd.inflow.size() + 1);
        double acc = 0;
        nd.inflow_prefix[0] = 0;
        for (size_t i = 0; i < nd.inflow.size(); i++) {
            acc += nd.inflow[i].tons;
            nd.inflow_prefix[i + 1] = acc;
        }
    }

    std::sort(TR.marks.begin(), TR.marks.end(),
              [](const Mark& a, const Mark& b) { return a.t0 < b.t0; });

    snprintf(TR.path, sizeof TR.path, "%s", path);
    printf("[viewer] %s: %zu nodes, %zu ticks, %zu marks, %.1f MB raw, load %.2fs\n",
           path, TR.nodes.size(), TR.ticks.size(), TR.marks.size(),
           raw.size() / 1048576.0, GetTime() - t0);
    if (TR.truncated) printf("[viewer] warning: trace ended mid-record (sim killed while writing?)\n");
    return TR.ticks.size() > 1;
}

// tons arrived at this node up to time t
static double cum_in_at(Node& nd, double t) {
    if (nd.inflow.empty()) return 0;
    auto it = std::upper_bound(nd.inflow.begin(), nd.inflow.end(), t,
        [](double tt, const InflowEvent& e) { return tt < (double)e.t; });
    return nd.inflow_prefix[it - nd.inflow.begin()];
}

// ============================================================================
//                         state reconstruction
// ============================================================================

static int tick_at(double t) {   // last tick index with time <= t
    auto& T = TR.ticks;
    int lo = 0, hi = (int)T.size() - 1;
    if (t <= T[0]) return 0;
    if (t >= T[hi]) return hi;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (T[mid] <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

// full node state at tick index k (checkpoints + <= CKPT delta replays)
static void node_state_at(int n, int k, NodeState& out) {
    Node& nd = TR.nodes[n];
    int lo = 0, hi = (int)nd.dtick.size() - 1, c = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((int)nd.dtick[mid] <= k) { c = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (c < 0) { out.zero(); return; }
    // ckpt[j] = state after change index (j+1)*CKPT-1 (pushed when dtick.size()
    // hits multiples of CKPT). Largest usable checkpoint for change c:
    int j = (c + 1) / CKPT - 1;
    if (j >= 0) {
        out = nd.ckpt[j];
        for (int i = (j + 1) * CKPT; i <= c; i++)
            apply_delta(out, nd.dbuf.data() + nd.doff[i]);
    } else {
        out.zero();
        for (int i = 0; i <= c; i++)
            apply_delta(out, nd.dbuf.data() + nd.doff[i]);
    }
}

struct Frame {
    int k = -2;
    NodeState st[TRACE_MAX_NODES];
    NodeState next[TRACE_MAX_NODES];   // state at k+1, for interpolation
    bool valid = false;
} FR;

static void build_frame(double t) {
    int k = tick_at(t);
    if (FR.k == k && FR.valid) return;
    FR.k = k;
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        node_state_at((int)i, k, FR.st[i]);
        node_state_at((int)i, k + 1 < (int)TR.ticks.size() ? k + 1 : k, FR.next[i]);
    }
    FR.valid = true;
}

// ============================================================================
//                                UI state
// ============================================================================

static struct UIState {
    double t = 0;                    // current sim time (ms, continuous)
    bool   playing = false;
    int    speed_idx = 2;
    int    sel = -1, hover = -1;
    double vw0 = 0, vw1 = 1;         // timeline view window (ms), animated
    double vw0t = 0, vw1t = 1;       // targets
    bool   scrubbing = false;
    double scrub_v = 0;               // inertia (ms per real s)
    Vector2 cam = { 0, 0 }; float zoom = 1;
    Vector2 cam_t = { 0, 0 }; float zoom_t = 1;
    Vector2 mouse;
    bool   show_help = false;
    char   tooltip[1024] = "";
    int    drag_node = -1;
    bool   pan_graph = false;
    Vector2 pan_grab;
    int    reload_request = 0;
    int    k = 0;                    // current tick index
    int    s24_k = -3;               // 24h-ago cache (table), -3 = invalid
    float  insp_scroll = 0, insp_h = 0;
    NodeState s24[TRACE_MAX_NODES];
    std::vector<size_t> inflow_cursor;   // per-node event cursors (particles)
} UI;

static const double SPEEDS[] = { MS_HOUR, 6 * MS_HOUR, MS_DAY, 7 * MS_DAY, 30 * MS_DAY };
static const char* SPEED_NAMES[] = { "1 h/s", "6 h/s", "1 d/s", "7 d/s", "30 d/s" };

// ============================================================================
//                                  fonts
// ============================================================================

struct Fnt { Font f; float sz; };
static Fnt F13, F16, F20, FB16, FB20;

static void load_fonts() {
    const char* mono[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf", nullptr
    };
    const char* bold[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-B.ttf", nullptr
    };
    const char* mp = nullptr, *bp = nullptr;
    for (int i = 0; mono[i]; i++) if (FileExists(mono[i])) { mp = mono[i]; break; }
    for (int i = 0; bold[i]; i++) if (FileExists(bold[i])) { bp = bold[i]; break; }
    if (!mp) {
        Font d = GetFontDefault();
        F13 = { d, 10 }; F16 = { d, 10 }; F20 = { d, 10 }; FB16 = { d, 10 }; FB20 = { d, 10 };
        return;
    }

    // ASCII + the few extra glyphs the UI uses (· — – … ‹ › arrows Δ τ ± °)
    int cps[128]; int n = 0;
    for (int c = 32; c <= 126; c++) cps[n++] = c;
    int extra[] = { 0xB7, 0xB0, 0xB1, 0x2013, 0x2014, 0x2026, 0x2039, 0x203A,
                    0x2190, 0x2191, 0x2192, 0x2193, 0x0394, 0x03C4, 0x2264, 0x2265 };
    for (int c : extra) cps[n++] = c;

    F13 = { LoadFontEx(mp, 26, cps, n), 13 };  SetTextureFilter(F13.f.texture, TEXTURE_FILTER_BILINEAR);
    F16 = { LoadFontEx(mp, 32, cps, n), 16 };  SetTextureFilter(F16.f.texture, TEXTURE_FILTER_BILINEAR);
    F20 = { LoadFontEx(mp, 40, cps, n), 20 };  SetTextureFilter(F20.f.texture, TEXTURE_FILTER_BILINEAR);
    if (bp) {
        FB16 = { LoadFontEx(bp, 32, cps, n), 16 }; SetTextureFilter(FB16.f.texture, TEXTURE_FILTER_BILINEAR);
        FB20 = { LoadFontEx(bp, 40, cps, n), 20 }; SetTextureFilter(FB20.f.texture, TEXTURE_FILTER_BILINEAR);
    } else FB16 = F16, FB20 = F20;
}

static void text(Fnt f, float x, float y, const char* s, Color c) {
    DrawTextEx(f.f, s, { x, y }, f.sz, 0, c);
}
static Vector2 measure(Fnt f, const char* s) { return MeasureTextEx(f.f, s, f.sz, 0); }

// draw text, truncated with "..." if wider than maxw
static void text_trunc(Fnt f, float x, float y, const char* s, Color c, float maxw) {
    Vector2 ts = measure(f, s);
    if (ts.x <= maxw) { text(f, x, y, s, c); return; }
    char buf[128];
    int lo = 0, hi = (int)strlen(s);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        snprintf(buf, sizeof buf, "%.*s...", mid, s);
        if (measure(f, buf).x > maxw) hi = mid; else lo = mid + 1;
    }
    snprintf(buf, sizeof buf, "%.*s...", std::max(0, lo - 1), s);
    text(f, x, y, buf, c);
}

// right-aligned text
static void text_r(Fnt f, float xr, float y, const char* s, Color c) {
    text(f, xr - measure(f, s).x, y, s, c);
}

// next-event label: future -> duration, past -> overdue, none -> never
static std::string fmt_next(i64 next, double now) {
    if (next <= 0) return "never";
    if ((double)next < now) return "overdue";
    return fmt_dur(next - (i64)now);
}

// tonnage with thousands separators: "541,347 t"
static std::string fmt_tons2(double v, const char* unit = " t") {
    char buf[48];
    if (v >= 100) {
        long long iv = (long long)llround(v);
        char tmp[32]; snprintf(tmp, sizeof tmp, "%lld", iv);
        int tl = (int)strlen(tmp), oi = 0;
        for (int i = 0; i < tl; i++) {
            buf[oi++] = tmp[i];
            int rem = tl - 1 - i;
            if (rem > 0 && rem % 3 == 0) buf[oi++] = ',';
        }
        buf[oi] = 0;
        snprintf(buf + oi, sizeof buf - oi, "%s", unit);
    } else if (v >= 1) snprintf(buf, sizeof buf, "%.1f%s", v, unit);
    else if (v > 0.005) snprintf(buf, sizeof buf, "%.2f%s", v, unit);
    else snprintf(buf, sizeof buf, "-%s", unit);
    return buf;
}

// ============================================================================
//                                widgets
// ============================================================================

static bool button(Rectangle r, const char* label, Fnt f) {
    bool h = CheckCollisionPointRec(UI.mouse, r);
    DrawRectangleRec(r, h ? COL_PANEL2 : COL_PANEL);
    DrawRectangleLinesEx(r, 1, h ? COL_ACCENT : COL_BORDER);
    Vector2 ts = measure(f, label);
    text(f, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2, label, h ? COL_TEXT : COL_DIM);
    return h && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

// transport icon button with drawn glyphs
static bool icon_button(Rectangle r, int kind, bool h) {
    DrawRectangleRec(r, h ? COL_PANEL2 : COL_PANEL);
    DrawRectangleLinesEx(r, 1, h ? COL_ACCENT : COL_BORDER);
    Color c = h ? COL_TEXT : COL_DIM;
    float cx = r.x + r.width / 2, cy = r.y + r.height / 2, s = 6;
    auto tri = [&](float dx, float ss, Color cc) {
        DrawTriangle({ cx + dx + ss, cy - ss }, { cx + dx - ss * 0.6f, cy }, { cx + dx + ss, cy + ss }, cc);
    };
    switch (kind) {
        case 0:  // to start |<
            DrawRectangle(cx - 10, cy - 6, 3, 12, c); tri(-1, s, c); break;
        case 1:  // prev tick  |<
            DrawRectangle(cx - 3, cy - 5, 2.5f, 10, Fade(c, 0.45f)); tri(4, 4.5f, c); break;
        case 2:  // play
            DrawTriangle({ cx - s + 2, cy - s + 1 }, { cx - s + 2, cy + s - 1 }, { cx + s + 3, cy }, c); break;
        case 3:  // pause
            DrawRectangle(cx - 6, cy - 7, 4, 14, c); DrawRectangle(cx + 2, cy - 7, 4, 14, c); break;
        case 4:  // next tick  >|
            tri(-4, 4.5f, c); DrawRectangle(cx + 2, cy - 5, 2.5f, 10, Fade(c, 0.45f)); break;
        case 5:  // to end >|
            tri(1, s, c); DrawRectangle(cx + 8, cy - 6, 3, 12, c); break;
    }
    return h && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

static void draw_tooltip() {
    if (!UI.tooltip[0]) return;
    const float pad = 8;
    int lines = 1;
    for (const char* c = UI.tooltip; *c; c++) if (*c == '\n') lines++;
    float w = 0, h = lines * (F13.sz + 3);
    char buf[1024]; strncpy(buf, UI.tooltip, 1023); buf[1023] = 0;
    for (char* line = strtok(buf, "\n"); line; line = strtok(nullptr, "\n")) {
        Vector2 ts = measure(F13, line);
        if (ts.x > w) w = ts.x;
    }
    w += pad * 2;
    float x = UI.mouse.x + 16, y = UI.mouse.y + 14;
    if (x + w > GetScreenWidth() - 8) x = UI.mouse.x - w - 16;
    if (y + h > GetScreenHeight() - 8) y = UI.mouse.y - h - 12;
    DrawRectangle(x, y, w, h, { 20, 26, 34, 246 });
    DrawRectangleLinesEx({ x, y, w, h }, 1, COL_BORDER);
    strncpy(buf, UI.tooltip, 1023); buf[1023] = 0;
    float yy = y + pad - 2;
    for (char* line = strtok(buf, "\n"); line; line = strtok(nullptr, "\n")) {
        text(F13, x + pad, yy, line, COL_TEXT); yy += F13.sz + 3;
    }
}

// small status chip; returns width
static float chip(float x, float y, const char* s, Color c, Fnt f = F13) {
    Vector2 ts = measure(f, s);
    float w = ts.x + 12;
    float chh = f.sz + 6;
    DrawRectangle(x, y, w, chh, Fade(c, 0.16f));
    DrawRectangleLinesEx({ x, y, w, chh }, 1, Fade(c, 0.45f));
    text(f, x + 6, y + (chh - f.sz) / 2 - 1, s, c);
    return w;
}

// label-left / value-right stat line with a thin bar track underneath
static void stat_line(float x, float y, float w, const char* label, const char* value,
                      Color vc, double bar_v, double bar_cap, Color bar_c) {
    text(F13, x, y, label, COL_FAINT);
    text_r(F13, x + w, y + 1, value, vc);
    float by = y + F13.sz + 2;
    DrawRectangle(x, by, w, 3, COL_PANEL2);
    if (bar_v > 0 && bar_cap > 0) {
        double u = clampd(bar_v / bar_cap, 0, 1);
        DrawRectangle(x, by, (float)(w * u), 3, Fade(bar_c, 0.55f));
    }
}

// ============================================================================
//                              screen layout
// ============================================================================

struct Layout {
    float top_bar_h = 54, timeline_h = 178, table_h = 190, inspector_w = 440;
    Rectangle timeline{}, graph{}, inspector{}, table{};
    Rectangle tl_minimap{ -1,-1,-1,-1 };
    bool screenshot_mode = false;
    void compute() {
        float W = GetScreenWidth(), H = GetScreenHeight();
        timeline  = { 0, top_bar_h, W, timeline_h };
        graph    = { 0, top_bar_h + timeline_h, W - inspector_w,
                     H - top_bar_h - timeline_h - table_h };
        inspector= { W - inspector_w, top_bar_h + timeline_h, inspector_w,
                     H - top_bar_h - timeline_h - table_h };
        table    = { 0, H - table_h, W, table_h };
    }
} LY;

// node cards are drawn at a FIXED pixel size; camera zoom only spreads
// their positions apart. Keeps text inside cards at every zoom.
static Vector2 world_to_screen(Vector2 w) {
    Vector2 c = { LY.graph.x + LY.graph.width * 0.5f, LY.graph.y + LY.graph.height * 0.5f };
    return { (w.x - UI.cam.x) * UI.zoom + c.x, (w.y - UI.cam.y) * UI.zoom + c.y };
}
static Vector2 screen_to_world(Vector2 s) {
    Vector2 c = { LY.graph.x + LY.graph.width * 0.5f, LY.graph.y + LY.graph.height * 0.5f };
    return { (s.x - c.x) / UI.zoom + UI.cam.x, (s.y - c.y) / UI.zoom + UI.cam.y };
}

// ============================================================================
//                              graph layout
// ============================================================================

static void default_layout() {
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        Node& n = TR.nodes[i];
        const char* name = n.info.name;
        n.size = { 200, 110 };
        if (n.info.type == T_CONVEYOR)  n.size = { 252, 106 };
        if (n.info.type == T_FLOTATION) n.size = { 210, 150 };
        if (n.info.type == T_TAILINGS)  n.size = { 200, 156 };
        if (n.info.type == T_CHEM_SRC)  n.size = { 232, 132 };
        if (n.info.type == T_CHEM_DISRUPTION) n.size = { 208, 74 };
        if (n.info.type == T_CHEM_SCRUM) n.size = { 210, 108 };
        if (n.info.type == TRACE_NODE_ADDITIVE_STOCK) n.size = { 208, 74 };
        if      (!strcmp(name, "Mine"))           n.pos = { 40, 420 };
        else if (!strcmp(name, "Haul truck"))     n.pos = { 250, 560 };
        else if (!strcmp(name, "Crusher"))       n.pos = { 430, 420 };
        else if (!strcmp(name, "Belt conveyor"))  n.pos = { 720, 432 };
        else if (!strcmp(name, "Flotation"))      n.pos = { 1050, 402 };
        else if (!strcmp(name, "Tailings dam"))   n.pos = { 1350, 392 };
        else if (!strcmp(name, "Chem market"))    n.pos = { 60, 64 };
        else if (!strcmp(name, "Acid supplier"))  n.pos = { 400, 48 };
        else if (!strcmp(name, "Acid S&OP"))      n.pos = { 400, 220 };
        else if (!strcmp(name, "Additive stock")) n.pos = { 60, 190 };
        else {  // unknown node: auto grid on the right
            int col = (int)i % 2, row = (int)i / 2;
            n.pos = { 1620.f + col * 230, 60.f + row * 170 };
        }
        if (n.info.type == T_TRUCK) n.draggable = false;
    }
    float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    for (auto& n : TR.nodes) {
        minx = std::min(minx, n.pos.x); miny = std::min(miny, n.pos.y);
        maxx = std::max(maxx, n.pos.x + n.size.x); maxy = std::max(maxy, n.pos.y + n.size.y);
    }
    float w = maxx - minx + 90, h = maxy - miny + 90;
    float fit = std::min(LY.graph.width / w, LY.graph.height / h);
    UI.zoom_t = UI.zoom = clampd(fit, 0.75f, 1.0f);
    UI.cam_t = UI.cam = { (minx + maxx) / 2, (miny + maxy) / 2 };
}

static Rectangle node_rect(int n) {
    Node& nd = TR.nodes[n];
    Vector2 s = world_to_screen(nd.pos);
    return { s.x, s.y, nd.size.x, nd.size.y };     // fixed size, no zoom scaling
}

// ============================================================================
//                              particles
// ============================================================================

struct Particle { int edge; float u, speed; Color c; };
static std::vector<Particle> PARTS;

static void spawn_edge_particles() {
    if (UI.inflow_cursor.empty()) UI.inflow_cursor.resize(TR.nodes.size(), 0);
    for (size_t n = 0; n < TR.nodes.size(); n++) {
        Node& nd = TR.nodes[n];
        if (nd.info.output < 0) continue;
        auto& cur = UI.inflow_cursor[n];
        while (cur > 0 && nd.inflow[cur].t > UI.t) cur--;              // scrubbed back
        while (cur < nd.inflow.size() && nd.inflow[cur].t <= UI.t) {   // new events
            if (UI.playing && nd.inflow[cur].t > UI.t - 2 * MS_DAY) {
                int count = (int)clampd(nd.inflow[cur].tons / 8.0, 1, 10);
                for (int i = 0; i < count && PARTS.size() < 600; i++)
                    PARTS.push_back({ (int)n, 0.0f, 0.9f + 0.35f * ((i * 2654435761u) % 100) / 100.0f, nd.color });
            }
            cur++;
        }
    }
}

// ============================================================================
//                            node / edge drawing
// ============================================================================

static const char* proc_name(uint8_t s)  { return s == 0 ? "-" : (s == 1 ? "IDLE" : "RUN"); }
static const char* truck_name(uint8_t s) {
    switch (s) { case 0: return "LOAD"; case 1: return "HAUL"; case 2: return "TO PLANT"; default: return "TO MINE"; }
}
static const char* conv_name(uint8_t s)  { return s == 0 ? "MOVING" : "LOADING"; }

static void draw_elem_bar(float x, float y, float w, float h, const float* vals) {
    if (!vals) { DrawRectangle(x, y, w, h, COL_PANEL2); return; }
    double tot = 0; for (int e = 0; e < 6; e++) tot += vals[e];
    if (tot <= 0) { DrawRectangle(x, y, w, h, COL_PANEL2); return; }
    float cx = x;
    for (int e = 0; e < 6; e++) {
        float ww = (float)(vals[e] / tot) * w;
        if (ww > 0) DrawRectangle(cx, y, ww, h, COL_ELEMS[e]);
        cx += ww;
    }
}

// the haul truck road: from under the mine to under the crusher
static void road_endpoints(int mine, int crusher, Vector2* a, Vector2* b, float* y) {
    Rectangle ma = node_rect(mine), ca = node_rect(crusher);
    *a = { ma.x + ma.width * 0.45f, ma.y + ma.height };
    *b = { ca.x + ca.width * 0.45f, ca.y + ca.height };
    *y = std::max(a->y, b->y) + 46;
}

static Vector2 truck_pos(int truck) {
    Node& nd = TR.nodes[truck];
    NodeState& st = FR.st[truck];
    i64 t_change = TR.ticks[0];
    for (auto& sc : nd.state_changes)
        if (sc.field == 1 && sc.t <= UI.t) t_change = sc.t;
    double u = clampd((UI.t - t_change) / (double)MS_HOUR, 0, 1);
    double prog;
    if (st.truck_state == 2) prog = u;
    else if (st.truck_state == 3) prog = 1 - u;
    else if (st.truck_state == 1) prog = 1.0;
    else prog = 0.0;
    int mine = -1, crusher = -1;
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        if (TR.nodes[i].info.type == T_MINER) mine = (int)i;
        if (TR.nodes[i].info.type == T_CRUSHER) crusher = (int)i;
    }
    if (mine < 0 || crusher < 0) return world_to_screen(nd.pos);
    Vector2 a, b; float ry;
    road_endpoints(mine, crusher, &a, &b, &ry);
    return { (float)lerpd((double)a.x, (double)b.x, prog), ry };
}

static void draw_road(int mine, int crusher) {
    Vector2 a, b; float ry;
    road_endpoints(mine, crusher, &a, &b, &ry);
    // drop from mine bottom, run along, rise into crusher bottom
    Vector2 p0 = { a.x, a.y + 6 }, p1 = { a.x, ry }, p2 = { b.x, ry }, p3 = { b.x, b.y + 6 };
    Color rc = { 52, 60, 71, 255 };
    DrawLineEx(p0, p1, 3, rc); DrawLineEx(p1, p2, 3, rc); DrawLineEx(p2, p3, 3, rc);
    // direction chevrons
    for (float x = p1.x + 14; x < p2.x - 16; x += 34) {
        DrawTriangle({ x, ry - 6 }, { x, ry + 6 }, { x + 9, ry }, Fade(COL_DIM, 0.85f));
    }
    // dock stubs into the cards
    DrawLineEx({ a.x - 5, a.y + 6 }, { a.x + 5, a.y + 6 }, 3, rc);
    DrawLineEx({ b.x - 5, b.y + 6 }, { b.x + 5, b.y + 6 }, 3, rc);
}

static void draw_truck(int truck) {
    Node& nd = TR.nodes[truck];
    NodeState& st = FR.st[truck];
    Vector2 tp = truck_pos(truck);

    Rectangle body = { tp.x - 21, tp.y - 15, 42, 22 };
    bool sel = UI.sel == truck, hov = UI.hover == truck;
    DrawRectangleRec(body, COL_PANEL);
    DrawRectangleLinesEx(body, 1.5f, sel ? COL_PLAYHEAD : (hov ? COL_ACCENT : nd.color));
    double load = clampd(st.mass_t() / 100.0, 0, 1);
    if (load > 0.01) DrawRectangle(body.x + 3, body.y + 3 + (body.height - 6) * (1 - (float)load),
                                   body.width - 6, (body.height - 6) * (float)load, Fade(nd.color, 0.65f));
    DrawRectangle(tp.x + 19, tp.y - 8, 12, 15, Fade(nd.color, 0.9f));       // cab
    DrawCircle(tp.x - 11, tp.y + 8, 4, COL_FAINT);
    DrawCircle(tp.x + 11, tp.y + 8, 4, COL_FAINT);
    DrawCircle(tp.x - 11, tp.y + 8, 1.6f, COL_BG);
    DrawCircle(tp.x + 11, tp.y + 8, 1.6f, COL_BG);

    // status bubble above the road, clamped to not hit the cards
    char b[64];
    snprintf(b, sizeof b, "%s  %.0f t", truck_name(st.truck_state), st.mass_t());
    Vector2 ts = measure(F13, b);
    float bx = clampd(tp.x - (ts.x + 12) / 2, LY.graph.x + 4, LY.graph.x + LY.graph.width - ts.x - 16);
    float by = tp.y - 40;
    DrawRectangle(bx, by, ts.x + 12, 18, { 16, 21, 28, 235 });
    DrawRectangleLinesEx({ bx, by, ts.x + 12, 18 }, 1, sel ? COL_PLAYHEAD : COL_BORDER);
    text(F13, bx + 6, by + 2, b, COL_TEXT);
}

static void draw_node(int n) {
    Node& nd = TR.nodes[n];
    NodeState& st = FR.st[n];
    Rectangle r = node_rect(n);
    char b[96];

    DrawRectangleRounded(r, 0.09f, 6, COL_PANEL);
    if (UI.sel == n) {
        float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 4.0f);
        DrawRectangleRoundedLinesEx(r, 0.09f, 6, 2.0f, Fade(COL_PLAYHEAD, 0.6f + 0.4f * pulse));
    } else {
        DrawRectangleRoundedLinesEx(r, 0.09f, 6, 1.2f, UI.hover == n ? COL_ACCENT : COL_BORDER);
    }

    // title row
    float th = 26;
    DrawRectangleRounded({ r.x, r.y, r.width, th + 6 }, 0.09f, 6, Fade(nd.color, 0.13f));
    DrawRectangle(r.x, r.y + th, r.width, 4, COL_PANEL);   // square off the bottom of the strip
    DrawCircle(r.x + 13, r.y + th / 2 + 1, 4, nd.color);
    float name_w = r.width - 26;
    const char* chip_s = nullptr; Color chip_c = COL_DIM;
    if (nd.info.type == T_CRUSHER || nd.info.type == T_FLOTATION) {
        chip_s = proc_name(st.proc_state);
        chip_c = st.proc_state == 2 ? COL_WARN : (st.proc_state == 1 ? COL_DIM : COL_FAINT);
    } else if (nd.info.type == T_CHEM_DISRUPTION) {
        chip_s = st.disrupted_until > UI.t ? "DISRUPTED" : "calm";
        chip_c = st.disrupted_until > UI.t ? COL_BAD : COL_DIM;
    }
    if (chip_s) {
        float cw = measure(F13, chip_s).x + 14;
        chip(r.x + r.width - cw - 6, r.y + 5, chip_s, chip_c);
        name_w = r.width - cw - 30;
    }
    text_trunc(FB16, r.x + 22, r.y + 5, nd.info.name, COL_TEXT, name_w);

    float ix = r.x + 12, iw = r.width - 24, y = r.y + th + 8;

    if (nd.info.type == T_CONVEYOR) {
        int cells = nd.info.belt_cells > 0 ? nd.info.belt_cells : 6;
        float cw = (iw - (cells - 1) * 4) / cells;
        float chh = 30;
        int head = ((st.belt_head % cells) + cells) % cells;
        for (int c = 0; c < cells; c++) {
            float cx = ix + c * (cw + 4);
            double tons = 0; for (int e = 0; e < 6; e++) tons += st.belt(c, e);
            DrawRectangle(cx, y, cw, chh, COL_PANEL2);
            double u = clampd(tons / (double)nd.info.mass_capacity, 0, 1);
            if (tons > 0.01) DrawRectangle(cx + 1, y + chh - 3 - (chh - 6) * (float)u + 1.5f, cw - 2, (chh - 6) * (float)u, Fade(nd.color, 0.7f));
            if (c == head && tons > 0.01) DrawRectangleLinesEx({ cx - 1, y - 3, cw + 2, chh + 6 }, 1.4f, Fade(COL_PLAYHEAD, 0.8f));
            snprintf(b, sizeof b, "%.0f", tons);
            text_r(F13, cx + cw, y + chh + 3, b, tons > 0.01 ? COL_DIM : COL_FAINT);
        }
        y += chh + 20;
        double tot = st.belt_t();
        if (tot > 0.01) {
            snprintf(b, sizeof b, "%s", fmt_tons2(tot).c_str());
            text(F13, ix, y, "belt", COL_FAINT); text_r(F13, ix + iw, y, b, COL_TEXT);
        } else { text(F13, ix, y, "belt empty", COL_FAINT); }
    } else if (nd.info.type == T_TAILINGS) {
        float fh = r.height - th - 20;
        DrawRectangle(ix, y - 2, iw, fh, COL_PANEL2);
        double u = clampd(st.mass_t() / 600000.0, 0, 1);
        if (u > 0.005) DrawRectangle(ix + 2, y - 2 + fh * (1 - (float)u) + 2, iw - 4, fh * (float)u - 4, Fade(nd.color, 0.38f));
        snprintf(b, sizeof b, "%s", fmt_tons2(st.mass_t()).c_str());
        text_r(FB20, ix + iw - 10, y + fh / 2 - 12, b, COL_TEXT);
        text(F13, ix + 8, y + fh / 2 - 10, "dam", COL_FAINT);
    } else if (nd.info.type == T_CHEM_SRC) {
        float fh = r.height - th - 20;
        float tw = 40;
        // tank on the right, values on the left
        DrawRectangle(ix + iw - tw, y - 2, tw, fh, COL_PANEL2);
        double u = clampd(st.chem() / (double)nd.info.mass_capacity, 0, 1);
        if (u > 0.005) DrawRectangle(ix + iw - tw + 2, y - 2 + fh * (1 - (float)u) + 2, tw - 4, fh * (float)u - 4, Fade(nd.color, 0.6f));
        // capacity ticks + target line at 50%
        for (int q = 1; q <= 3; q++) {
            float qy = y - 2 + fh * (1 - q / 4.0f);
            DrawLine(ix + iw - tw, qy, ix + iw - tw + 5, qy, Fade(COL_TEXT, 0.25f));
        }
        float ty = y - 2 + fh * (1 - 0.5f);
        DrawLine(ix + iw - tw, ty, ix + iw, ty, Fade(COL_TEXT, 0.35f));
        float vw = iw - tw - 14;
        snprintf(b, sizeof b, "%s", fmt_tons2(st.chem()).c_str());
        stat_line(ix, y, vw, "tank", b, st.chem() < 50 ? COL_BAD : COL_TEXT, st.chem(), nd.info.mass_capacity, nd.color);
        snprintf(b, sizeof b, "%.0f t", st.chem_burned);
        stat_line(ix, y + 26, vw, "burned", b, COL_TEXT, 0, 0, COL_DIM);
        snprintf(b, sizeof b, "%d cars", st.chem_cars);
        text(F13, ix, y + 52, "contract", COL_FAINT); text_r(F13, ix + vw, y + 53, b, COL_TEXT);
        if (st.disrupted_until > UI.t) {
            chip(ix, y + 72, "MARKET DISRUPTED", COL_BAD);
        } else {
            snprintf(b, sizeof b, "%s", fmt_dur(st.next_tick - (i64)UI.t).c_str());
            text(F13, ix, y + 72, "next delivery", COL_FAINT); text_r(F13, ix + vw, y + 73, b, COL_DIM);
        }
    } else if (nd.info.type == T_CHEM_SCRUM) {
        int sup = -1;
        for (size_t i = 0; i < TR.nodes.size(); i++) if (TR.nodes[i].info.type == T_CHEM_SRC) sup = (int)i;
        snprintf(b, sizeof b, "%d cars", sup >= 0 ? FR.st[sup].chem_cars : 0);
        text(F13, ix, y, "contract", COL_FAINT); text_r(F13, ix + iw, y, b, COL_TEXT); y += 22;
        snprintf(b, sizeof b, "%s ago", fmt_dur((i64)UI.t - st.chem_last_scrum).c_str());
        text(F13, ix, y, "last scrum", COL_FAINT); text_r(F13, ix + iw, y, b, COL_DIM); y += 22;
        snprintf(b, sizeof b, "%s", fmt_tons2(st.chem_burned_at_scrum).c_str());
        text(F13, ix, y, "burn @ scrum", COL_FAINT); text_r(F13, ix + iw, y, b, COL_DIM); y += 22;
        snprintf(b, sizeof b, "next %s", fmt_next(st.next_tick, UI.t).c_str());
        text(F13, ix, y, b, COL_ACCENT);
    } else if (nd.info.type == T_CHEM_DISRUPTION || nd.info.type == TRACE_NODE_ADDITIVE_STOCK) {
        if (nd.info.type == TRACE_NODE_ADDITIVE_STOCK) {
            if (st.mass_t() > 0.01) {
                snprintf(b, sizeof b, "%s", fmt_tons2(st.mass_t()).c_str());
                text(F13, ix, y, "imported", COL_FAINT); text_r(F13, ix + iw, y, b, COL_TEXT);
                draw_elem_bar(ix, y + 22, iw, 6, st.mass);
            } else {
                text(F13, ix, y, "no deliveries yet", COL_FAINT);
                text(F13, ix, y + 17, "awaiting consumer", COL_FAINT);
            }
        } else {
            if (st.disrupted_until > UI.t) {
                snprintf(b, sizeof b, "%s left", fmt_dur(st.disrupted_until - (i64)UI.t).c_str());
                chip(ix, y, b, COL_BAD);
            } else {
                snprintf(b, sizeof b, "calm · next %s", fmt_dur(st.next_tick - (i64)UI.t).c_str());
                text_trunc(F13, ix, y, b, COL_FAINT, iw);
            }
        }
    } else {
        // generic processor card: stock/wip/out + elements + footer
        double vw = iw;
        snprintf(b, sizeof b, "%s", fmt_tons2(st.mass_t()).c_str());
        stat_line(ix, y, vw, "stock", b, COL_TEXT, st.mass_t(),
                  std::isfinite(nd.info.mass_capacity) ? nd.info.mass_capacity : 250, nd.color); y += 27;
        bool is_proc = (nd.info.type == T_CRUSHER || nd.info.type == T_FLOTATION);
        if (st.wip_t() > 0.005 || is_proc) {
            snprintf(b, sizeof b, "%s", fmt_tons2(st.wip_t()).c_str());
            stat_line(ix, y, vw, "wip", b, st.wip_t() > 0 ? COL_WARN : COL_FAINT, st.wip_t(), 6000, COL_WARN); y += 27;
        }
        if (st.out_t() > 0.005) {
            snprintf(b, sizeof b, "%s", fmt_tons2(st.out_t()).c_str());
            stat_line(ix, y, vw, "product", b, COL_GOOD, st.out_t(), 80000, COL_GOOD); y += 27;
        }
        if (st.mass_t() > 0.01 || st.wip_t() > 0.01) {
            draw_elem_bar(ix, y + 2, iw, 6, st.mass_t() > 0.01 ? st.mass : (const float*)nullptr);
            y += 14;
        }
        double tot = st.mass_t(), reo = 0;
        for (int e = 0; e < 5; e++) reo += st.m(e);
        snprintf(b, sizeof b, "grade %.1f%%", tot > 1 ? 100 * reo / tot : 0.0);
        float gw = measure(F13, b).x;
        snprintf(b, sizeof b, "next %s", fmt_next(st.next_tick, UI.t).c_str());
        float nw = measure(F13, b).x;
        if (gw + nw + 12 < iw) {
            text(F13, ix, y + 2, "grade", COL_FAINT);
            text(F13, ix + 44, y + 2, b, st.next_tick > UI.t ? COL_ACCENT : COL_FAINT);
        } else {   // too tight: two lines
            char b2[64];
            snprintf(b2, sizeof b2, "grade %.1f%%", tot > 1 ? 100 * reo / tot : 0.0);
            text(F13, ix, y + 2, b2, COL_FAINT);
            text(F13, ix, y + 17, b, st.next_tick > UI.t ? COL_ACCENT : COL_FAINT);
        }
    }

    // subtle ring when the node ticked at the current tick
    if (((TR.ticked[UI.k] >> n) & 1))
        DrawRectangleRoundedLinesEx({ r.x - 3, r.y - 3, r.width + 6, r.height + 6 }, 0.09f, 6, 1.3f, Fade(COL_TEXT, 0.18f));
}

// 3-segment orthogonal route from src right edge to dst left edge
static void edge_route(int from, int to, Vector2 pts[4]) {
    Rectangle a = node_rect(from), b = node_rect(to);
    Vector2 p0 = { a.x + a.width, a.y + a.height * 0.42f };
    Vector2 p3 = { b.x, b.y + b.height * 0.42f };
    float mx = (p0.x + p3.x) / 2;
    pts[0] = p0; pts[1] = { mx, p0.y }; pts[2] = { mx, p3.y }; pts[3] = p3;
}

static Vector2 route_point(Vector2 pts[4], float u) {
    if (u <= 0) return pts[0];
    if (u >= 1) return pts[3];
    float lens[3], tot = 0;
    for (int i = 0; i < 3; i++) { lens[i] = Vector2Distance(pts[i], pts[i + 1]); tot += lens[i]; }
    float d = u * tot;
    for (int i = 0; i < 3; i++) {
        if (d <= lens[i] || i == 2) {
            float uu = lens[i] > 0 ? d / lens[i] : 0;
            return { (float)lerpd((double)pts[i].x, (double)pts[i + 1].x, (double)uu),
                     (float)lerpd((double)pts[i].y, (double)pts[i + 1].y, (double)uu) };
        }
        d -= lens[i];
    }
    return pts[3];
}

static void draw_edge(int from, int to, Color c, float thick, bool dotted) {
    Vector2 pts[4];
    edge_route(from, to, pts);
    if (dotted) {
        for (int s = 0; s < 3; s++) {
            float len = Vector2Distance(pts[s], pts[s + 1]);
            if (len < 1) continue;
            Vector2 d = { (pts[s + 1].x - pts[s].x) / len, (pts[s + 1].y - pts[s].y) / len };
            for (float u = 0; u < len; u += 18)
                DrawLineEx({ pts[s].x + d.x * u, pts[s].y + d.y * u },
                           { pts[s].x + d.x * std::min(u + 3, len), pts[s].y + d.y * std::min(u + 3, len) }, thick, c);
        }
    } else {
        DrawLineEx(pts[0], pts[1], thick, c);
        DrawLineEx(pts[1], pts[2], thick, c);
        DrawLineEx(pts[2], pts[3], thick, c);
        Vector2 dir = Vector2Normalize(Vector2Subtract(pts[3], pts[2]));
        Vector2 perp = { -dir.y, dir.x };
        float as = 6 + thick;
        DrawTriangle(pts[3],
                     { pts[3].x - dir.x * as - perp.x * as * 0.55f, pts[3].y - dir.y * as - perp.y * as * 0.55f },
                     { pts[3].x - dir.x * as + perp.x * as * 0.55f, pts[3].y - dir.y * as + perp.y * as * 0.55f }, c);
    }
}

// ============================================================================
//                              timeline drawing
// ============================================================================

static double tl_x_to_t(float x) {
    return UI.vw0 + (x - LY.timeline.x) / LY.timeline.width * (UI.vw1 - UI.vw0);
}
static float tl_t_to_x(double t) {
    return LY.timeline.x + (float)((t - UI.vw0) / (UI.vw1 - UI.vw0)) * LY.timeline.width;
}

static const Color MARK_COL[N_KINDS] = {
    { 190, 145, 45, 255 }, COL_ACCENT, { 235, 130, 60, 255 }, COL_BAD, { 34, 190, 210, 255 }, COL_GOOD
};
static const char* MARK_NAME[N_KINDS] = {
    "pad empty", "acid low", "flot starved", "acid disruption", "S&OP scrum", "campaign"
};

static void draw_timeline() {
    Rectangle r = LY.timeline;
    DrawRectangleRec(r, COL_PANEL);
    DrawRectangleLinesEx(r, 1, COL_BORDER);

    float chart_h = r.height - 92;
    float chart_y = r.y + 10;
    i64 t_end = TR.t_end;
    float y0 = chart_y + 4, ch = chart_h - 22;

    // ---- horizontal gridlines ------------------------------------------------
    for (int i = 1; i <= 3; i++)
        DrawLine(r.x, y0 + ch * i / 4, r.x + r.width, y0 + ch * i / 4, Fade(COL_BORDER, 0.35f));

    // ---- vertical grid + time labels -------------------------------------------
    double span = UI.vw1 - UI.vw0;
    double steps[] = { MS_HOUR, 6 * MS_HOUR, MS_DAY, 7 * MS_DAY, 30 * MS_DAY, 90 * MS_DAY, 365 * MS_DAY };
    double step = steps[6];
    for (double s : steps) if (s / span * LY.timeline.width > 110) { step = s; break; }
    if (365.0 * MS_DAY / span * LY.timeline.width < 80) step = 2 * 365 * MS_DAY;
    for (double t = ceil(UI.vw0 / step) * step; t < UI.vw1; t += step) {
        float x = tl_t_to_x(t);
        DrawLine(x, chart_y, x, chart_y + chart_h, Fade(COL_BORDER, 0.5f));
        char b[32];
        double days = t / MS_DAY;
        if (step >= 90 * MS_DAY) snprintf(b, sizeof b, "Y%d D%03d", (int)(days / 365) + 1, (int)days % 365 + 1);
        else snprintf(b, sizeof b, "D%03d", (int)days);
        if (x > r.x + 2 && x < r.x + r.width - 60)
            text(F13, x + 3, chart_y + chart_h - 14, b, COL_FAINT);
    }

    // ---- node stock envelopes, each normalized to its own max -----------------
    for (size_t n = 0; n < TR.nodes.size(); n++) {
        Node& nd = TR.nodes[n];
        double nmax = 0, nmin = 1e30;
        for (int b = 0; b < NB; b++) {
            nmax = std::max(nmax, (double)nd.series[b].mass.mx);
            nmin = std::min(nmin, (double)nd.series[b].mass.mn);
        }
        if (nmax < 1) continue;
        if ((nmax - nmin) / nmax < 0.01 && (int)n != UI.sel) continue;   // flat line: reads as a border
        bool strong = (int)n == UI.sel;
        for (int px = (int)r.x; px < (int)(r.x + r.width); px++) {
            double ta = tl_x_to_t((float)px), tb = tl_x_to_t((float)(px + 1));
            int b0 = (int)clampd(ta / t_end * NB, 0, NB - 1);
            int b1 = (int)clampd(tb / t_end * NB, 0, NB - 1);
            float mn = 1e30f, mx = -1e30f;
            for (int b = b0; b <= b1; b++) {
                mn = std::min(mn, nd.series[b].mass.mn);
                mx = std::max(mx, nd.series[b].mass.mx);
            }
            if (mx < 0) continue;
            float yn = y0 + ch * (1 - (float)(mn / nmax) * 0.94f);
            float yx = y0 + ch * (1 - (float)(mx / nmax) * 0.94f);
            float hgt = std::max(1.0f, yx - yn + 1);
            // a hairline glued to the baseline reads as a stray axis rule — skip it
            if (hgt <= 2.5f && yn > y0 + ch - 4) continue;
            DrawRectangle((float)px, yn, 1, hgt, strong ? Fade(nd.color, 0.95f) : Fade(nd.color, 0.55f));
        }
        // selected: current value tag at the playhead
        if (strong) {
            double v = FR.st[n].mass_t();
            double u = clampd(v / nmax, 0, 1);
            DrawCircle(tl_t_to_x(UI.t), y0 + ch * (1 - (float)u * 0.94f), 3.5f, nd.color);
        }
    }

    // ---- right-edge series labels at their current value ------------------------
    {
        struct Lbl { float y; Color c; const char* name; };
        Lbl lbls[TRACE_MAX_NODES];
        int nl = 0;
        for (size_t n = 0; n < TR.nodes.size(); n++) {
            Node& nd = TR.nodes[n];
            double nmax = 0, nmin = 1e30;
            for (int b = 0; b < NB; b++) {
                nmax = std::max(nmax, (double)nd.series[b].mass.mx);
                nmin = std::min(nmin, (double)nd.series[b].mass.mn);
            }
            if (nmax < 1 || (nmax - nmin) / nmax < 0.01) continue;
            double v = FR.st[n].mass_t();
            float yy = y0 + ch * (1 - (float)clampd(v / nmax, 0, 1) * 0.94f) - 6;
            lbls[nl++] = { yy, nd.color, nd.info.name };
        }
        std::sort(lbls, lbls + nl, [](const Lbl& a, const Lbl& b) { return a.y < b.y; });
        float next_free = -100;
        for (int i = 0; i < nl; i++) {
            if (lbls[i].y < next_free + 13) lbls[i].y = next_free + 13;   // nudge apart
            next_free = lbls[i].y;
            float tw = measure(F13, lbls[i].name).x + 8;
            float lx = r.x + r.width - tw - 6;
            DrawRectangle(lx - 2, lbls[i].y - 1, tw + 4, 13, Color{ 15, 19, 25, 210 });
            text(F13, lx, lbls[i].y, lbls[i].name, lbls[i].c);
        }
    }

    // ---- mark lanes -------------------------------------------------------------
    float lane_y = chart_y + chart_h + 8;
    float lane_h = 4, lane_gap = 4;
    for (auto& mk : TR.marks) {
        float x = tl_t_to_x((double)mk.t0);
        float x2 = tl_t_to_x((double)mk.t1);
        float y = lane_y + mk.kind * (lane_h + lane_gap);
        if (mk.t0 == mk.t1) DrawRectangle(x - 0.5f, y, 1.5f, lane_h, Fade(MARK_COL[mk.kind], 0.9f));
        else DrawRectangle(x, y, std::max(1.5f, x2 - x), lane_h + (mk.kind == N_DISRUPTED ? 2 : 0),
                           Fade(MARK_COL[mk.kind], mk.kind == N_DISRUPTED ? 0.65f : 0.34f));
    }

    // ---- caption + legend ---------------------------------------------------------
    float cap_y = lane_y + N_KINDS * (lane_h + lane_gap) + 5;
    const char* caption = "drag to scrub · wheel zoom · dblclick fit";
    text(F13, r.x + 10, cap_y, caption, COL_FAINT);
    float lx = r.x + 10 + measure(F13, caption).x + 22;
    for (int k = 0; k < N_KINDS; k++) {
        bool used = false;
        for (auto& mk : TR.marks) if (mk.kind == k) { used = true; break; }
        if (!used) continue;
        if (lx + measure(F13, MARK_NAME[k]).x + 18 > r.x + r.width) break;
        DrawRectangle(lx, cap_y + 4, 9, 3, Fade(MARK_COL[k], 0.85f));
        text(F13, lx + 14, cap_y, MARK_NAME[k], COL_FAINT);
        lx += measure(F13, MARK_NAME[k]).x + 28;
    }

    // ---- hover crosshair + tooltip ------------------------------------------------
    bool hover_tl = CheckCollisionPointRec(UI.mouse, r) && !UI.scrubbing;
    if (hover_tl) {
        double ht = tl_x_to_t(UI.mouse.x);
        float hx = tl_t_to_x(ht);
        DrawLine(hx, chart_y, hx, chart_y + chart_h, Fade(COL_TEXT, 0.3f));
        int k = tick_at(ht);
        NodeState hs[TRACE_MAX_NODES];
        for (size_t n = 0; n < TR.nodes.size(); n++) node_state_at((int)n, k, hs[n]);
        char buf[1024];
        int w = snprintf(buf, sizeof buf, "%s   tick %d\n", fmt_time(TR.ticks[k]).c_str(), k);
        for (size_t n = 0; n < TR.nodes.size() && w < 950; n++)
            w += snprintf(buf + w, sizeof buf - w, "%-16s %10.0f t\n", TR.nodes[n].info.name,
                           hs[n].mass_t() + hs[n].wip_t() + hs[n].out_t() + hs[n].belt_t());
        snprintf(UI.tooltip, sizeof UI.tooltip, "%s", buf);
    }

    // ---- minimap when zoomed --------------------------------------------------------
    float mm_y = cap_y + 15;
    float mm_h = 8;
    if (UI.vw1 - UI.vw0 < t_end * 0.995) {
        LY.tl_minimap = { r.x, mm_y, r.width, mm_h };
        DrawRectangle(r.x, mm_y, r.width, mm_h, COL_PANEL2);
        DrawRectangleLinesEx(LY.tl_minimap, 1, COL_BORDER);
        // faint full-run silhouette of the selected node
        float wx0 = r.x + (float)(UI.vw0 / t_end) * r.width;
        float wx1 = r.x + (float)(UI.vw1 / t_end) * r.width;
        DrawRectangle(wx0, mm_y + 1, std::max(3.0f, wx1 - wx0), mm_h - 2, Fade(COL_ACCENT, 0.45f));
        static bool mm_drag = false;
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(UI.mouse, LY.tl_minimap))
            mm_drag = true;
        if (!IsMouseButtonDown(MOUSE_LEFT_BUTTON)) mm_drag = false;
        if (mm_drag) {
            double sp = UI.vw1t - UI.vw0t;
            double c = (UI.mouse.x - r.x) / r.width * t_end;
            UI.vw0t = clampd(c - sp / 2, 0, t_end - sp);
            UI.vw1t = UI.vw0t + sp;
        }
    } else {
        LY.tl_minimap = { -1, -1, -1, -1 };
    }

    // ---- playhead with time flag -----------------------------------------------------
    float px = tl_t_to_x(UI.t);
    DrawRectangle(px - 1, chart_y, 2, chart_y + chart_h + 8 + N_KINDS * (lane_h + lane_gap) - chart_y, Fade(COL_PLAYHEAD, 0.95f));
    char fb[32];
    snprintf(fb, sizeof fb, "Y%d D%03d", (int)(UI.t / MS_DAY / 365) + 1, (int)(UI.t / MS_DAY) % 365 + 1);
    Vector2 fts = measure(F13, fb);
    float fx = clampd(px - (fts.x + 10) / 2, r.x + 2, r.x + r.width - fts.x - 12);
    DrawRectangle(fx, r.y, fts.x + 10, 17, COL_PLAYHEAD);
    DrawTriangle({ px - 4, r.y + 20 }, { px, r.y + 13 }, { px + 4, r.y + 20 }, COL_PLAYHEAD);
    text(F13, fx + 5, r.y + 2, fb, Color{ 40, 30, 8, 255 });
    DrawTriangle({ px - 5, r.y + r.height - 1 }, { px, r.y + r.height - 9 }, { px + 5, r.y + r.height - 1 }, COL_PLAYHEAD);
}

// ============================================================================
//                                inspector
// ============================================================================

// chart with a legend line above it and day labels below
static void draw_series_chart(Rectangle r, int n, double t0, double t1) {
    Node& nd = TR.nodes[n];
    struct SRef { Series NodeSeries::* s; Color c; const char* name; };
    SRef refs[4] = {
        { &NodeSeries::mass, nd.color, "stock" },
        { &NodeSeries::wip, COL_WARN, "wip" },
        { &NodeSeries::out, COL_GOOD, "product" },
        { &NodeSeries::chem, COL_ACCENT, "acid" },
    };
    double maxs[4] = {};
    for (int ri = 0; ri < 4; ri++)
        for (int b = 0; b < NB; b++)
            maxs[ri] = std::max(maxs[ri], (double)(nd.series[b].*refs[ri].s).mx);

    // legend above: colored dash + name + max
    float lx = r.x;
    for (int ri = 0; ri < 4; ri++) {
        if (maxs[ri] < 0.01) continue;
        char b[48];
        if (maxs[ri] >= 10000)      snprintf(b, sizeof b, "%.0fk", maxs[ri] / 1000);
        else if (maxs[ri] >= 1000) snprintf(b, sizeof b, "%.1fk", maxs[ri] / 1000);
        else                        snprintf(b, sizeof b, "%.0f", maxs[ri]);
        char b2[64]; snprintf(b2, sizeof b2, "%s max %s", refs[ri].name, b);
        DrawRectangle(lx, r.y - 13, 10, 2.5f, refs[ri].c);
        text(F13, lx + 15, r.y - 21, b2, COL_FAINT);
        lx += 15 + measure(F13, b2).x + 14;
    }

    DrawRectangleRec(r, COL_PANEL2);
    for (int i = 0; i <= 4; i++)
        DrawLine(r.x, r.y + 3 + (r.height - 6) * i / 4, r.x + r.width, r.y + 3 + (r.height - 6) * i / 4, Fade(COL_BORDER, 0.3f));
    for (int ri = 0; ri < 4; ri++) {
        if (maxs[ri] < 0.01) continue;
        float px_prev = -1, y_prev = -1;
        for (int px = 0; px <= (int)r.width; px++) {
            double t = t0 + (double)px / r.width * (t1 - t0);
            int b = (int)clampd(t / TR.t_end * NB, 0, NB - 1);
            float y = r.y + r.height - 3 - (float)(((nd.series[b].*refs[ri].s).last / maxs[ri]) * (r.height - 8));
            if (px_prev >= 0) DrawLineEx({ px_prev, y_prev }, { r.x + px, y }, 1.6f, refs[ri].c);
            px_prev = r.x + px; y_prev = y;
        }
    }
    float px = r.x + (float)((UI.t - t0) / (t1 - t0)) * r.width;
    if (px >= r.x && px <= r.x + r.width) DrawLine(px, r.y, px, r.y + r.height, Fade(COL_PLAYHEAD, 0.95f));
    DrawRectangleLinesEx(r, 1, COL_BORDER);

    // day labels below
    char b[32];
    snprintf(b, sizeof b, "D%lld", (long long)(t0 / MS_DAY));
    text(F13, r.x, r.y + r.height + 4, b, COL_FAINT);
    snprintf(b, sizeof b, "D%lld", (long long)(t1 / MS_DAY));
    text_r(F13, r.x + r.width, r.y + r.height + 4, b, COL_FAINT);
}

static void draw_state_strip(Rectangle r, int n) {
    DrawRectangleRec(r, COL_PANEL2);
    Node& nd = TR.nodes[n];
    bool has[3] = {};
    for (auto& sc : nd.state_changes) has[sc.field] = true;
    int rows = has[0] + has[1] + has[2];
    if (rows == 0) {
        // no state machine: show arrival events instead
        if (nd.inflow.empty()) { text(F13, r.x + 6, r.y + 4, "no state changes", COL_FAINT); return; }
        DrawRectangleRec(r, COL_PANEL2);
        for (auto& ev : nd.inflow) {
            float x = r.x + (float)((double)ev.t / TR.t_end) * r.width;
            DrawRectangle(x - 0.5f, r.y + 6, 1.5f, r.height - 12, Fade(nd.color, 0.8f));
        }
        char b[96];
        snprintf(b, sizeof b, "%zu arrivals", nd.inflow.size());
        text(F13, r.x + 6, r.y + r.height / 2 - 7, b, COL_TEXT);
        float px = r.x + (float)((double)UI.t / TR.t_end) * r.width;
        DrawLine(px, r.y, px, r.y + r.height, Fade(COL_PLAYHEAD, 0.95f));
        return;
    }

    const char* row_names[3] = { "proc", "truck", "conv" };
    int f_drawn = -1;
    for (int f = 0; f < 3; f++) {
        if (!has[f]) continue;
        f_drawn++;
        float y = r.y + 2 + f_drawn * (r.height / rows);
        float h = r.height / rows - 3;
        i64 t_prev = TR.ticks[0];
        uint8_t v_prev = 0;
        static const Color TRACK = { 36, 43, 54, 255 };
        auto seg_col = [&](uint8_t v) {
            if (f == 1) return v >= 2 ? &COL_ACCENT : (v == 1 ? &COL_GOOD : &TRACK);
            return v == 2 ? &COL_WARN : (v == 1 ? &TRACK : &COL_PANEL2);
        };
        for (auto& sc : nd.state_changes) {
            if (sc.field != f) continue;
            float x0 = r.x + (float)((double)t_prev / TR.t_end) * r.width;
            float x1 = r.x + (float)((double)sc.t / TR.t_end) * r.width;
            DrawRectangle(x0, y, std::max(0.5f, x1 - x0), h, Fade(*seg_col(v_prev), 0.8f));
            t_prev = sc.t; v_prev = sc.value;
        }
        float x0 = r.x + (float)((double)t_prev / TR.t_end) * r.width;
        DrawRectangle(x0, y, r.x + r.width - x0, h, Fade(*seg_col(v_prev), 0.8f));
    }
    (void)row_names;
    float px = r.x + (float)((double)UI.t / TR.t_end) * r.width;
    DrawLine(px, r.y, px, r.y + r.height, Fade(COL_PLAYHEAD, 0.95f));
}

static void draw_inspector() {
    Rectangle r = LY.inspector;
    DrawRectangleRec(r, COL_PANEL);
    DrawRectangleLinesEx(r, 1, COL_BORDER);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);

    float x = r.x + 16, y = r.y + 14 - UI.insp_scroll;
    float iw = r.width - 32;
    char b[160];

    if (UI.sel < 0) {
        text(FB20, x, y, "Inspector", COL_TEXT); y += 30;
        text(F16, x, y, "click a node in the graph, or a table row", COL_DIM); y += 42;

        text(FB16, x, y, "SYSTEM INVENTORY · ELEMENTS IN ALL STOCKS", COL_TEXT); y += 22;
        double elem[6] = {};
        for (size_t n = 0; n < TR.nodes.size(); n++)
            for (int e = 0; e < 6; e++)
                elem[e] += FR.st[n].mass[e] + FR.st[n].mass[6 + e] + FR.st[n].mass[12 + e];
        double tot = 0; for (double e : elem) tot += e;
        float cx = x;
        for (int e = 0; e < 6; e++) {
            float w = tot > 0 ? (float)(elem[e] / tot) * iw : 0;
            DrawRectangle(cx, y, w, 14, COL_ELEMS[e]); cx += w;
        }
        y += 24;
        for (int e = 0; e < 6; e++) {
            snprintf(b, sizeof b, "%-5s", ELEM_NAMES[e]);
            text(F13, x, y, b, e < 5 ? COL_ELEMS[e] : COL_DIM);
            snprintf(b, sizeof b, "%12.0f t", elem[e]);
            text_r(F13, x + iw * 0.55f, y, b, COL_TEXT);
            snprintf(b, sizeof b, "%5.1f%%", tot > 0 ? 100 * elem[e] / tot : 0);
            text_r(F13, x + iw, y, b, COL_DIM);
            y += 17;
        }
        y += 14;
        text(FB16, x, y, "CONTROLS", COL_TEXT); y += 22;
        const char* lines[] = {
            "space  play/pause      <>  step one tick",
            "shift + <>  step one day",
            "PgUp/PgDn  jump to notable events",
            "[ ]  playback speed    R  reload trace",
            "F11 fullscreen   H help   Esc quit",
            "",
            "timeline: drag = scrub, wheel = zoom,",
            "shift+wheel = pan, dbl-click = fit",
            "graph: click node, drag node/bg, wheel",
        };
        for (auto& l : lines) { text(F13, x, y, l, COL_DIM); y += 17; }
        UI.insp_h = 0;
        EndScissorMode();
        return;
    }

    Node& nd = TR.nodes[UI.sel];
    NodeState& st = FR.st[UI.sel];

    // header
    DrawCircle(x + 8, y + 10, 6, nd.color);
    text_trunc(FB20, x + 24, y, nd.info.name, COL_TEXT, iw - 24); y += 28;
    snprintf(b, sizeof b, "in: %s   out: %s",
             nd.info.input >= 0 ? TR.nodes[nd.info.input].info.name : "-",
             nd.info.output >= 0 ? TR.nodes[nd.info.output].info.name : "-");
    text_trunc(F13, x, y, b, COL_FAINT, iw); y += 24;

    // state chips
    text(FB16, x, y, "STATE", COL_TEXT); y += 20;
    float cx2 = x;
    if (nd.info.type == T_CRUSHER || nd.info.type == T_FLOTATION)
        cx2 += chip(cx2, y, proc_name(st.proc_state), st.proc_state == 2 ? COL_WARN : COL_DIM) + 8;
    if (nd.info.type == T_TRUCK)
        cx2 += chip(cx2, y, truck_name(st.truck_state), st.truck_state >= 2 ? COL_ACCENT : COL_GOOD) + 8;
    if (nd.info.type == T_CHEM_DISRUPTION && st.disrupted_until > UI.t)
        cx2 += chip(cx2, y, "DISRUPTED", COL_BAD) + 8;
    snprintf(b, sizeof b, "next event in %s", fmt_next(st.next_tick, UI.t).c_str());
    text_r(F13, x + iw, y + 1, b, COL_ACCENT);
    y += 30;

    // values
    text(FB16, x, y, "VALUES", COL_TEXT); y += 20;
    auto vline = [&](const char* name, double v) {
        text(F13, x, y, name, COL_FAINT);
        snprintf(b, sizeof b, "%s", fmt_tons2(v, "").c_str());
        text_r(F13, x + iw, y, b, COL_TEXT);
        y += 17;
    };
    vline("stock (t)", st.mass_t());
    vline("wip (t)", st.wip_t());
    vline("product (t)", st.out_t());
    if (nd.info.belt_cells > 0) vline("belt (t)", st.belt_t());
    if (nd.info.type == T_CHEM_SRC) vline("acid tank (t)", st.chem());
    vline("total (t)", st.level());
    vline("arrived to date (t)", cum_in_at(nd, UI.t));
    y += 10;

    // element split
    if (st.mass_t() > 0.01) {
        text(FB16, x, y, "STOCK ELEMENT SPLIT", COL_TEXT); y += 20;
        draw_elem_bar(x, y, iw, 10, st.mass); y += 16;
        double mt = st.mass_t();
        for (int e = 0; e < 6; e++) {
            text(F13, x, y, ELEM_NAMES[e], COL_ELEMS[e]);
            snprintf(b, sizeof b, "%s", fmt_tons2(st.m(e)).c_str());
            text_r(F13, x + iw * 0.6f, y, b, COL_TEXT);
            double pct = mt > 0 ? 100 * st.m(e) / mt : 0;
            snprintf(b, sizeof b, pct < 0.1 && pct > 0 ? "%.2f%%" : "%.1f%%", pct);
            text_r(F13, x + iw, y, b, COL_DIM);
            y += 16;
        }
        y += 8;
    }

    // history (the timeline itself is the zoomable view — this is the overview)
    text(FB16, x, y, "HISTORY · WHOLE RUN", COL_TEXT); y += 24;    // heading row
    y += 18;                                                        // legend row (drawn by chart)
    draw_series_chart({ x, y, iw, 78 }, UI.sel, 0, TR.t_end); y += 78 + 20;

    text(FB16, x, y, "STATE OVER TIME", COL_TEXT);
    text(F13, x + 150, y + 2, "rows: proc · truck · conv", COL_FAINT);
    y += 19;
    draw_state_strip({ x, y, iw, 34 }, UI.sel); y += 46;

    if (button({ x, y, 140, 26 }, "< prev change", F13)) {
        i64 best = -1;
        for (auto& sc : nd.state_changes) if (sc.t < UI.t - 1) best = std::max(best, sc.t);
        if (best >= 0) UI.t = best + 1;
    }
    if (button({ x + 150, y, 140, 26 }, "next change >", F13)) {
        for (auto& sc : nd.state_changes) if (sc.t > UI.t + 1) { UI.t = sc.t + 1; break; }
    }
    y += 34;

    // content height + scrollbar
    UI.insp_h = y + UI.insp_scroll - r.y;
    float visible = r.height - 20;
    if (UI.insp_h > visible) {
        float track_h = r.height - 8;
        float thumb_h = std::max(30.0f, track_h * visible / UI.insp_h);
        float thumb_y = r.y + 4 + (track_h - thumb_h) * UI.insp_scroll / (UI.insp_h - visible);
        DrawRectangle(r.x + r.width - 5, r.y + 4, 3, track_h, Fade(COL_BORDER, 0.4f));
        DrawRectangle(r.x + r.width - 5, thumb_y, 3, thumb_h, Fade(COL_FAINT, 0.7f));
    }
    EndScissorMode();
}

// ============================================================================
//                                  table
// ============================================================================

static void draw_table() {
    Rectangle r = LY.table;
    DrawRectangleRec(r, COL_PANEL);
    DrawLine(r.x, r.y, r.x + r.width, r.y, COL_BORDER);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);

    // columns as cumulative x offsets (label column widths sized to content)
    struct Col { const char* head; float w; bool right; };
    Col cols[] = {
        { "",       40,  false },   // color chip
        { "node",   190, false },
        { "state",  110, false },
        { "stock t",100, true  },
        { "grade %", 76, true  },
        { "wip t",   86, true  },
        { "product t", 104, true },
        { "next event", 96, true },
        { "t / 24h", 84, true },
    };
    const int NC = 9;
    float colx[NC];
    float x0 = r.x + 16;
    colx[0] = x0;
    for (int i = 1; i < NC; i++) colx[i] = colx[i - 1] + cols[i - 1].w;

    float y = r.y + 10;
    for (int i = 0; i < NC; i++) {
        if (cols[i].right) text_r(F13, colx[i] + cols[i].w - 6, y, cols[i].head, COL_FAINT);
        else text(F13, colx[i], y, cols[i].head, COL_FAINT);
    }
    DrawLine(r.x, y + 16, r.x + r.width, y + 16, Fade(COL_BORDER, 0.6f));
    y += 22;
    float rh = 19;

    static NodeState s24[TRACE_MAX_NODES];
    int k24 = tick_at(std::max(0.0, UI.t - MS_DAY));
    if (k24 != UI.s24_k) {
        for (size_t n = 0; n < TR.nodes.size(); n++) node_state_at((int)n, k24, s24[n]);
        UI.s24_k = k24;
        memcpy(UI.s24, s24, sizeof(s24));
    }

    int rows = std::min((int)((r.height - 48) / rh), (int)TR.nodes.size());
    for (int n = 0; n < rows; n++) {
        Rectangle row_r = { r.x, y - 2, r.width, rh };
        bool hover = CheckCollisionPointRec(UI.mouse, row_r);
        if (n == UI.sel) DrawRectangleRec(row_r, Fade(COL_ACCENT, 0.12f));
        else if (hover) DrawRectangleRec(row_r, Fade(COL_TEXT, 0.045f));
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && hover) UI.sel = n;

        NodeState& st = FR.st[n];
        Node& nd = TR.nodes[n];
        char b[64];

        DrawRectangle(colx[0], y + 4, 10, 10, nd.color);
        text_trunc(F13, colx[1], y, nd.info.name, COL_TEXT, cols[1].w - 12);

        const char* sname = proc_name(st.proc_state);
        if (nd.info.type == T_TRUCK) sname = truck_name(st.truck_state);
        else if (nd.info.type == T_CONVEYOR) sname = conv_name(st.conv_state);
        else if (nd.info.type == T_CHEM_DISRUPTION) sname = st.disrupted_until > UI.t ? "DISRUPTED" : "calm";
        else if (nd.info.type == T_CHEM_SRC) sname = "supplying";
        else if (nd.info.type == T_CHEM_SCRUM) sname = "S&OP";
        else if (nd.info.type == T_TAILINGS || nd.info.type == TRACE_NODE_ADDITIVE_STOCK) sname = "store";
        else if (nd.info.type == T_MINER) sname = "mining";
        bool bad_state = !strcmp(sname, "DISRUPTED");
        text_trunc(F13, colx[2], y, sname, bad_state ? COL_BAD : COL_DIM, cols[2].w - 12);

        snprintf(b, sizeof b, "%s", fmt_tons2(st.mass_t() + st.belt_t(), "").c_str());
        text_r(F13, colx[3] + cols[3].w - 6, y, b, COL_TEXT);
        double tot = st.mass_t() + st.belt_t(), reo = 0;
        for (int e = 0; e < 5; e++) reo += st.m(e);
        if (tot > 1) snprintf(b, sizeof b, "%.1f", 100 * reo / tot);
        else snprintf(b, sizeof b, "-");
        text_r(F13, colx[4] + cols[4].w - 6, y, b, COL_DIM);
        snprintf(b, sizeof b, "%s", fmt_tons2(st.wip_t(), "").c_str());
        text_r(F13, colx[5] + cols[5].w - 6, y, b, st.wip_t() > 0 ? COL_WARN : COL_FAINT);
        snprintf(b, sizeof b, "%s", fmt_tons2(st.out_t() + st.chem(), "").c_str());
        text_r(F13, colx[6] + cols[6].w - 6, y, b, st.out_t() > 0 ? COL_GOOD : COL_FAINT);
        snprintf(b, sizeof b, "%s", fmt_next(st.next_tick, UI.t).c_str());
        text_r(F13, colx[7] + cols[7].w - 6, y, b, st.next_tick > UI.t ? COL_ACCENT : COL_FAINT);

        // exact throughput: tons arrived in the trailing 24 h
        double thr = 0;
        {
            Node& nd2 = TR.nodes[n];
            auto it = std::upper_bound(nd2.inflow.begin(), nd2.inflow.end(), UI.t,
                [](double tt, const InflowEvent& e) { return tt < (double)e.t; });
            for (auto j = it; j-- > nd2.inflow.begin();) {
                if (j->t <= UI.t - MS_DAY) break;
                thr += j->tons;
            }
        }
        snprintf(b, sizeof b, "%.0f", thr);
        text_r(F13, colx[8] + cols[8].w - 6, y, b, thr > 1 ? COL_TEXT : COL_FAINT);
        y += rh;
    }
    EndScissorMode();
}

// ============================================================================
//                                 top bar
// ============================================================================

static void draw_topbar() {
    Rectangle r = { 0, 0, (float)GetScreenWidth(), LY.top_bar_h };
    DrawRectangleRec(r, COL_PANEL);
    DrawLine(0, r.y + r.height - 1, r.x + r.width, r.y + r.height - 1, COL_BORDER);

    float y = 12;
    float x = 14;
    char b[96];

    // ---- title zone
    text(FB20, x, y + 1, "IMS-T8", COL_TEXT);
    x += measure(FB20, "IMS-T8").x;
    text(F13, x + 8, y + 12, "REE plant", COL_FAINT);
    x += 8 + measure(F13, "REE plant").x + 14;
    DrawLine(x, 8, x, r.height - 8, COL_BORDER);
    x += 14;

    // ---- transport zone (measured, never overlaps)
    float by = y + 1, bh = 30;
    bool hov[5];
    for (int i = 0; i < 5; i++) {
        Rectangle br = { x, by, 36, bh };
        hov[i] = CheckCollisionPointRec(UI.mouse, br);
        if (i == 2 && UI.playing) hov[i] = true;   // keep pause highlighted
        if (icon_button(br, i == 2 && UI.playing ? 3 : i, hov[i])) {
            switch (i) {
                case 0: UI.t = TR.ticks[0] + 1; UI.playing = false; break;
                case 1: UI.t = TR.ticks[std::max(0, UI.k - 1)]; UI.playing = false; break;
                case 2: UI.playing = !UI.playing; break;
                case 4: UI.t = TR.ticks[std::min((int)TR.ticks.size() - 1, UI.k + 1)]; UI.playing = false; break;
                case 5: UI.t = TR.ticks.back() - 1; UI.playing = false; break;
            }
        }
        x += 42;
    }

    // ---- speed
    if (button({ x, by, 84, bh }, SPEED_NAMES[UI.speed_idx], F16))
        UI.speed_idx = (UI.speed_idx + 1) % 5;
    x += 84 + 12;
    DrawLine(x, 8, x, r.height - 8, COL_BORDER);
    x += 14;

    // ---- clock zone
    snprintf(b, sizeof b, "%s", fmt_time((i64)UI.t).c_str());
    text(FB20, x, y, b, COL_PLAYHEAD);
    x += measure(FB20, b).x + 10;
    snprintf(b, sizeof b, "tick %d/%zu", UI.k, TR.ticks.size());
    text(F13, x, y + 12, b, COL_DIM);
    x += measure(F13, b).x + 14;
    DrawLine(x, 8, x, r.height - 8, COL_BORDER);
    x += 14;

    // ---- KPI zone (measured, flows right)
    auto kpi = [&](const char* name, double v, Color c) {
        snprintf(b, sizeof b, "%s %s", name, fmt_tons2(v, "").c_str());
        Vector2 t2 = measure(F13, b);
        float w = t2.x + 16;
        if (x + w > GetScreenWidth() - 460) return;   // leave room for right-side buttons
        DrawRectangle(x, y + 5, w, 22, Fade(c, 0.12f));
        DrawRectangle(x, y + 5, 3, 22, c);
        text(F13, x + 10, y + 9, b, c);
        x += w + 8;
    };
    for (size_t n = 0; n < TR.nodes.size(); n++) {
        uint32_t ty = TR.nodes[n].info.type;
        if (ty == T_MINER)     kpi("mined", cum_in_at(TR.nodes[n], UI.t), TR.nodes[n].color);
        if (ty == T_FLOTATION) kpi("concentrate", FR.st[n].out_t(), COL_GOOD);
        if (ty == T_TAILINGS)  kpi("tailings", FR.st[n].mass_t(), TR.nodes[n].color);
        if (ty == T_CHEM_SRC)  kpi("acid", FR.st[n].chem(), COL_ACCENT);
    }

    // ---- right-side zone
    float rx = GetScreenWidth() - 14;
    if (!LY.screenshot_mode) {
        snprintf(b, sizeof b, "%.0f fps", 1.0f / std::max(0.0001f, GetFrameTime()));
        Vector2 fb2 = measure(F13, b);
        text(F13, rx - fb2.x, y + 11, b, COL_FAINT);
        rx -= fb2.x + 12;
    }
    if (button({ rx - 34, by, 34, bh }, "?", F16)) UI.show_help = !UI.show_help;
    rx -= 34 + 8;
    if (button({ rx - 76, by, 76, bh }, "reload", F13)) UI.reload_request = 1;
    rx -= 76 + 8;
    if (button({ rx - 66, by, 66, bh }, "evt >", F13)) {
        for (auto& mk : TR.marks) if (mk.t0 > UI.t + 1) { UI.t = mk.t0 + 1; break; }
    }
    rx -= 66 + 8;
    if (button({ rx - 66, by, 66, bh }, "< evt", F13)) {
        i64 best = -1;
        for (auto& mk : TR.marks) if (mk.t0 < UI.t - 1) best = std::max(best, mk.t0);
        if (best >= 0) UI.t = best + 1;
    }
}

// ============================================================================
//                              graph view
// ============================================================================

static void draw_graph() {
    Rectangle r = LY.graph;
    DrawRectangleRec(r, COL_BG);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);

    int mine = -1, truck = -1, crusher = -1;
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        uint32_t t = TR.nodes[i].info.type;
        if (t == T_MINER) mine = (int)i;
        if (t == T_TRUCK) truck = (int)i;
        if (t == T_CRUSHER) crusher = (int)i;
    }

    // edges first (skip the mine/truck legs: the road covers them)
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        Node& nd = TR.nodes[i];
        if (nd.info.output < 0) continue;
        if ((int)i == mine || (int)i == truck) continue;
        Node& out = TR.nodes[nd.info.output];
        bool info_edge = (nd.info.type == T_CHEM_SRC);
        double recent = 0;
        {
            auto it = std::upper_bound(nd.inflow.begin(), nd.inflow.end(), UI.t,
                [](double t, const InflowEvent& e) { return t < (double)e.t; });
            for (auto j = it; j-- > nd.inflow.begin();) {
                if (j->t <= UI.t - 2 * MS_DAY) break;
                recent += j->tons;
            }
        }
        float glow = (float)clampd(recent / 200.0, 0.0, 1.0);
        Color c = Fade(info_edge ? COL_ACCENT : out.color, 0.4f + 0.5f * glow);
        draw_edge((int)i, nd.info.output, c, info_edge ? 1.5f : 2.5f + 2 * glow, info_edge);
    }

    // supply-chain region computed from the actual node rects
    {
        int reg_ids[] = { T_CHEM_DISRUPTION, T_CHEM_SRC, T_CHEM_SCRUM, TRACE_NODE_ADDITIVE_STOCK };
        float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
        int found = 0;
        for (int t : reg_ids)
            for (size_t i = 0; i < TR.nodes.size(); i++)
                if (TR.nodes[i].info.type == (uint32_t)t) {
                    Rectangle nr = node_rect((int)i);
                    minx = std::min(minx, nr.x); miny = std::min(miny, nr.y);
                    maxx = std::max(maxx, nr.x + nr.width); maxy = std::max(maxy, nr.y + nr.height);
                    found++;
                }
        if (found) {
            Rectangle reg = { minx - 18, miny - 26, maxx - minx + 36, maxy - miny + 44 };
            DrawRectangleRounded(reg, 0.05f, 6, Fade(COL_ACCENT, 0.04f));
            DrawRectangleRoundedLinesEx(reg, 0.05f, 6, 1.2f, Fade(COL_ACCENT, 0.25f));
            text_trunc(F13, reg.x + 12, reg.y + 7, "imports & supply contracts", Fade(COL_ACCENT, 0.6f), reg.width - 24);
        }
    }
    // market disruption hangs over the acid supplier
    {
        int market = -1, supplier = -1;
        for (size_t i = 0; i < TR.nodes.size(); i++) {
            if (TR.nodes[i].info.type == T_CHEM_DISRUPTION) market = (int)i;
            if (TR.nodes[i].info.type == T_CHEM_SRC) supplier = (int)i;
        }
        if (market >= 0 && supplier >= 0) {
            bool dis = FR.st[market].disrupted_until > UI.t;
            draw_edge(market, supplier, dis ? Fade(COL_BAD, 0.85f) : Fade(COL_BAD, 0.45f), 2.2f, true);
        }
    }

    if (mine >= 0 && crusher >= 0) draw_road(mine, crusher);
    if (truck >= 0) draw_truck(truck);

    for (size_t i = 0; i < TR.nodes.size(); i++) {
        if ((int)i == truck) continue;
        draw_node((int)i);
    }

    for (auto& p : PARTS) {
        Vector2 pts[4];
        edge_route(p.edge, TR.nodes[p.edge].info.output, pts);
        Vector2 pos = route_point(pts, p.u);
        DrawCircle(pos.x, pos.y, 2.6f, Fade(p.c, 0.5f + 0.5f * p.u));
    }
    EndScissorMode();
}

// ============================================================================
//                              input handling
// ============================================================================

static void handle_input(float dt) {
    if (IsKeyPressed(KEY_SPACE)) UI.playing = !UI.playing;
    if (IsKeyPressed(KEY_LEFT)) {
        UI.playing = false;
        UI.t = IsKeyDown(KEY_LEFT_SHIFT) ? std::max(0.0, UI.t - MS_DAY) : (double)TR.ticks[std::max(0, UI.k - 1)];
    }
    if (IsKeyPressed(KEY_RIGHT)) {
        UI.playing = false;
        UI.t = IsKeyDown(KEY_LEFT_SHIFT) ? UI.t + MS_DAY : (double)TR.ticks[std::min((int)TR.ticks.size() - 1, UI.k + 1)];
    }
    if (IsKeyPressed(KEY_PAGE_DOWN))
        for (auto& mk : TR.marks) if (mk.t0 > UI.t + 1) { UI.t = mk.t0 + 1; break; }
    if (IsKeyPressed(KEY_PAGE_UP)) {
        i64 best = -1;
        for (auto& mk : TR.marks) if (mk.t0 < UI.t - 1) best = std::max(best, mk.t0);
        if (best >= 0) UI.t = best + 1;
    }
    if (IsKeyPressed(KEY_LEFT_BRACKET))  UI.speed_idx = std::max(0, UI.speed_idx - 1);
    if (IsKeyPressed(KEY_RIGHT_BRACKET)) UI.speed_idx = std::min(4, UI.speed_idx + 1);
    if (IsKeyPressed(KEY_R)) UI.reload_request = 1;
    if (IsKeyPressed(KEY_H)) UI.show_help = !UI.show_help;
    if (IsKeyPressed(KEY_F11)) ToggleFullscreen();
    if (IsKeyPressed(KEY_ESCAPE)) exit(0);

    float wheel = GetMouseWheelMove();

    // ---- inspector scroll -----------------------------------------------------
    if (wheel != 0 && CheckCollisionPointRec(UI.mouse, LY.inspector) && UI.sel >= 0) {
        float visible = LY.inspector.height - 20;
        float max_scroll = std::max(0.0f, UI.insp_h - visible);
        UI.insp_scroll = clampd(UI.insp_scroll - wheel * 36, 0, max_scroll);
    }

    // ---- timeline scrub ------------------------------------------------------
    Rectangle tr = LY.timeline;
    bool over_tl = CheckCollisionPointRec(UI.mouse, tr);

    if (over_tl && wheel != 0 && !IsKeyDown(KEY_LEFT_SHIFT)) {
        double ct = tl_x_to_t(UI.mouse.x);
        double span = (UI.vw1t - UI.vw0t) / (wheel > 0 ? 1.35 : 1 / 1.35);
        span = clampd(span, MS_HOUR, TR.t_end);
        UI.vw0t = clampd(ct - (ct - UI.vw0t) / (UI.vw1t - UI.vw0t) * span, 0, TR.t_end - span);
        UI.vw1t = UI.vw0t + span;
    }
    if (over_tl && wheel != 0 && IsKeyDown(KEY_LEFT_SHIFT)) {
        double pan = (UI.vw1t - UI.vw0t) * 0.15 * (wheel > 0 ? -1 : 1);
        UI.vw0t = clampd(UI.vw0t + pan, 0, TR.t_end - (UI.vw1t - UI.vw0t));
        UI.vw1t = UI.vw0t + (UI.vw1t - UI.vw0t);
    }

    bool over_minimap = LY.tl_minimap.width > 0 && CheckCollisionPointRec(UI.mouse, LY.tl_minimap);
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && over_tl && !over_minimap) {
        UI.scrubbing = true;
        UI.playing = false;
        UI.scrub_v = 0;
    }
    if (UI.scrubbing) {
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            double nt = clampd(tl_x_to_t(UI.mouse.x), 0, TR.t_end);
            UI.scrub_v = 0.7 * UI.scrub_v + 0.3 * (nt - UI.t) / std::max(dt, 0.001f);
            UI.t = nt;
        } else {
            UI.t = clampd(UI.t + UI.scrub_v * dt, 0, TR.t_end);
            UI.scrub_v *= exp(-5.0f * dt);
            if (fabs(UI.scrub_v) < MS_MIN) { UI.scrubbing = false; UI.scrub_v = 0; }
        }
    }
    if (over_tl) {
        static double last_click = -1;
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (GetTime() - last_click < 0.35) { UI.vw0t = 0; UI.vw1t = TR.t_end; }
            last_click = GetTime();
        }
    }

    // ---- graph pan / zoom / select ---------------------------------------------
    Rectangle gr = LY.graph;
    bool over_gr = CheckCollisionPointRec(UI.mouse, gr);
    if (over_gr && wheel != 0) {
        Vector2 before = screen_to_world(UI.mouse);
        UI.zoom_t = clampd(UI.zoom_t * (wheel > 0 ? 1.15 : 1 / 1.15), 0.75, 2.6);
        UI.zoom = UI.zoom_t;
        Vector2 after = screen_to_world(UI.mouse);
        UI.cam_t.x += before.x - after.x; UI.cam_t.y += before.y - after.y;
        UI.cam = UI.cam_t;
    }

    UI.hover = -1;
    if (over_gr) {
        for (int pass = 0; pass < 2 && UI.hover < 0; pass++)
            for (size_t i = 0; i < TR.nodes.size(); i++) {
                Node& nd = TR.nodes[i];
                if (pass == 0 && nd.info.type == T_TRUCK) continue;
                if (pass == 1 && nd.info.type != T_TRUCK) continue;
                Rectangle nr = (nd.info.type == T_TRUCK)
                    ? Rectangle{ truck_pos((int)i).x - 24, truck_pos((int)i).y - 42, 48, 56 }
                    : node_rect((int)i);
                if (CheckCollisionPointRec(UI.mouse, nr)) { UI.hover = (int)i; break; }
            }
        if (UI.hover >= 0) {
            NodeState& st = FR.st[UI.hover];
            snprintf(UI.tooltip, sizeof UI.tooltip,
                     "%s\nstock %.1f t · wip %.1f t · out %.1f t\nstate %s · next event %s",
                     TR.nodes[UI.hover].info.name, st.mass_t(), st.wip_t(), st.out_t(),
                     (TR.nodes[UI.hover].info.type == T_TRUCK ? truck_name(st.truck_state) : proc_name(st.proc_state)),
                     fmt_dur(st.next_tick - (i64)UI.t).c_str());
        }
    }

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && over_gr) {
        if (UI.hover >= 0) {
            UI.sel = UI.hover;
            if (TR.nodes[UI.hover].draggable) UI.drag_node = UI.hover;
        } else {
            UI.pan_graph = true;
            UI.pan_grab = UI.mouse;
        }
    }
    if (UI.drag_node >= 0 && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        Vector2 w = screen_to_world(UI.mouse);
        TR.nodes[UI.drag_node].pos = { w.x - TR.nodes[UI.drag_node].size.x / 2, w.y - TR.nodes[UI.drag_node].size.y / 2 };
    }
    if (!IsMouseButtonDown(MOUSE_LEFT_BUTTON)) UI.drag_node = -1;

    if (UI.pan_graph) {
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            Vector2 d = Vector2Subtract(UI.mouse, UI.pan_grab);
            UI.cam_t.x -= d.x / UI.zoom; UI.cam_t.y -= d.y / UI.zoom;
            UI.cam = UI.cam_t;
            UI.pan_grab = UI.mouse;
        } else UI.pan_graph = false;
    }
}

// ============================================================================
//                                   help
// ============================================================================

static void draw_help() {
    if (!UI.show_help) return;
    Rectangle r = { GetScreenWidth() / 2.0f - 340, GetScreenHeight() / 2.0f - 250, 680, 500 };
    DrawRectangleRec(r, { 16, 20, 27, 250 });
    DrawRectangleLinesEx(r, 1, COL_ACCENT);
    float x = r.x + 20, y = r.y + 16;
    text(FB20, x, y, "Controls & legend", COL_TEXT); y += 32;
    const char* lines[] = {
        "timeline    drag = scrub (with inertia)     wheel = zoom at cursor",
        "            shift+wheel = pan    double-click = fit whole run",
        "            hover = crosshair + values tooltip",
        "graph       click node = inspect     drag node = move",
        "            drag background = pan     wheel = zoom",
        "table       click row = inspect that node",
        "",
        "space play/pause     <- -> step one tick     shift + <- -> step one day",
        "PgUp/PgDn jump to previous/next notable event",
        "[ ] playback speed   R reload trace   F11 fullscreen   Esc quit",
        "",
        "mark lanes under the timeline:",
        "  amber     mine pad empty (< 20 t)",
        "  blue      acid tank low (< 50 t)",
        "  orange    flotation starved (idle, no feed)",
        "  red       acid supply market disruption",
        "  cyan      S&OP scrum (contract revision)",
        "  green     flotation campaign start",
        "",
        "the chart shows every node's stock, each normalized to its own",
        "maximum: shapes, not absolute values. click a node to highlight it.",
    };
    for (auto& l : lines) { text(F13, x, y, l, COL_DIM); y += 17; }
}

// ============================================================================
//                                   main
// ============================================================================

static double parse_time_arg(const char* s, i64 t_end) {
    if (!strcmp(s, "end")) return (double)t_end;
    char* endp = nullptr;
    double v = strtod(s, &endp);
    if (endp && *endp == 'd') return v * MS_DAY;
    if (endp && *endp == 'h') return v * MS_HOUR;
    if (endp && *endp == 'y') return v * 365 * MS_DAY;
    return v;
}

int main(int argc, char** argv) {
    const char* path = "trace.bin.gz";
    const char* shot = nullptr;
    const char* sel_name = nullptr;
    const char* at_arg = nullptr;
    bool dump = false;
    int smoke_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--sel") && i + 1 < argc) sel_name = argv[++i];
        else if (!strcmp(argv[i], "--dump")) dump = true;
        else if (!strcmp(argv[i], "--smoke") && i + 1 < argc) smoke_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--smoke")) smoke_frames = 300;
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) at_arg = argv[++i];
        else path = argv[i];
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | (shot ? FLAG_WINDOW_HIDDEN : 0));
    InitWindow(1800, 1000, "IMS-T8 · simulation viewer");
    SetTargetFPS(60);
    load_fonts();
    LY.compute();
    LY.screenshot_mode = shot != nullptr;

    if (!load_trace(path)) { fprintf(stderr, "failed to load %s\n", path); return 1; }

    double shot_at = at_arg ? parse_time_arg(at_arg, TR.t_end) : -1;
    UI.vw0 = 0; UI.vw1 = TR.t_end;
    UI.vw0t = 0; UI.vw1t = TR.t_end;
    UI.t = shot_at >= 0 ? clampd(shot_at, 0, (double)TR.t_end) : TR.ticks[0] + 1;
    default_layout();
    build_frame(UI.t);
    UI.k = FR.k;
    UI.sel = -1;

    if (sel_name) {
        for (size_t i = 0; i < TR.nodes.size(); i++)
            if (!strcmp(TR.nodes[i].info.name, sel_name)) UI.sel = (int)i;
    } else {
        // pre-select the most information-rich node so the inspector shows data
        for (size_t i = 0; i < TR.nodes.size(); i++)
            if (TR.nodes[i].info.type == T_FLOTATION) UI.sel = (int)i;
    }

    if (dump) {
        printf("--- state at %s (tick %d) ---\n", fmt_time((i64)UI.t).c_str(), FR.k);
        for (size_t i = 0; i < TR.nodes.size(); i++) {
            NodeState& st = FR.st[i];
            printf("%-16s mass=%10.2f wip=%9.2f out=%10.2f chem=%8.2f belt=%7.2f proc=%d truck=%d next=%.2f d\n",
                   TR.nodes[i].info.name, st.mass_t(), st.wip_t(), st.out_t(), st.chem(), st.belt_t(),
                   st.proc_state, st.truck_state, st.next_tick / (double)MS_DAY);
        }
        return 0;
    }

    if (shot) {
        UI.tooltip[0] = 0;
        build_frame(UI.t);
        BeginDrawing();
        ClearBackground(COL_BG);
        LY.compute();
        draw_topbar(); draw_timeline(); draw_graph(); draw_inspector(); draw_table();
        rlDrawRenderBatchActive();
        TakeScreenshot("_viewer_shot.png");
        EndDrawing();
        {
            FILE* in = fopen("_viewer_shot.png", "rb");
            FILE* out = fopen(shot, "wb");
            if (in && out) {
                char b2[65536]; int n2;
                while ((n2 = fread(b2, 1, sizeof b2, in)) > 0) fwrite(b2, 1, n2, out);
            }
            if (in) fclose(in);
            if (out) fclose(out);
            remove("_viewer_shot.png");
        }
        printf("[viewer] screenshot -> %s\n", shot);
        return 0;
    }

    UI.playing = smoke_frames > 0;
    int frames = 0;
    while (!WindowShouldClose()) {
        if (smoke_frames && ++frames > smoke_frames) break;
        float dt = smoke_frames ? 1 / 60.0f : GetFrameTime();
        UI.mouse = GetMousePosition();
        UI.tooltip[0] = 0;

        if (UI.reload_request) {
            UI.reload_request = 0;
            size_t old_sel = UI.sel < 0 ? 0 : (size_t)UI.sel;
            if (load_trace(TR.path)) {
                UI.vw1t = TR.t_end;
                FR.k = -2; FR.valid = false;
                UI.s24_k = -3;
                build_frame(UI.t);
                UI.sel = (int)std::min(old_sel, TR.nodes.size() - 1);
                PARTS.clear();
                UI.inflow_cursor.clear();
                printf("[viewer] reloaded %s\n", TR.path);
            }
        }

        if (UI.playing && !UI.scrubbing) {
            UI.t += SPEEDS[UI.speed_idx] * dt;
            if (UI.t >= TR.t_end) { UI.t = TR.t_end; UI.playing = false; }
        }

        handle_input(dt);

        UI.vw0 = UI.vw0 + (UI.vw0t - UI.vw0) * clampd(dt * 14, 0, 1);
        UI.vw1 = UI.vw1 + (UI.vw1t - UI.vw1) * clampd(dt * 14, 0, 1);

        build_frame(UI.t);
        UI.k = FR.k;
        spawn_edge_particles();
        for (size_t i = 0; i < PARTS.size();) {
            PARTS[i].u += PARTS[i].speed * dt;
            if (PARTS[i].u >= 1) PARTS.erase(PARTS.begin() + i);
            else i++;
        }

        BeginDrawing();
        ClearBackground(COL_BG);
        LY.compute();
        draw_topbar();
        draw_timeline();
        draw_graph();
        draw_inspector();
        draw_table();
        draw_help();
        draw_tooltip();
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
