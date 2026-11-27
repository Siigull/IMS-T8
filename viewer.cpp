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
static std::string fmt_tons(double v) {
    char buf[48];
    if (v >= 100000) snprintf(buf, sizeof buf, "%.0f t", v);
    else if (v >= 100) snprintf(buf, sizeof buf, "%.1f t", v);
    else snprintf(buf, sizeof buf, "%.2f t", v);
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
            double lv = run[i].level();
            double jump = lv - prev_level[i];
            if (jump > 1.0) {
                nd.inflow.push_back({ time, (float)jump });
                nd.cum_in += jump;
                bucket_tons[i] += jump;
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

    std::sort(TR.marks.begin(), TR.marks.end(),
              [](const Mark& a, const Mark& b) { return a.t0 < b.t0; });

    snprintf(TR.path, sizeof TR.path, "%s", path);
    printf("[viewer] %s: %zu nodes, %zu ticks, %zu marks, %.1f MB raw, load %.2fs\n",
           path, TR.nodes.size(), TR.ticks.size(), TR.marks.size(),
           raw.size() / 1048576.0, GetTime() - t0);
    if (TR.truncated) printf("[viewer] warning: trace ended mid-record (sim killed while writing?)\n");
    return TR.ticks.size() > 1;
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
    NodeState s24[TRACE_MAX_NODES];
    std::vector<size_t> inflow_cursor;   // per-node event cursors (particles)
} UI;

static const double SPEEDS[] = { MS_HOUR, 6 * MS_HOUR, MS_DAY, 7 * MS_DAY, 30 * MS_DAY };
static const char* SPEED_NAMES[] = { "1 h/s", "6 h/s", "1 d/s", "7 d/s", "30 d/s" };

// ============================================================================
//                                  fonts
// ============================================================================

static Font F13, F16, F20, F28, FB16, FB20;

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
    if (!mp) { F13 = F16 = F20 = F28 = GetFontDefault(); FB16 = FB20 = GetFontDefault(); return; }
    F13 = LoadFontEx(mp, 26, nullptr, 0);  SetTextureFilter(F13.texture, TEXTURE_FILTER_BILINEAR);
    F16 = LoadFontEx(mp, 32, nullptr, 0);  SetTextureFilter(F16.texture, TEXTURE_FILTER_BILINEAR);
    F20 = LoadFontEx(mp, 40, nullptr, 0);  SetTextureFilter(F20.texture, TEXTURE_FILTER_BILINEAR);
    F28 = LoadFontEx(mp, 56, nullptr, 0);  SetTextureFilter(F28.texture, TEXTURE_FILTER_BILINEAR);
    if (bp) {
        FB16 = LoadFontEx(bp, 32, nullptr, 0); SetTextureFilter(FB16.texture, TEXTURE_FILTER_BILINEAR);
        FB20 = LoadFontEx(bp, 40, nullptr, 0); SetTextureFilter(FB20.texture, TEXTURE_FILTER_BILINEAR);
    } else FB16 = F16, FB20 = F20;
}

static void text(Font f, float x, float y, const char* s, Color c) {
    DrawTextEx(f, s, { x, y }, (float)f.baseSize, 0, c);
}
static Vector2 measure(Font f, const char* s) { return MeasureTextEx(f, s, (float)f.baseSize, 0); }

// ============================================================================
//                                widgets
// ============================================================================

static bool button(Rectangle r, const char* label, Font f) {
    bool h = CheckCollisionPointRec(UI.mouse, r);
    DrawRectangleRec(r, h ? COL_PANEL2 : COL_PANEL);
    DrawRectangleLinesEx(r, 1, h ? COL_ACCENT : COL_BORDER);
    Vector2 ts = measure(f, label);
    text(f, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2, label, h ? COL_TEXT : COL_DIM);
    return h && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

static void draw_tooltip() {
    if (!UI.tooltip[0]) return;
    const float pad = 8;
    int lines = 1;
    for (const char* c = UI.tooltip; *c; c++) if (*c == '\n') lines++;
    float w = 0, h = lines * (F13.baseSize + 3);
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
        text(F13, x + pad, yy, line, COL_TEXT); yy += F13.baseSize + 3;
    }
}

// ============================================================================
//                              screen layout
// ============================================================================

struct Layout {
    float top_bar_h = 52, timeline_h = 170, table_h = 176, inspector_w = 430;
    Rectangle timeline{}, graph{}, inspector{}, table{};
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
        n.size = { 176, 100 };
        if (n.info.type == T_CONVEYOR)  n.size = { 244, 88 };
        if (n.info.type == T_TAILINGS)  n.size = { 176, 122 };
        if (n.info.type == TRACE_NODE_ADDITIVE_STOCK) n.size = { 168, 66 };
        if      (!strcmp(name, "Mine"))           n.pos = { 30, 300 };
        else if (!strcmp(name, "Haul truck"))     n.pos = { 255, 328 }, n.size = { 120, 46 };
        else if (!strcmp(name, "Crusher"))        n.pos = { 480, 300 };
        else if (!strcmp(name, "Belt conveyor"))  n.pos = { 715, 306 };
        else if (!strcmp(name, "Flotation"))      n.pos = { 1020, 296 };
        else if (!strcmp(name, "Tailings dam"))   n.pos = { 1270, 286 };
        else if (!strcmp(name, "Chem market"))    n.pos = { 490, 40 };
        else if (!strcmp(name, "Acid supplier"))  n.pos = { 130, 110 };
        else if (!strcmp(name, "Acid S&OP"))      n.pos = { 380, 126 };
        else if (!strcmp(name, "Additive stock")) n.pos = { 800, 46 };
        else {  // unknown node: auto grid on the right
            int col = (int)i % 3, row = (int)i / 3;
            n.pos = { 1560.f + col * 210, 60.f + row * 140 };
        }
        if (n.info.type == T_TRUCK) n.draggable = false;
    }
    float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    for (auto& n : TR.nodes) {
        minx = std::min(minx, n.pos.x); miny = std::min(miny, n.pos.y);
        maxx = std::max(maxx, n.pos.x + n.size.x); maxy = std::max(maxy, n.pos.y + n.size.y);
    }
    float w = maxx - minx + 160, h = maxy - miny + 160;
    float zx = LY.graph.width / w, zy = LY.graph.height / h;
    UI.zoom_t = UI.zoom = std::min(zx, zy);
    UI.cam_t = UI.cam = { (minx + maxx) / 2, (miny + maxy) / 2 };
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

static void draw_bar(float x, float y, float w, float h, double v, double cap, Color c, const char* label) {
    DrawRectangle(x, y, w, h, COL_PANEL2);
    double u = (cap > 0 && std::isfinite(cap)) ? v / cap : (v > 0 ? 1 : 0);
    u = clampd(u, 0, 1);
    if (v > 0) DrawRectangle(x + 1, y + 1, (float)((w - 2) * u), h - 2, Fade(c, 0.85f));
    Vector2 ts = measure(F13, label);
    text(F13, x + 5, y + (h - ts.y) / 2, label, COL_TEXT);
}

