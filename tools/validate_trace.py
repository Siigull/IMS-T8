#!/usr/bin/env python3
"""Quick validation of trace.bin.gz — replays deltas and checks totals."""
import struct, gzip, sys

path = sys.argv[1] if len(sys.argv) > 1 else "trace.bin.gz"
raw = gzip.open(path, "rb").read()
pos = 0

def take(fmt):
    global pos
    sz = struct.calcsize(fmt)
    v = struct.unpack_from(fmt, raw, pos)
    pos += sz
    return v

magic, version, elem_count = take("<8sII")
assert magic == b"IMSTRC01", magic
names = [take("<8s")[0].rstrip(b"\0").decode() for _ in range(elem_count)]
node_count, t_end_hdr = take("<Iq")
print(f"version={version} elems={names} nodes={node_count} t_end_day={t_end_hdr/86_400_000:.0f}")

nodes = []
for i in range(node_count):
    ntype, inp, outp = take("<Iqq")  # u32 + i64 + i64
    cap, = take("<d")
    belt_cells, = take("<i")
    name = take("<32s")[0].rstrip(b"\0").decode()
    nodes.append(dict(type=ntype, input=inp, output=outp, cap=cap, belt=belt_cells, name=name))
    print(f"  [{i}] {name:20s} type={ntype:3d} in={inp} out={outp} cap={cap:.0f} belt={belt_cells}")

# state per node
S = [dict(mass=[0.0]*6, wip=[0.0]*6, out=[0.0]*6, belt=[0.0]*36, chem=0.0,
          next_tick=0, belt_head=0, proc=0, truck=0, conv=0, cars=0, burned=0.0,
          b_scrum=0.0, l_scrum=0, disrupt=-1) for _ in range(node_count)]

def slot(st, s):
    if s < 6: return st["mass"], s
    if s < 12: return st["wip"], s-6
    if s < 18: return st["out"], s-12
    if s < 54: return st["belt"], s-18
    return None, s-54

nticks = 0
last_time = 0
t_first = t_last = None
try:
    while pos < len(raw):
        time, ticked, dirty = take("<qqQ")
        nticks += 1
        t_first = time if t_first is None else t_first
        last_time = time
        for i in range(node_count):
            if not (dirty >> i) & 1: continue
            smask, = take("<I")
            mmask, = take("<Q")
            st = S[i]
            if smask & (1<<0): st["proc"], = take("<B")
            if smask & (1<<1): st["truck"], = take("<B")
            if smask & (1<<2): st["conv"], = take("<B")
            if smask & (1<<3): st["next_tick"], = take("<q")
            if smask & (1<<4): st["cars"], = take("<i")
            if smask & (1<<5): st["burned"], = take("<f")
            if smask & (1<<6): st["b_scrum"], = take("<f")
            if smask & (1<<7): st["l_scrum"], = take("<q")
            if smask & (1<<8): st["disrupt"], = take("<q")
            if smask & (1<<9): st["belt_head"], = take("<i")
            for s in range(55):
                if mmask & (1<<s):
                    v, = take("<f")
                    if s == 54: st["chem"] = v
                    else:
                        arr, idx = slot(st, s)
                        arr[idx] = v
except Exception as e:
    print(f"(stopped mid-record at {pos}: {e})")

DAY = 86_400_000
print(f"\nticks={nticks} first_day={t_first/DAY:.1f} last_day={last_time/DAY:.1f}")
tot = lambda st: sum(st["mass"]) + sum(st["wip"]) + sum(st["out"]) + sum(st["belt"]) + st["chem"]
print("\nfinal state (should match the sim's console report at day 1824):")
for i, n in enumerate(nodes):
    st = S[i]
    mass = sum(st["mass"]); wip = sum(st["wip"]); out = sum(st["out"])
    print(f"  {n['name']:20s} mass={mass:10.1f} wip={wip:8.1f} out={out:10.1f} chem={st['chem']:7.1f} belt={sum(st['belt']):6.1f} proc={st['proc']} truck={st['truck']} next_day={st['next_tick']/DAY:.2f}")
