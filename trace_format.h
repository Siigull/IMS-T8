// #############################################################################
// ## Trace format v1 — shared contract between the simulator (writer) and  ##
// ## the viewer (reader).                                                    ##
// ##                                                                          ##
// ## The simulator is event-driven: it does not have a fixed tick rate, it   ##
// ## jumps between scheduled event times. One "tick" = one settled event     ##
// ## time. The trace logs, for every tick, the delta of every node state     ##
// ## that changed since the previous tick. A reader replays deltas to get   ##
// ## the full state at any tick.                                             ##
// ##                                                                          ##
// ## The file is gzip-compressed raw little-endian structs (zlib gz* API in  ##
// ## both writer and reader; the first two bytes are 0x1F 0x8B when the      ##
// ## file is gzipped, which the reader uses to auto-detect).                 ##
// #############################################################################
#ifndef TRACE_FORMAT_H
#define TRACE_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACE_MAGIC       "IMSTRC01"   // 8 bytes
#define TRACE_VERSION     1

// Bitmasks are u64 → at most 64 logged nodes (sim asserts on more).
#define TRACE_MAX_NODES   64

// La, Ce, Pr, Nd, Dy — the basket elements.
#define TRACE_MAX_ELEMS   5
#define TRACE_ELEM_SLOTS  (TRACE_MAX_ELEMS + 1)   // elems + waste

// Mass slots per node, in bit order of the u64 mass mask:
//   [ 0.. 5]  mass.elem[0..4], mass.waste
//   [ 6..11]  wip.elem[0..4],  wip.waste
//   [12..17]  out.elem[0..4],  out.waste
//   [18..53]  belt[c].elem[0..4], belt[c].waste  (c = 0..5, conveyors only)
//   [54]      chem_mass.chem
#define TRACE_MASS_SLOTS  (TRACE_ELEM_SLOTS*3 + 36 + 1)   // 55
#define TRACE_MASS_ALL    0x7FFFFFFFFFFFFFULL

// Scalar slots (bit order of the u32 scalar mask; values are appended to
// the record in this bit order):
enum {
    TR_SC_PROC_STATE          = 0,   // u8
    TR_SC_TRUCK_STATE         = 1,   // u8
    TR_SC_CONV_STATE          = 2,   // u8
    TR_SC_NEXT_TICK           = 3,   // i64
    TR_SC_CHEM_CARS           = 4,   // i32
    TR_SC_CHEM_BURNED         = 5,   // f32
    TR_SC_CHEM_BURNED_AT_SCRUM= 6,   // f32
    TR_SC_CHEM_LAST_SCRUM     = 7,   // i64
    TR_SC_DISRUPTED_UNTIL     = 8,   // i64  (world global, logged on the
                                      //        N_CHEM_DISRUPTION node)
    TR_SC_BELT_HEAD           = 9,   // i32
    TR_SC_COUNT
};
#define TRACE_SCALAR_ALL   0xFFFFu

// Pseudo node type appended after the real nodes: the imported-additives
// stock (g_additives), which is a world blob, not a node.
#define TRACE_NODE_ADDITIVE_STOCK  1000u

#pragma pack(push, 1)

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t elem_count;                 // N_ELEMS in the sim (== TRACE_MAX_ELEMS)
    char     elem_names[TRACE_MAX_ELEMS][8];
    uint32_t node_count;                 // real nodes + pseudo additive node
    int64_t  t_end;                      // sim horizon (C_World.END), for scaling
    // followed by node_count × TraceNodeInfo
} TraceFileHeader;

typedef struct {
    uint32_t type;                      // E_Node_Type (or TRACE_NODE_ADDITIVE_STOCK)
    int64_t  input;                     // link ids, -1 = none
    int64_t  output;
    double   mass_capacity;             // HUGE_VAL = unlimited
    int32_t  belt_cells;                // > 0 for conveyors
    char     name[32];
} TraceNodeInfo;                         // 64 bytes

typedef struct {
    int64_t  time;                      // sim ms of this settled tick
    uint64_t ticked_mask;               // nodes whose tick() body ran at this time
    uint64_t dirty_mask;                // nodes that carry a delta below
    // followed by, for each set bit of dirty_mask in ascending node order:
    //   uint32_t scalar_mask
    //   uint64_t mass_mask
    //   then scalar values (bit order, packed sizes), then f32 mass values
} TraceTickHeader;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif // TRACE_FORMAT_H