static void draw_elem_bar(float x, float y, float w, float h, const float vals[6]) {
    double tot = 0; for (int e = 0; e < 6; e++) tot += vals[e];
    if (tot <= 0) { DrawRectangle(x, y, w, h, COL_PANEL2); return; }
    float cx = x;
    for (int e = 0; e < 6; e++) {
        float ww = (float)(vals[e] / tot) * w;
        if (ww > 0) DrawRectangle(cx, y, ww, h, COL_ELEMS[e]);
        cx += ww;
    }
}

// node screen rectangle
static Rectangle node_rect(int n) {
    Node& nd = TR.nodes[n];
    Vector2 s = world_to_screen(nd.pos);
    return { s.x, s.y, nd.size.x * UI.zoom, nd.size.y * UI.zoom };
}

static void draw_node(int n) {
    Node& nd = TR.nodes[n];
    NodeState& st = FR.st[n];
    Rectangle r = node_rect(n);
    float z = UI.zoom;

    DrawRectangleRounded({ r.x + 3, r.y + 4, r.width, r.height }, 0.12f, 4, Fade(BLACK, 0.35f));
    DrawRectangleRounded(r, 0.12f, 4, COL_PANEL);
    if (UI.sel == n) {
        float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 4.0f);
        DrawRectangleRoundedLinesEx(r, 0.12f, 4, 2.5f, Fade(COL_PLAYHEAD, 0.55f + 0.45f * pulse));
    } else {
        DrawRectangleRoundedLinesEx(r, 0.12f, 4, 1.5f, UI.hover == n ? COL_ACCENT : COL_BORDER);
    }

    float th = 24 * z;
    DrawRectangleRounded({ r.x, r.y, r.width, th }, 0.12f, 4, Fade(nd.color, 0.16f));
    DrawCircle(r.x + 10 * z, r.y + th / 2, 4 * z, nd.color);
    text(FB16, r.x + 20 * z, r.y + (th - FB16.baseSize) / 2, nd.info.name, COL_TEXT);

    const char* chip = nullptr; Color chip_c = COL_DIM;
    if (nd.info.type == T_CRUSHER || nd.info.type == T_FLOTATION) {
        chip = proc_name(st.proc_state);
        chip_c = st.proc_state == 2 ? COL_WARN : (st.proc_state == 1 ? COL_DIM : COL_FAINT);
    } else if (nd.info.type == T_CONVEYOR) {
        chip = conv_name(st.conv_state);
    } else if (nd.info.type == T_CHEM_DISRUPTION) {
        chip = st.disrupted_until > UI.t ? "DISRUPTED" : "calm";
        chip_c = st.disrupted_until > UI.t ? COL_BAD : COL_DIM;
    }
    if (chip) {
        Vector2 ts = measure(F13, chip);
        float cw = ts.x + 12 * z;
        DrawRectangle(r.x + r.width - cw - 6, r.y + 4, cw, th - 8, Fade(chip_c, 0.18f));
        text(F13, r.x + r.width - cw, r.y + (th - F13.baseSize) / 2, chip, chip_c);
    }

    float y = r.y + th + 6 * z;
    char b[80];

    if (nd.info.type == T_CONVEYOR) {
        int cells = nd.info.belt_cells > 0 ? nd.info.belt_cells : 6;
        float cw = (r.width - 16 * z) / cells;
        float chh = 24 * z;
        for (int c = 0; c < cells; c++) {
            float cx = r.x + 8 * z + c * cw;
            double tons = 0; for (int e = 0; e < 6; e++) tons += st.belt(c, e);
            DrawRectangle(cx + 1, y, cw - 2, chh, COL_PANEL2);
            double u = clampd(tons / (double)nd.info.mass_capacity, 0, 1);
            if (tons > 0.01) DrawRectangle(cx + 2, y + 1, (cw - 4) * (float)u, chh - 2, Fade(nd.color, 0.8f));
            int head = ((st.belt_head % cells) + cells) % cells;
            if (c == head) DrawRectangleLinesEx({ cx + 1, y - 2, cw - 2, chh + 4 }, 1.5f, COL_PLAYHEAD);
            if (cw > 40) {
                snprintf(b, sizeof b, "%.0f", tons);
                Vector2 ts = measure(F13, b);
                text(F13, cx + (cw - ts.x) / 2, y + chh + 2, b, COL_DIM);
            }
        }
        y += chh + 17 * z;
        snprintf(b, sizeof b, "belt %.0f t", st.belt_t());
        text(F13, r.x + 8 * z, y, b, COL_DIM);
    } else if (nd.info.type == T_TAILINGS) {
        float fh = r.height - th - 12 * z;
        DrawRectangle(r.x + 8 * z, r.y + th + 6 * z, r.width - 16 * z, fh, COL_PANEL2);
        double u = clampd(st.mass_t() / 600000.0, 0, 1);
        if (u > 0.01) DrawRectangle(r.x + 9 * z, r.y + th + 6 * z + fh * (1 - (float)u),
                                    r.width - 18 * z, fh * (float)u, Fade(nd.color, 0.45f));
        snprintf(b, sizeof b, "%.0f t", st.mass_t());
        Vector2 ts = measure(FB20, b);
        text(FB20, r.x + (r.width - ts.x) / 2, r.y + th + 6 * z + fh / 2 - 10, b, COL_TEXT);
    } else if (nd.info.type == T_CHEM_SRC) {
        float fh = r.height - th - 12 * z;
        float tw = 36 * z;
        DrawRectangle(r.x + r.width - tw - 10 * z, r.y + th + 6 * z, tw, fh, COL_PANEL2);
        double u = clampd(st.chem() / (double)nd.info.mass_capacity, 0, 1);
        if (u > 0.005) DrawRectangle(r.x + r.width - tw - 9 * z, r.y + th + 6 * z + fh * (1 - (float)u),
                                     tw - 2, fh * (float)u, Fade(nd.color, 0.75f));
        float ty = r.y + th + 6 * z;
        snprintf(b, sizeof b, "tank %.0f t", st.chem());  text(F16, r.x + 10 * z, ty, b, COL_TEXT); ty += 21 * z;
        snprintf(b, sizeof b, "burned %.0f t", st.chem_burned); text(F13, r.x + 10 * z, ty, b, COL_DIM); ty += 18 * z;
        snprintf(b, sizeof b, "contract %d cars", st.chem_cars); text(F13, r.x + 10 * z, ty, b, COL_DIM); ty += 18 * z;
        snprintf(b, sizeof b, "next %s", fmt_dur(st.next_tick - (i64)UI.t).c_str());
        text(F13, r.x + 10 * z, ty, b, st.disrupted_until > UI.t ? COL_BAD : COL_FAINT);
    } else if (nd.info.type == TRACE_NODE_ADDITIVE_STOCK) {
        snprintf(b, sizeof b, "%.1f t Dy oxide", st.mass_t());
        text(F16, r.x + 10 * z, y, b, COL_TEXT);
        draw_elem_bar(r.x + 10 * z, y + 24 * z, r.width - 20 * z, 7 * z, st.mass);
    } else {
        float bh = 14 * z, bw = r.width - 16 * z;
        snprintf(b, sizeof b, "stock %s", fmt_tons(st.mass_t()).c_str());
        draw_bar(r.x + 8 * z, y, bw, bh, st.mass_t(), nd.info.mass_capacity, nd.color, b); y += bh + 4 * z;
        if (st.wip_t() > 0.005 || nd.info.type == T_CRUSHER || nd.info.type == T_FLOTATION) {
            snprintf(b, sizeof b, "wip %s", fmt_tons(st.wip_t()).c_str());
            draw_bar(r.x + 8 * z, y, bw, bh, st.wip_t(), 150, COL_WARN, b); y += bh + 4 * z;
        }
        if (st.out_t() > 0.005) {
            snprintf(b, sizeof b, "out %s", fmt_tons(st.out_t()).c_str());
            draw_bar(r.x + 8 * z, y, bw, bh, st.out_t(), HUGE_VAL, COL_GOOD, b); y += bh + 4 * z;
        }
        if (st.mass_t() > 0.01) draw_elem_bar(r.x + 8 * z, y, bw, 7 * z, st.mass);
        y += 12 * z;
        double tot = st.mass_t(), reo = 0;
        for (int e = 0; e < 5; e++) reo += st.m(e);
        snprintf(b, sizeof b, "grade %.1f%%   next in %s", tot > 1 ? 100 * reo / tot : 0,
                 fmt_dur(st.next_tick - (i64)UI.t).c_str());
        text(F13, r.x + 8 * z, y, b, COL_FAINT);
    }

    // subtle ring when the node ticked at the current tick
    if (((TR.ticked[UI.k] >> n) & 1))
        DrawRectangleRoundedLinesEx({ r.x - 3, r.y - 3, r.width + 6, r.height + 6 }, 0.12f, 4, 1.5f, Fade(COL_TEXT, 0.22f));
}

