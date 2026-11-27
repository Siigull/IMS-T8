// #############################################################################
// ## sim_trace.h — trace WRITER (stb-style single-include header).          ##
// ##                                                                          ##
// ## Include this file ONCE, from a single translation unit (main.cpp),     ##
// ## AFTER all simulation types and globals are defined. It reaches into    ##
// ## the simulation's globals directly:                                     ##
// ##   g_nodes, g_node_count, g_w, g_additives, N_ELEMS, T_Node, T_ID ...    ##
// ##                                                                          ##
// ## Usage from main():                                                      ##
// ##   trace_set_name(id, "Mine");          // optional, pretty names       ##
// ##   trace_open("trace.bin.gz");           // after all nodes exist+linked ##
// ##   ... inside the tick loop, once per settled tick:                      ##
// ##   trace_tick(ticked_mask);              // after transactions settle    ##
// ##   trace_close();                        // at the end                   ##
// ##                                                                          ##
// ## Everything is delta-encoded against the last logged state, quantized   ##
// ## to f32 for masses. The first logged tick is forced full. See           ##
// ## trace_format.h for the on-disk layout.                                  ##
// #############################################################################
#ifndef SIM_TRACE_H
#define SIM_TRACE_H

#include <cstdio>
#include <cstring>
#include <cmath>
#include <zlib.h>
#include "trace_format.h"

// ---- writer state ----------------------------------------------------------

static gzFile trace_file = NULL;
static char   trace_path[512] = {0};

// Last-logged state, used for delta detection. f32-quantized masses.
typedef struct T_Trace_Cache {
    float   mass[TRACE_MASS_SLOTS];
    int64_t next_tick, chem_last_scrum, disrupted_until;
    int32_t chem_cars, belt_head;
    float   chem_burned, chem_burned_at_scrum;
    uint8_t proc_state, truck_state, conv_state;
    b32     ever_logged;
} T_Trace_Cache;

static T_Trace_Cache trace_cache[TRACE_MAX_NODES];
static char           trace_names[TRACE_MAX_NODES][32];

// Record assembly buffer. Worst case ~280 B per node × 64 nodes.
static unsigned char trace_buf[TRACE_MAX_NODES * 320];
static i64           trace_buf_len = 0;
static i64           trace_ticks_logged = 0;

static void trace_put(const void* p, i64 n) {
    memcpy(trace_buf + trace_buf_len, p, n);
    trace_buf_len += n;
}

// Give a node a human-readable name for the viewer (default = type name).
static void trace_set_name(T_ID id, const char* name) {
    if (id < 0 || id >= TRACE_MAX_NODES) return;
    snprintf(trace_names[id], 32, "%s", name);
}

static const char* trace_default_name(E_Node_Type t) {
    switch (t) {
        case N_MINER:             return "Mine";
        case N_CHEM_SRC:          return "Chem supplier";
        case N_CHEM_DISRUPTION:   return "Chem market";
        case N_CHEM_MONTHLY_SCRUM:return "Chem S&OP";
        case N_ADDITIVE_SRC:      return "Additive src";
        case N_TRUCK:             return "Truck";
        case N_CRUSHER:           return "Crusher";
        case N_CONVEYOR:          return "Conveyor";
        case N_FLOTATION:         return "Flotation";
        case N_TAILINGS:          return "Tailings";
        default:                  return "?";
    }
}