// 3-segment orthogonal route from src right edge to dst left edge
static void edge_route(int from, int to, Vector2 pts[4]) {
    Rectangle a = node_rect(from), b = node_rect(to);
    Vector2 p0 = { a.x + a.width, a.y + a.height * 0.35f };
    Vector2 p3 = { b.x, b.y + b.height * 0.35f };
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
            return { (float)lerpd(pts[i].x, pts[i + 1].x, (double)uu),
                      (float)lerpd(pts[i].y, pts[i + 1].y, (double)uu) };
        }
        d -= lens[i];
    }
    return pts[3];
}

static void draw_edge(int from, int to, Color c, float thick, bool dashed) {
    Vector2 pts[4];
    edge_route(from, to, pts);
    if (dashed) {
        for (int s = 0; s < 3; s++) {
            float len = Vector2Distance(pts[s], pts[s + 1]);
            if (len < 1) continue;
            Vector2 d = { (pts[s + 1].x - pts[s].x) / len, (pts[s + 1].y - pts[s].y) / len };
            for (float u = 0; u < len; u += 22)
                DrawLineEx({ pts[s].x + d.x * u, pts[s].y + d.y * u },
                           { pts[s].x + d.x * std::min(u + 12, len), pts[s].y + d.y * std::min(u + 12, len) },
                           thick, c);
        }
    } else {
        DrawLineEx(pts[0], pts[1], thick, c);
        DrawLineEx(pts[1], pts[2], thick, c);
        DrawLineEx(pts[2], pts[3], thick, c);
    }
    // arrowhead
    Vector2 dir = Vector2Normalize(Vector2Subtract(pts[3], pts[2]));
    Vector2 perp = { -dir.y, dir.x };
    float as = 6 + thick * 2;
    DrawTriangle(pts[3],
                 { pts[3].x - dir.x * as - perp.x * as * 0.55f, pts[3].y - dir.y * as - perp.y * as * 0.55f },
                 { pts[3].x - dir.x * as + perp.x * as * 0.55f, pts[3].y - dir.y * as + perp.y * as * 0.55f }, c);
}

// the haul truck: a moving glyph on the mine → crusher road
static Vector2 truck_pos(int truck) {
    Node& nd = TR.nodes[truck];
    NodeState& st = FR.st[truck];
    // last truck-state change before now
    i64 t_change = TR.ticks[0];
    const StateChange* best = nullptr;
    for (auto& sc : nd.state_changes)
        if (sc.field == 1 && sc.t <= UI.t) { best = &sc; }
    if (best) t_change = best->t;
    double u = clampd((UI.t - t_change) / (double)MS_HOUR, 0, 1);
    double prog;
    if (st.truck_state == 2) prog = u;            // loaded, mine → plant
    else if (st.truck_state == 3) prog = 1 - u;   // empty, plant → mine
    else if (st.truck_state == 1) prog = 1.0;     // dumping at plant
    else prog = 0.0;                              // loading at mine
    // road from mine to crusher, offset below the boxes
    int mine = -1, crusher = -1;
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        if (TR.nodes[i].info.type == T_MINER) mine = (int)i;
        if (TR.nodes[i].info.type == T_CRUSHER) crusher = (int)i;
    }
    if (mine < 0 || crusher < 0) return world_to_screen(nd.pos);
    Rectangle a = node_rect(mine), b2 = node_rect(crusher);
    float x0 = a.x + a.width * 0.55f, x1 = b2.x + b2.width * 0.3f;
    float y = a.y + a.height + 34 * UI.zoom;
    return { (float)lerpd((double)x0, (double)x1, prog), y };
}

static void draw_truck(int truck) {
    Node& nd = TR.nodes[truck];
    NodeState& st = FR.st[truck];
    Vector2 tp = truck_pos(truck);
    float s = clampd(UI.zoom, 0.6f, 1.4f);

    // road
    int mine = -1, crusher = -1;
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        if (TR.nodes[i].info.type == T_MINER) mine = (int)i;
        if (TR.nodes[i].info.type == T_CRUSHER) crusher = (int)i;
    }
    if (mine >= 0 && crusher >= 0) {
        Rectangle a = node_rect(mine), b2 = node_rect(crusher);
        float x0 = a.x + a.width * 0.55f, x1 = b2.x + b2.width * 0.3f;
        float y = a.y + a.height + 34 * UI.zoom;
        DrawLineEx({ x0, y + 8 * s }, { x1, y + 8 * s }, 3, Fade(COL_BORDER, 0.9f));
        for (float x = x0; x < x1; x += 26 * s)
            DrawLineEx({ x, y + 8 * s - 4 }, { x + 12 * s, y + 8 * s - 4 }, 1.5f, Fade(COL_FAINT, 0.5f));
    }

    // body
    Rectangle body = { tp.x - 18 * s, tp.y - 12 * s, 36 * s, 20 * s };
    bool sel = UI.sel == truck, hov = UI.hover == truck;
    DrawRectangleRec(body, COL_PANEL);
    DrawRectangleLinesEx(body, 1.5f, sel ? COL_PLAYHEAD : (hov ? COL_ACCENT : nd.color));
    // bed fill = load
    double load = clampd(st.mass_t() / 100.0, 0, 1);
    if (load > 0.01) DrawRectangle(body.x + 2, body.y + 2 + (body.height - 4) * (1 - (float)load),
                                  body.width - 4, (body.height - 4) * (float)load, Fade(nd.color, 0.7f));
    // cab
    DrawRectangle(tp.x + 16 * s, tp.y - 6 * s, 10 * s, 12 * s, Fade(nd.color, 0.9f));
    // wheels
    DrawCircle(tp.x - 10 * s, tp.y + 9 * s, 3.5f * s, COL_FAINT);
    DrawCircle(tp.x + 10 * s, tp.y + 9 * s, 3.5f * s, COL_FAINT);

    // status bubble
    char b[64];
    snprintf(b, sizeof b, "%s  %.0f t", truck_name(st.truck_state), st.mass_t());
    Vector2 ts = measure(F13, b);
    float bx = tp.x - (ts.x + 12) / 2, by = tp.y - 34 * s;
    DrawRectangle(bx, by, ts.x + 12, 17, { 20, 26, 34, 230 });
    DrawRectangleLinesEx({ bx, by, ts.x + 12, 17 }, 1, sel ? COL_PLAYHEAD : COL_BORDER);
    text(F13, bx + 6, by, b, COL_TEXT);
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
    COL_WARN, COL_ACCENT, { 251, 146, 60, 255 }, COL_BAD, { 34, 211, 238, 255 }, COL_GOOD
};

static void draw_timeline() {
    Rectangle r = LY.timeline;
    DrawRectangleRec(r, COL_PANEL);
    DrawRectangleLinesEx(r, 1, COL_BORDER);

    float chart_h = r.height - 86;
    float chart_y = r.y + 8;
    i64 t_end = TR.t_end;

    // ---- grid + time labels ------------------------------------------------
    double span = UI.vw1 - UI.vw0;
    double steps[] = { MS_HOUR, 6 * MS_HOUR, MS_DAY, 7 * MS_DAY, 30 * MS_DAY, 90 * MS_DAY, 365 * MS_DAY };
    double step = steps[6];
    for (double s : steps) if (s / span * LY.timeline.width > 90) { step = s; break; }
    if (365.0 * MS_DAY / span * LY.timeline.width < 60) step = 2 * 365 * MS_DAY;
    for (double t = ceil(UI.vw0 / step) * step; t < UI.vw1; t += step) {
        float x = tl_t_to_x(t);
        DrawLine(x, chart_y, x, chart_y + chart_h, Fade(COL_BORDER, 0.55f));
        char b[32];
        double days = t / MS_DAY;
        if (step >= 90 * MS_DAY) snprintf(b, sizeof b, "Y%d D%03d", (int)(days / 365) + 1, (int)days % 365 + 1);
        else snprintf(b, sizeof b, "D%03d", (int)days);
        text(F13, x + 3, chart_y + chart_h - 13, b, COL_FAINT);
    }

    // ---- node stock envelopes, each normalized to its own max ----------------
    for (size_t n = 0; n < TR.nodes.size(); n++) {
        Node& nd = TR.nodes[n];
        double nmax = 0;
        for (int b = 0; b < NB; b++) nmax = std::max(nmax, (double)nd.series[b].mass.mx);
        if (nmax < 1) continue;
        bool strong = (int)n == UI.sel;
        float y0 = chart_y + 4, h = chart_h - 24;
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
            float yn = y0 + h * (1 - (float)(mn / nmax) * 0.92f);
            float yx = y0 + h * (1 - (float)(mx / nmax) * 0.92f);
            DrawRectangle((float)px, yn, 1, std::max(1.0f, yx - yn + 1), Fade(nd.color, strong ? 0.9f : 0.38f));
        }
    }

    // ---- mark lanes ----------------------------------------------------------
    float lane_y = chart_y + chart_h + 6;
    float lane_h = 4, lane_gap = 3;
    for (auto& mk : TR.marks) {
        float x = tl_t_to_x((double)mk.t0);
        float x2 = tl_t_to_x((double)mk.t1);
        float y = lane_y + mk.kind * (lane_h + lane_gap);
        if (mk.t0 == mk.t1) DrawRectangle(x - 0.5f, y, 1.5f, lane_h, MARK_COL[mk.kind]);
        else DrawRectangle(x, y - (mk.kind == N_DISRUPTED ? 1 : 0),
                           std::max(1.5f, x2 - x), lane_h + (mk.kind == N_DISRUPTED ? 2 : 0),
                           Fade(MARK_COL[mk.kind], mk.kind == N_DISRUPTED ? 0.85f : 0.55f));
    }

    // ---- hover crosshair + tooltip -------------------------------------------
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

    // ---- minimap when zoomed --------------------------------------------------
    float mm_y = lane_y + N_KINDS * (lane_h + lane_gap) + 4;
    float mm_h = 9;
    if (UI.vw1 - UI.vw0 < t_end * 0.995) {
        DrawRectangle(r.x, mm_y, r.width, mm_h, COL_PANEL2);
        DrawRectangleLinesEx({ r.x, mm_y, r.width, mm_h }, 1, COL_BORDER);
        float wx0 = r.x + (float)(UI.vw0 / t_end) * r.width;
        float wx1 = r.x + (float)(UI.vw1 / t_end) * r.width;
        DrawRectangle(wx0, mm_y + 1, std::max(3.0f, wx1 - wx0), mm_h - 2, Fade(COL_ACCENT, 0.45f));
        static bool mm_drag = false;
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(UI.mouse, { r.x, mm_y, r.width, mm_h }))
            mm_drag = true;
        if (!IsMouseButtonDown(MOUSE_LEFT_BUTTON)) mm_drag = false;
        if (mm_drag) {
            double sp = UI.vw1t - UI.vw0t;
            double c = (UI.mouse.x - r.x) / r.width * t_end;
            UI.vw0t = clampd(c - sp / 2, 0, t_end - sp);
            UI.vw1t = UI.vw0t + sp;
        }
    } else {
        text(F13, r.x + 8, mm_y, "whole run — drag to scrub, wheel to zoom, shift+drag to pan", COL_FAINT);
    }

    // ---- playhead ---------------------------------------------------------------
    float px = tl_t_to_x(UI.t);
    DrawRectangle(px - 1, chart_y, 2, chart_h + 6 + N_KINDS * (lane_h + lane_gap), Fade(COL_PLAYHEAD, 0.95f));
    DrawTriangle({ px - 6, r.y + 1 }, { px + 6, r.y + 1 }, { px, r.y + 10 }, COL_PLAYHEAD);
    DrawTriangle({ px - 5, r.y + r.height - 1 }, { px, r.y + r.height - 9 }, { px + 5, r.y + r.height - 1 }, COL_PLAYHEAD);
}