// Open the trace, write the file + node header. Call after every node exists
// and is linked. Returns false (and disables tracing) if the file can't be
// opened. Pass NULL or "-" to run without tracing.
static b32 trace_open(const char* path) {
    if (path == NULL || strcmp(path, "-") == 0) return false;

    snprintf(trace_path, sizeof(trace_path), "%s", path);

    assert(g_node_count + 1 <= TRACE_MAX_NODES, "Too many nodes for the trace (max 64).");

    trace_file = gzopen(path, "wb");
    if (trace_file == NULL) {
        std::cout << "[trace] could not open '" << path << "', tracing disabled\n";
        return false;
    }

    // Header + node table.
    TraceFileHeader h = {};
    memcpy(h.magic, TRACE_MAGIC, 8);
    h.version    = TRACE_VERSION;
    h.elem_count = N_ELEMS;
    const char* elem_names[TRACE_MAX_ELEMS] = {"La", "Ce", "Pr", "Nd", "Dy"};
    for (int e = 0; e < TRACE_MAX_ELEMS; e++)
        snprintf(h.elem_names[e], 8, "%.7s", elem_names[e]);
    h.node_count = g_node_count + 1; // + pseudo additive stock
    h.t_end       = g_w.END;

    gzwrite(trace_file, &h, sizeof(h));

    for (int i = 0; i < g_node_count; i++) {
        TraceNodeInfo ni = {};
        ni.type          = (uint32_t)g_nodes[i].type;
        ni.input         = g_nodes[i].input;
        ni.output        = g_nodes[i].output;
        ni.mass_capacity = g_nodes[i].mass_capacity;
        ni.belt_cells    = (g_nodes[i].type == N_CONVEYOR) ? BELT_CELLS : 0;
        if (trace_names[i][0] == 0) snprintf(trace_names[i], 32, "%s", trace_default_name(g_nodes[i].type));
        snprintf(ni.name, 32, "%.31s", trace_names[i]);
        gzwrite(trace_file, &ni, sizeof(ni));
    }

    // Pseudo node: the imported additive stock (g_additives).
    {
        TraceNodeInfo ni = {};
        ni.type          = TRACE_NODE_ADDITIVE_STOCK;
        ni.input         = -1;
        ni.output        = -1;
        ni.mass_capacity = HUGE_VAL;
        ni.belt_cells    = 0;
        snprintf(ni.name, 32, "Additive stock");
        gzwrite(trace_file, &ni, sizeof(ni));
    }

    memset(trace_cache, 0, sizeof(trace_cache));
    return true;
}