// ============================================================================
//                                inspector
// ============================================================================

static void draw_series_chart(Rectangle r, int n, double t0, double t1) {
    DrawRectangleRec(r, COL_PANEL2);
    Node& nd = TR.nodes[n];
    struct SRef { Series NodeSeries::* s; Color c; };
    SRef refs[4] = {
        { &NodeSeries::mass, nd.color },
        { &NodeSeries::wip, COL_WARN },
        { &NodeSeries::out, COL_GOOD },
        { &NodeSeries::chem, COL_ACCENT },
    };
    double maxs[4] = {};
    for (int ri = 0; ri < 4; ri++)
        for (int b = 0; b < NB; b++)
            maxs[ri] = std::max(maxs[ri], (double)(nd.series[b].*refs[ri].s).mx);
    for (int ri = 0; ri < 4; ri++) {
        if (maxs[ri] < 0.01) continue;
        float px_prev = -1, y_prev = -1;
        for (int px = 0; px <= (int)r.width; px++) {
            double t = t0 + (double)px / r.width * (t1 - t0);
            int b = (int)clampd(t / TR.t_end * NB, 0, NB - 1);
            float y = r.y + r.height - 3 - (float)(((nd.series[b].*refs[ri].s).last / maxs[ri]) * (r.height - 8));
            if (px_prev >= 0) DrawLineEx({ px_prev, y_prev }, { r.x + px, y }, 1.5f, refs[ri].c);
            px_prev = r.x + px; y_prev = y;
        }
        // max label
        char b[32]; snprintf(b, sizeof b, "%.0f", maxs[ri]);
        text(F13, r.x + 4, r.y + 2 + ri * 12, b, Fade(refs[ri].c, 0.9f));
    }
    float px = r.x + (float)((UI.t - t0) / (t1 - t0)) * r.width;
    if (px >= r.x && px <= r.x + r.width) DrawLine(px, r.y, px, r.y + r.height, Fade(COL_PLAYHEAD, 0.95f));
    DrawRectangleLinesEx(r, 1, COL_BORDER);
}

static void draw_state_strip(Rectangle r, int n) {
    DrawRectangleRec(r, COL_PANEL2);
    Node& nd = TR.nodes[n];
    bool has[3] = {};
    for (auto& sc : nd.state_changes) has[sc.field] = true;
    int rows = has[0] + has[1] + has[2];
    if (rows == 0) { text(F13, r.x + 6, r.y + 4, "no state changes", COL_FAINT); return; }

    uint8_t cur[3] = { FR.st[n].proc_state, FR.st[n].truck_state, FR.st[n].conv_state };
    int f_drawn = -1;
    for (int f = 0; f < 3; f++) {
        if (!has[f]) continue;
        f_drawn++;
        float y = r.y + 2 + f_drawn * (r.height / rows);
        float h = r.height / rows - 4;
        i64 t_prev = TR.ticks[0];
        uint8_t v_prev = 0;
        Color c;
        for (auto& sc : nd.state_changes) {
            if (sc.field != f) continue;
            float x0 = r.x + (float)((double)t_prev / TR.t_end) * r.width;
            float x1 = r.x + (float)((double)sc.t / TR.t_end) * r.width;
            c = v_prev == 2 ? COL_WARN : (v_prev == 1 ? COL_DIM : COL_PANEL2);
            if (f == 1) c = v_prev >= 2 ? COL_ACCENT : (v_prev == 1 ? COL_GOOD : COL_DIM);
            DrawRectangle(x0, y, std::max(0.5f, x1 - x0), h, Fade(c, 0.8f));
            t_prev = sc.t; v_prev = sc.value;
        }
        c = v_prev == 2 ? COL_WARN : (v_prev == 1 ? COL_DIM : COL_PANEL2);
        if (f == 1) c = v_prev >= 2 ? COL_ACCENT : (v_prev == 1 ? COL_GOOD : COL_DIM);
        float x0 = r.x + (float)((double)t_prev / TR.t_end) * r.width;
        DrawRectangle(x0, y, r.x + r.width - x0, h, Fade(c, 0.8f));
        const char* lbl = f == 0 ? proc_name(cur[0]) : (f == 1 ? truck_name(cur[1]) : conv_name(cur[2]));
        text(F13, r.x + 4, y + h / 2 - 6, lbl, COL_TEXT);
    }
    float px = r.x + (float)((double)UI.t / TR.t_end) * r.width;
    DrawLine(px, r.y, px, r.y + r.height, Fade(COL_PLAYHEAD, 0.95f));
}

static void draw_inspector() {
    Rectangle r = LY.inspector;
    DrawRectangleRec(r, COL_PANEL);
    DrawRectangleLinesEx(r, 1, COL_BORDER);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);

    if (UI.sel < 0) {
        float x = r.x + 16, y = r.y + 14;
        text(FB20, x, y, "Inspector", COL_TEXT); y += 30;
        text(F16, x, y, "click a node in the graph, or a table row", COL_DIM); y += 40;

        text(FB16, x, y, "System inventory (elements in stocks)", COL_TEXT); y += 22;
        double elem[6] = {};
        for (size_t n = 0; n < TR.nodes.size(); n++)
            for (int e = 0; e < 6; e++)
                elem[e] += FR.st[n].mass[e] + FR.st[n].mass[6 + e] + FR.st[n].mass[12 + e];
        double tot = 0; for (double e : elem) tot += e;
        float cx = x;
        for (int e = 0; e < 6; e++) {
            float w = tot > 0 ? (float)(elem[e] / tot) * (r.width - 32) : 0;
            DrawRectangle(cx, y, w, 16, COL_ELEMS[e]); cx += w;
        }
        y += 24;
        char b[96];
        for (int e = 0; e < 6; e++) {
            snprintf(b, sizeof b, "%-5s %12.0f t   %5.1f%%", ELEM_NAMES[e], elem[e], tot > 0 ? 100 * elem[e] / tot : 0);
            text(F13, x, y, b, e < 5 ? COL_ELEMS[e] : COL_DIM); y += 17;
        }
        y += 16;
        text(FB16, x, y, "Controls", COL_TEXT); y += 22;
        const char* lines[] = {
            "space play/pause    ←/→ step tick    shift+←/→ day",
            "PgUp/PgDn  jump to prev/next notable event",
            "[ ] speed   R reload trace   F11 fullscreen   H help",
            "timeline: drag = scrub (inertia), wheel = zoom,",
            "          shift+drag = pan, double-click = fit",
            "graph: click node = select, drag node = move,",
            "       drag bg = pan, wheel = zoom",
        };
        for (auto& l : lines) { text(F13, x, y, l, COL_DIM); y += 17; }
        return;
    }

    Node& nd = TR.nodes[UI.sel];
    NodeState& st = FR.st[UI.sel];
    float x = r.x + 14, y = r.y + 12;
    char b[160];

    DrawCircle(x + 7, y + 11, 6, nd.color);
    text(FB20, x + 20, y, nd.info.name, COL_TEXT); y += 30;
    snprintf(b, sizeof b, "in: %s    out: %s",
             nd.info.input >= 0 ? TR.nodes[nd.info.input].info.name : "-",
             nd.info.output >= 0 ? TR.nodes[nd.info.output].info.name : "-");
    text(F13, x, y, b, COL_FAINT); y += 22;

    auto val_line = [&](const char* name, double v, Color c) {
        text(F13, x, y, name, COL_DIM);
        snprintf(b, sizeof b, "%.1f", v);
        Vector2 ts = measure(F13, b);
        text(F13, r.x + r.width - 16 - ts.x, y, b, c);
        y += 17;
    };
    val_line("stock (t)", st.mass_t(), COL_TEXT);
    val_line("wip (t)", st.wip_t(), COL_TEXT);
    val_line("product (t)", st.out_t(), COL_TEXT);
    if (nd.info.belt_cells > 0) val_line("belt (t)", st.belt_t(), COL_TEXT);
    if (nd.info.type == T_CHEM_SRC) val_line("acid tank (t)", st.chem(), COL_ACCENT);
    val_line("total (t)", st.level(), COL_TEXT);
    val_line("stock REO (t)", [&]{ double s=0; for(int e=0;e<5;e++) s+=st.m(e); return s; }(), COL_TEXT);
    y += 4;

    snprintf(b, sizeof b, "proc %s · truck %s · conv %s", proc_name(st.proc_state), truck_name(st.truck_state), conv_name(st.conv_state));
    text(F13, x, y, b, COL_DIM); y += 17;
    snprintf(b, sizeof b, "next event in %s", fmt_dur(st.next_tick - (i64)UI.t).c_str());
    text(F13, x, y, b, COL_ACCENT); y += 24;

    if (st.mass_t() > 0.01) {
        text(FB16, x, y, "stock element split", COL_TEXT); y += 18;
        draw_elem_bar(x, y, r.width - 28, 10, st.mass); y += 16;
        for (int e = 0; e < 6; e++) {
            snprintf(b, sizeof b, "%-5s %10.1f t", ELEM_NAMES[e], st.m(e));
            text(F13, x, y, b, e < 5 ? COL_ELEMS[e] : COL_DIM); y += 15;
        }
        y += 6;
    }

    text(FB16, x, y, "history (whole run)", COL_TEXT); y += 18;
    draw_series_chart({ x, y, r.width - 28, 96 }, UI.sel, 0, TR.t_end); y += 104;

    double zspan = std::max(30.0 * MS_DAY, UI.vw1 - UI.vw0);
    text(FB16, x, y, "zoomed around playhead", COL_TEXT); y += 18;
    draw_series_chart({ x, y, r.width - 28, 96 }, UI.sel, UI.t - zspan / 2, UI.t + zspan / 2); y += 104;

    text(FB16, x, y, "state over time", COL_TEXT); y += 18;
    draw_state_strip({ x, y, r.width - 28, 40 }, UI.sel); y += 50;

    if (button({ x, y, 132, 26 }, "< prev change", F13)) {
        i64 best = -1;
        for (auto& sc : nd.state_changes) if (sc.t < UI.t - 1) best = std::max(best, sc.t);
        if (best >= 0) UI.t = best + 1;
    }
    if (button({ x + 142, y, 132, 26 }, "next change >", F13)) {
        for (auto& sc : nd.state_changes) if (sc.t > UI.t + 1) { UI.t = sc.t + 1; break; }
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

    float cols[9] = { 14, 190, 86, 92, 74, 84, 96, 108, 92 };
    const char* heads[9] = { "", "node", "state", "stock t", "grade %", "wip t", "product t", "next event", "d 24h t" };
    float y = r.y + 8, x = r.x + 14;
    for (int i = 0; i < 9; i++) { text(F13, x + cols[i], y, heads[i], COL_FAINT); x += cols[i]; }
    y += 19;
    float rh = 17.5f;

    int rows = std::min((int)((r.height - 42) / rh), (int)TR.nodes.size());
    int k24 = tick_at(std::max(0.0, UI.t - MS_DAY));
    if (k24 != UI.s24_k) {
        for (size_t n = 0; n < TR.nodes.size(); n++) node_state_at((int)n, k24, UI.s24[n]);
        UI.s24_k = k24;
    }

    for (int n = 0; n < rows; n++) {
        Rectangle row_r = { r.x, y - 1, r.width, rh };
        bool hover = CheckCollisionPointRec(UI.mouse, row_r);
        if (n == UI.sel) DrawRectangleRec(row_r, Fade(COL_ACCENT, 0.12f));
        else if (hover) DrawRectangleRec(row_r, Fade(COL_TEXT, 0.05f));
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && hover) UI.sel = n;

        NodeState& st = FR.st[n];
        Node& nd = TR.nodes[n];
        float x2 = r.x + 14;
        int ci = 0;
        DrawRectangle(x2 + cols[ci], y + 4, 9, 9, nd.color); ci++;
        text(F13, x2 + cols[ci], y, nd.info.name, COL_TEXT); ci++;
        const char* sname = proc_name(st.proc_state);
        if (nd.info.type == T_TRUCK) sname = truck_name(st.truck_state);
        else if (nd.info.type == T_CONVEYOR) sname = conv_name(st.conv_state);
        else if (nd.info.type == T_CHEM_DISRUPTION) sname = st.disrupted_until > UI.t ? "DISRUPTED" : "calm";
        else if (nd.info.type == T_CHEM_SRC) sname = "supplying";
        else if (nd.info.type == T_CHEM_SCRUM) sname = "S&OP";
        else if (nd.info.type == T_TAILINGS || nd.info.type == TRACE_NODE_ADDITIVE_STOCK) sname = "store";
        else if (nd.info.type == T_MINER) sname = "mining";
        text(F13, x2 + cols[ci], y, sname, COL_DIM); ci++;

        char b[64];
        snprintf(b, sizeof b, "%.0f", st.mass_t() + st.belt_t());
        text(F13, x2 + cols[ci], y, b, COL_TEXT); ci++;
        double tot = st.mass_t() + st.belt_t(), reo = 0;
        for (int e = 0; e < 5; e++) reo += st.m(e);
        snprintf(b, sizeof b, "%.1f", tot > 1 ? 100 * reo / tot : 0.0);
        text(F13, x2 + cols[ci], y, b, COL_DIM); ci++;
        snprintf(b, sizeof b, "%.0f", st.wip_t());
        text(F13, x2 + cols[ci], y, b, st.wip_t() > 0 ? COL_WARN : COL_FAINT); ci++;
        snprintf(b, sizeof b, "%.0f", st.out_t() + st.chem());
        text(F13, x2 + cols[ci], y, b, st.out_t() > 0 ? COL_GOOD : COL_FAINT); ci++;
        snprintf(b, sizeof b, "%s", fmt_dur(st.next_tick - (i64)UI.t).c_str());
        text(F13, x2 + cols[ci], y, b, st.next_tick > UI.t ? COL_ACCENT : COL_FAINT); ci++;

        double d24 = st.level() - UI.s24[n].level();
        snprintf(b, sizeof b, "%+.0f", d24);
        text(F13, x2 + cols[ci], y, b, d24 > 0.5 ? COL_GOOD : (d24 < -0.5 ? COL_BAD : COL_FAINT)); ci++;
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

    float x = 12, y = 11;
    text(FB20, x, y - 3, "IMS-T8 · REE plant", COL_TEXT);
    x += 225;

    auto ib = [&](float bx, const char* lbl) { return button({ bx, y + 1, 36, 30 }, lbl, F16); };
    if (ib(x, "|<")) { UI.t = TR.ticks[0] + 1; UI.playing = false; }
    x += 41;
    if (ib(x, "<"))  { UI.t = TR.ticks[std::max(0, UI.k - 1)] + 1; }
    x += 41;
    if (ib(x, UI.playing ? "||" : ">")) UI.playing = !UI.playing;
    x += 41;
    if (ib(x, ">|")) { UI.t = TR.ticks.back() - 1; UI.playing = false; }
    x += 46;

    if (button({ x, y + 1, 88, 30 }, SPEED_NAMES[UI.speed_idx], F16))
        UI.speed_idx = (UI.speed_idx + 1) % 5;
    x += 96;

    char b[96];
    snprintf(b, sizeof b, "%s", fmt_time((i64)UI.t).c_str());
    text(FB20, x, y, b, COL_PLAYHEAD);
    x += measure(FB20, b).x + 16;
    snprintf(b, sizeof b, "tick %d/%zu", UI.k, TR.ticks.size());
    text(F13, x, y + 9, b, COL_DIM);
    x += 118;

    auto kpi = [&](const char* name, double v, Color c) {
        snprintf(b, sizeof b, "%s %.0f t", name, v);
        Vector2 t2 = measure(F13, b);
        DrawRectangle(x, y + 5, t2.x + 14, 22, Fade(c, 0.13f));
        text(F13, x + 7, y + 9, b, c);
        x += t2.x + 20;
    };
    for (size_t n = 0; n < TR.nodes.size(); n++) {
        uint32_t ty = TR.nodes[n].info.type;
        if (ty == T_MINER)     kpi("mined", TR.nodes[n].cum_in, TR.nodes[n].color);
        if (ty == T_FLOTATION) kpi("concentrate", FR.st[n].out_t(), COL_GOOD);
        if (ty == T_TAILINGS)  kpi("tailings", FR.st[n].mass_t(), TR.nodes[n].color);
        if (ty == T_CHEM_SRC)  kpi("acid", FR.st[n].chem(), COL_ACCENT);
    }

    // right side buttons
    float rx = GetScreenWidth() - 12;
    snprintf(b, sizeof b, "%.0f fps", 1.0f / std::max(0.0001f, GetFrameTime()));
    Vector2 fb = measure(F13, b);
    text(F13, rx - fb.x, y + 9, b, COL_FAINT);
    rx -= fb.x + 14;
    if (button({ rx - 34, y + 1, 34, 30 }, "?", F16)) UI.show_help = !UI.show_help;
    rx -= 40;
    if (button({ rx - 78, y + 1, 78, 30 }, "reload", F13)) UI.reload_request = 1;
    rx -= 84;
    if (button({ rx - 72, y + 1, 72, 30 }, "evt >", F13)) {
        for (auto& mk : TR.marks) if (mk.t0 > UI.t + 1) { UI.t = mk.t0 + 1; break; }
    }
    rx -= 76;
    if (button({ rx - 72, y + 1, 72, 30 }, "< evt", F13)) {
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

    // grid dots
    float gs = 56 * UI.zoom;
    if (gs > 18)
        for (float gx = fmodf(-UI.cam.x * UI.zoom, gs); gx < r.width; gx += gs)
            for (float gy = fmodf(-UI.cam.y * UI.zoom, gs); gy < r.height; gy += gs)
                DrawPixel((int)(r.x + gx), (int)(r.y + gy), Fade(COL_BORDER, 0.6f));

    int mine = -1, truck = -1;
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        uint32_t t = TR.nodes[i].info.type;
        if (t == T_MINER) mine = (int)i;
        if (t == T_TRUCK) truck = (int)i;
    }

    // edges (skip mine→truck / truck→crusher: drawn as the road)
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        Node& nd = TR.nodes[i];
        if (nd.info.output < 0) continue;
        if ((int)i == mine || (int)i == truck) continue;
        Node& out = TR.nodes[nd.info.output];
        bool info_edge = (nd.info.type == T_CHEM_SRC);
        // recent flow → brighter (tons arrived in the last 2 days, up to now)
        double recent = 0;
        {
            auto it = std::upper_bound(nd.inflow.begin(), nd.inflow.end(), UI.t,
                [](double t, const InflowEvent& e) { return t < (double)e.t; });
            for (auto j = it; j-- != nd.inflow.begin() && j->t > UI.t - 2 * MS_DAY; )
                recent += j->tons;
        }
        float glow = (float)clampd(recent / 200.0, 0.0, 1.0);
        Color c = Fade(info_edge ? COL_ACCENT : out.color, 0.35f + 0.55f * glow);
        draw_edge((int)i, nd.info.output, c, info_edge ? 1.5f : 2.5f + 2 * glow, info_edge);
    }

    // truck + road
    if (truck >= 0) draw_truck(truck);

    // nodes (truck node drawn as the glyph above; skip its box)
    for (size_t i = 0; i < TR.nodes.size(); i++) {
        if ((int)i == truck) continue;
        draw_node((int)i);
    }

    // particles along edges
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
    // ---- keyboard -----------------------------------------------------------
    if (IsKeyPressed(KEY_SPACE)) UI.playing = !UI.playing;
    if (IsKeyPressed(KEY_LEFT)) {
        UI.playing = false;
        UI.t = IsKeyDown(KEY_LEFT_SHIFT) ? std::max(0.0, UI.t - MS_DAY) : (double)TR.ticks[std::max(0, UI.k - 1)];
    }
    if (IsKeyPressed(KEY_RIGHT)) {
        UI.playing = false;
        UI.t = IsKeyDown(KEY_LEFT_SHIFT) ? UI.t + MS_DAY : (double)TR.ticks[std::min((int)TR.ticks.size() - 1, UI.k + 1)];
    }
    if (IsKeyPressed(KEY_PAGE_DOWN)) {
        for (auto& mk : TR.marks) if (mk.t0 > UI.t + 1) { UI.t = mk.t0 + 1; break; }
    }
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

    // ---- timeline scrub -------------------------------------------------------
    Rectangle tr = LY.timeline;
    bool over_tl = CheckCollisionPointRec(UI.mouse, tr);
    float wheel = GetMouseWheelMove();

    if (over_tl && wheel != 0 && !IsKeyDown(KEY_LEFT_SHIFT)) {
        // zoom at cursor
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

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && over_tl) {
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
    if (over_tl && IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) { }   // reserved
    if (over_tl && IsMouseButtonPressed(MOUSE_MIDDLE_BUTTON)) { } // reserved

    // double click = fit
    if (over_tl) {
        static double last_click = -1;
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (GetTime() - last_click < 0.35) { UI.vw0t = 0; UI.vw1t = TR.t_end; }
            last_click = GetTime();
        }
    }

    // ---- graph pan / zoom / select -------------------------------------------
    Rectangle gr = LY.graph;
    bool over_gr = CheckCollisionPointRec(UI.mouse, gr);
    if (over_gr && wheel != 0) {
        Vector2 before = screen_to_world(UI.mouse);
        UI.zoom_t = clampd(UI.zoom_t * (wheel > 0 ? 1.15f : 1 / 1.15f), 0.25f, 4.0f);
        UI.zoom = UI.zoom_t;   // instant zoom, keeps cursor anchored
        Vector2 after = screen_to_world(UI.mouse);
        UI.cam_t.x += before.x - after.x; UI.cam_t.y += before.y - after.y;
        UI.cam = UI.cam_t;
    }

    UI.hover = -1;
    if (over_gr) {
        for (int pass = 0; pass < 2 && UI.hover < 0; pass++)
            for (size_t i = 0; i < TR.nodes.size(); i++) {
                Node& nd = TR.nodes[i];
                if (pass == 0 && nd.info.type == T_TRUCK) continue;   // trucks hit via glyph
                if (pass == 1 && nd.info.type != T_TRUCK) continue;
                Rectangle nr = (nd.info.type == T_TRUCK)
                    ? Rectangle{ truck_pos((int)i).x - 20, truck_pos((int)i).y - 34, 40, 46 }
                    : node_rect((int)i);
                if (CheckCollisionPointRec(UI.mouse, nr)) { UI.hover = (int)i; break; }
            }
        if (UI.hover >= 0) {
            // tooltip
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
            if (TR.nodes[UI.hover].draggable) { UI.drag_node = UI.hover; }
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
    Rectangle r = { GetScreenWidth() / 2.0f - 330, GetScreenHeight() / 2.0f - 240, 660, 480 };
    (void)0;
    DrawRectangleRec(r, { 16, 20, 27, 248 });
    DrawRectangleLinesEx(r, 1, COL_ACCENT);
    float x = r.x + 20, y = r.y + 16;
    text(FB20, x, y, "Controls & legend", COL_TEXT); y += 32;
    const char* lines[] = {
        "timeline   drag = scrub (with inertia)    wheel = zoom at cursor",
        "           shift+wheel = pan    double-click = fit whole run",
        "           hover = crosshair + values tooltip",
        "graph      click node = inspect    drag node = move",
        "           drag background = pan    wheel = zoom",
        "table      click row = inspect that node",
        "",
        "space play/pause    ←/→ step one tick    shift+←/→ step one day",
        "PgUp/PgDn  jump to previous/next notable event",
        "[ ] playback speed   R reload trace   F11 fullscreen   Esc quit",
        "",
        "mark lanes under the timeline:",
        "  amber    mine pad empty (< 20 t)",
        "  blue     acid tank low (< 50 t)",
        "  orange   flotation starved (idle, no feed)",
        "  red      acid supply market disruption",
        "  cyan     S&OP scrum (contract revision)",
        "  green    flotation campaign start",
        "",
        "the chart on the timeline shows every node's stock, each",
        "normalized to its own maximum — shapes, not absolute values.",
        "click a node to make its envelope stand out.",
    };
    for (auto& l : lines) { text(F13, x, y, l, COL_DIM); y += 17; }
}

// ============================================================================
//                                   main
// ============================================================================

static double parse_time_arg(const char* s, i64 t_end) {
    if (!strcmp(s, "end")) return t_end;
    char* endp = nullptr;
    double v = strtod(s, &endp);
    if (endp && *endp == 'd') return v * MS_DAY;
    if (endp && *endp == 'h') return v * MS_HOUR;
    if (endp && *endp == 'y') return v * 365 * MS_DAY;
    return v;   // raw ms
}

int main(int argc, char** argv) {
    const char* path = "trace.bin.gz";
    const char* shot = nullptr;
    const char* sel_name = nullptr;
    const char* at_arg = nullptr;
    bool dump = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--sel") && i + 1 < argc) sel_name = argv[++i];
        else if (!strcmp(argv[i], "--dump")) dump = true;
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) at_arg = argv[++i];
        else path = argv[i];
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | (shot ? FLAG_WINDOW_HIDDEN : 0));
    InitWindow(1800, 1000, "IMS-T8 · simulation viewer");
    SetTargetFPS(60);
    load_fonts();

    if (!load_trace(path)) { fprintf(stderr, "failed to load %s\n", path); return 1; }

    double shot_at = at_arg ? parse_time_arg(at_arg, TR.t_end) : -1;
    UI.vw0 = UI.vw1 = 0; UI.vw0t = 0; UI.vw1t = TR.t_end;
    UI.vw1 = TR.t_end;
    UI.t = shot_at >= 0 ? clampd(shot_at, 0, TR.t_end) : TR.ticks[0] + 1;
    LY.compute();
    default_layout();
    build_frame(UI.t);
    UI.k = FR.k;
    UI.sel = -1;

    if (sel_name)
        for (size_t i = 0; i < TR.nodes.size(); i++)
            if (!strcmp(TR.nodes[i].info.name, sel_name)) UI.sel = (int)i;

    if (dump) {
        build_frame(UI.t);
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
        // single-frame render for testing / automation
        UI.tooltip[0] = 0;
        build_frame(UI.t);
        BeginDrawing();
        ClearBackground(COL_BG);
        LY.compute();
        draw_topbar(); draw_timeline(); draw_graph(); draw_inspector(); draw_table();
        rlDrawRenderBatchActive();          // flush the shape batch to the GPU
        TakeScreenshot("_viewer_shot.png"); // raylib strips directories, save in CWD
        EndDrawing();
        // move to the requested path
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

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        UI.mouse = GetMousePosition();
        UI.tooltip[0] = 0;

        if (UI.reload_request) {
            UI.reload_request = 0;
            size_t old_sel = UI.sel < 0 ? 0 : (size_t)UI.sel;
            if (load_trace(TR.path)) {
                UI.vw1t = TR.t_end;
                FR.k = -2; FR.valid = false;      // data changed: rebuild states
                UI.s24_k = -3;
                build_frame(UI.t);
                UI.sel = (int)std::min(old_sel, TR.nodes.size() - 1);
                PARTS.clear();
                UI.inflow_cursor.clear();
                printf("[viewer] reloaded %s\n", TR.path);
            }
        }

        // playback
        if (UI.playing && !UI.scrubbing) {
            UI.t += SPEEDS[UI.speed_idx] * dt;
            if (UI.t >= TR.t_end) { UI.t = TR.t_end; UI.playing = false; }
        }

        handle_input(dt);

        // animated values
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