// Log one settled tick: everything that changed since the previous logged
// tick, for every node. `ticked_mask` = bitmask of nodes whose tick() body
// actually ran at g_w.time (the viewer shows it as node activity).
static void trace_tick(u64 ticked_mask) {
    if (trace_file == NULL) return;

    // The first tick ever logged is forced full so the reader gets a
    // baseline. (Nodes whose slots are still 0 simply don't re-emit.)
    b32 first = !trace_cache[0].ever_logged;

    trace_buf_len = 0;

    i64 header_pos = trace_buf_len;
    TraceTickHeader th = {};
    th.time        = g_w.time;
    th.ticked_mask = ticked_mask;
    th.dirty_mask  = 0;
    trace_put(&th, sizeof(th));

    for (int i = 0; i <= g_node_count; i++) {
        b32 is_additive = (i == g_node_count);

        // ---- gather current values of this node -------------------------
        T_Trace_Cache cur = {};
        T_Mass* m; T_Mass* w; T_Mass* o;

        if (is_additive) {
            m = &g_additives; w = m; o = m; // wip/out stay zero
        } else {
            T_Node* n = &g_nodes[i];
            m = &n->mass; w = &n->wip; o = &n->out;
        }

        for (int e = 0; e < N_ELEMS; e++) cur.mass[e]             = (float)m->elem[e];
                                         cur.mass[N_ELEMS]       = (float)m->waste;
        for (int e = 0; e < N_ELEMS; e++) cur.mass[6+e]           = (float)w->elem[e];
                                         cur.mass[6+N_ELEMS]     = (float)w->waste;
        for (int e = 0; e < N_ELEMS; e++) cur.mass[12+e]          = (float)o->elem[e];
                                         cur.mass[12+N_ELEMS]    = (float)o->waste;

        if (!is_additive) {
            T_Node* n = &g_nodes[i];
            for (int c = 0; c < BELT_CELLS; c++) {
                for (int e = 0; e < N_ELEMS; e++)
                    cur.mass[18 + c*6 + e] = (float)n->belt[c].elem[e];
                cur.mass[18 + c*6 + N_ELEMS] = (float)n->belt[c].waste;
            }
            cur.mass[54] = (float)n->chem_mass.chem;

            cur.proc_state   = (uint8_t)n->proc_state;
            cur.truck_state  = (uint8_t)n->truck_state;
            cur.conv_state   = (uint8_t)n->conv_state;
            cur.next_tick    = n->next_tick;
            cur.chem_cars    = (int32_t)n->chem_cars;
            cur.chem_burned  = (float)n->chem_burned;
            cur.chem_burned_at_scrum = (float)n->chem_burned_at_scrum;
            cur.chem_last_scrum     = n->chem_last_scrum;
            cur.belt_head    = (int32_t)n->belt_head;
            if (n->type == N_CHEM_DISRUPTION)
                cur.disrupted_until = g_w.chem_disrupted_until;
        }

        // ---- diff against cache, emit masks + changed values ------------
        T_Trace_Cache* old = &trace_cache[i];
        b32 force = first || !old->ever_logged;

        uint32_t sc_mask = 0;
        u64 ms_mask = 0;

        if (force || old->proc_state   != cur.proc_state)   sc_mask |= 1u<<TR_SC_PROC_STATE;
        if (force || old->truck_state  != cur.truck_state) sc_mask |= 1u<<TR_SC_TRUCK_STATE;
        if (force || old->conv_state   != cur.conv_state)  sc_mask |= 1u<<TR_SC_CONV_STATE;
        if (force || old->next_tick    != cur.next_tick)   sc_mask |= 1u<<TR_SC_NEXT_TICK;
        if (force || old->chem_cars    != cur.chem_cars)    sc_mask |= 1u<<TR_SC_CHEM_CARS;
        if (force || old->chem_burned  != cur.chem_burned)  sc_mask |= 1u<<TR_SC_CHEM_BURNED;
        if (force || old->chem_burned_at_scrum != cur.chem_burned_at_scrum) sc_mask |= 1u<<TR_SC_CHEM_BURNED_AT_SCRUM;
        if (force || old->chem_last_scrum     != cur.chem_last_scrum)      sc_mask |= 1u<<TR_SC_CHEM_LAST_SCRUM;
        if (force || old->disrupted_until     != cur.disrupted_until)      sc_mask |= 1u<<TR_SC_DISRUPTED_UNTIL;
        if (force || old->belt_head    != cur.belt_head)    sc_mask |= 1u<<TR_SC_BELT_HEAD;

        for (int s = 0; s < TRACE_MASS_SLOTS; s++)
            if (force || old->mass[s] != cur.mass[s]) ms_mask |= 1ull << s;

        if (sc_mask == 0 && ms_mask == 0) continue; // node unchanged

        ((TraceTickHeader*)(trace_buf + header_pos))->dirty_mask |= 1ull << i;

        trace_put(&sc_mask, 4);
        trace_put(&ms_mask, 8);

        // scalars, in bit order
        if (sc_mask & (1u<<TR_SC_PROC_STATE))   { uint8_t v = cur.proc_state;   trace_put(&v,1); }
        if (sc_mask & (1u<<TR_SC_TRUCK_STATE))  { uint8_t v = cur.truck_state;  trace_put(&v,1); }
        if (sc_mask & (1u<<TR_SC_CONV_STATE))   { uint8_t v = cur.conv_state;   trace_put(&v,1); }
        if (sc_mask & (1u<<TR_SC_NEXT_TICK))    { int64_t v = cur.next_tick;    trace_put(&v,8); }
        if (sc_mask & (1u<<TR_SC_CHEM_CARS))    { int32_t v = cur.chem_cars;    trace_put(&v,4); }
        if (sc_mask & (1u<<TR_SC_CHEM_BURNED))  { float   v = cur.chem_burned;  trace_put(&v,4); }
        if (sc_mask & (1u<<TR_SC_CHEM_BURNED_AT_SCRUM)) { float v = cur.chem_burned_at_scrum; trace_put(&v,4); }
        if (sc_mask & (1u<<TR_SC_CHEM_LAST_SCRUM)) { int64_t v = cur.chem_last_scrum; trace_put(&v,8); }
        if (sc_mask & (1u<<TR_SC_DISRUPTED_UNTIL)) { int64_t v = cur.disrupted_until;  trace_put(&v,8); }
        if (sc_mask & (1u<<TR_SC_BELT_HEAD))   { int32_t v = cur.belt_head;    trace_put(&v,4); }

        // masses, in bit order
        for (int s = 0; s < TRACE_MASS_SLOTS; s++)
            if (ms_mask & (1ull << s)) trace_put(&cur.mass[s], 4);

        *old = cur;
        old->ever_logged = true;
    }

    gzwrite(trace_file, trace_buf, (unsigned)trace_buf_len);
    trace_ticks_logged++;

    // Keep the file readable while the sim runs (live tail / reload).
    if ((trace_ticks_logged & 0xFF) == 0) gzflush(trace_file, Z_SYNC_FLUSH);
}

static void trace_close() {
    if (trace_file == NULL) return;

    gzflush(trace_file, Z_FINISH);
    gzclose(trace_file);
    trace_file = NULL;

    std::cout << "[trace] " << trace_ticks_logged << " ticks -> " << trace_path << "\n";
}

#endif // SIM_TRACE_H
