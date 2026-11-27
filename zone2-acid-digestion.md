# Zone 2: Acid digestion + chemical supply — pseudocode + reality

Two objects:

- **CHEM_SUP** — an inventory process (one consumable reagent, H₂SO₄).
- **DIGEST** — a batch server with a *resource precondition* (needs feed **and**
  reagent to start) whose completion applies a mass map and splits output into
  two streams.

Material everywhere is a vector `m = (m[La], m[Ce], m[Pr], m[Nd], m[Dy], gangue)`.
Both steps preserve element ratios — acid dissolves minerals, not elements.
First selectivity appears in Zone 3, as in reality.

---

## 1. Reality in brief

Zone 1's product is still rock. Solvent extraction needs RE³⁺ ions in a liquor.
The industrial fix (Bayan Obo, Chinese plants, Lynas Kuantan):

```
concentrate (50–60 % REO) + 93–98 % H2SO4
    → rotary kiln, 250–500 °C, 1–4 h          "acid bake": lattice destroyed,
    │                                          RE → water-soluble RE2(SO4)3
    │                                          off-gas CO2 + HF → scrubber
    ↓
water leach tanks, 2–6 h  →  solid/liquid separation
    ├── liquor  = PLS  (RE sulfate, tens g/L, pH 1–2)   → to Zone 3
    └── solids  = residue (gypsum, barite, silica, Fe, Th) → residue pond
```

Key reactions:

```
2 REFCO3 + 3 H2SO4 → RE2(SO4)3 + 2 HF↑ + 2 CO2↑ + 2 H2O↑   (bastnäsite)
   CaCO3 + H2SO4    → CaSO4↓ + CO2↑ + H2O                  (gangue → gypsum)
   Th (from monazite) reports to the residue                (the regulatory story)
```

Real numbers [M, unless marked W=web-verified]:

| quantity | value |
|---|---|
| end-to-end campaign time | 6–16 h (bake + leach + filter) |
| acid consumption | 1.0–1.5 t H₂SO₄ / t concentrate |
| REO leached into PLS | 90–98 % |
| residue | 0.6–0.8 t / t concentrate, Th-bearing |
| acid logistics | ~85 t per rail car [W]; tank farm = days–weeks of cover; deliveries slip right-skewed |

Acid is bought as a bulk commodity through a standing reorder loop — a slipped
train means the front end freezes while the SX cascade keeps drinking the PLS
tank. That is the assignment's "delayed chemical deliveries" scenario, and it
lives entirely in these two objects.

---

## 2. CHEM_SUP — inventory process

```
state:
    stock          # t H2SO4 in the tank farm
    on_order       # bool
    arrival_time   # time the outstanding order lands

on delivery  (t == arrival_time):
    stock += LOT                        # 350 t
    on_order = false

on daily review:
    if not on_order and stock <= REORDER_POINT:   # 300 t ≈ 9 d of cover
        on_order  = true
        arrival_time = t + LogNormal(median = 4 d, sigma = 0.5)

consumption:
    DIGEST subtracts from stock when it charges a campaign
```

**Reality:** the purchasing agent checks the tank gauge daily; at ~9 days of
cover a 350 t order goes out; the train comes when it comes — usually ~4 days,
occasionally 10+ (lognormal tail: supplier allocation, rail, market tightness).
Nothing in the plant is in this loop; the plant only sees the tank level.

---

## 3. DIGEST — batch server

```
state:
    feed        # concentrate buffer (in)
    campaign    # material inside kiln + leach train
    pls         # product buffer, PLS, measured in contained-REO tons
    phase       # IDLE | RUNNING | BLOCKED

constants/draws per campaign:
    a    ~ Tri(0.9, 1.2, 1.5)      # t acid / t concentrate (gangue reactivity
                                  #   wanders with feed grade)
    rate ~ Tri(1.0, 1.6, 2.4) t/h  # bake + leach + filter, one lumped server
    r    ~ Tri(0.90, 0.945, 0.98)  # leach recovery, drawn at completion;
                                  #   one scalar for ALL elements


IDLE:
    # start needs BOTH inputs — two distinct starvation modes:
    if tons(feed) < MIN_CAMPAIGN:      wait          # starved on concentrate
    if chem.stock < a * tons(feed):    wait          # starved on reagent

    campaign  = feed                    # take the whole buffer
    feed      = empty
    chem.stock -= a * tons(campaign)
    duration  = tons(campaign) / rate
    phase = RUNNING;  set timer(duration)


RUNNING, on timer:
    # release gated by product space (DONE_BLOCKED):
    if REO(pls) + REO(campaign) * r > PLS_CAP:   phase = BLOCKED; wait
                                               # freed when pls is drained

    # ---- the mass map: the only place mass/composition change ----
    for each element e:
        pls[e]      += campaign[e] * r
        residue[e]  += campaign[e] * (1 - r)      # → waste sink
    residue.gangue += campaign.gangue              # → waste sink
                     + 0.25 * acid_used            # gypsum etc.
    # off-gas (CO2 + HF, remainder of acid) leaves the system

    campaign = empty
    phase = IDLE
```

**Reality:**

- *Start condition* — the control room charges a campaign only with a minimum
  kiln feed **and** enough acid for this concentrate's stoichiometry (`a`
  varies because the calcite share of the feed wanders with mine grade).
- *Duration* — kiln residence + leach + filtration lumped; ~12 t campaign at
  ~1.6 t/h ≈ 7.5 h, matching real 6–16 h batch trains.
- *Mass map* — the whole of the chemistry. `r` is element-uniform because
  recovery is mineral-driven (a sulfate can't prefer Nd over Ce). PLS is
  tracked as contained-REO tons; residue takes the gangue, the unleached REO,
  and the acid-derived gypsum. CO₂/HF off-gas leaves the books.
- *Blocking* — if the PLS tank is full the liquor waits inside; a full
  downstream step physically backs up the filter discharge.

Stuck states and their rescuers (this is the complete state machine):

| stuck | rescued by |
|---|---|
| starved on concentrate | concentrate arriving |
| starved on acid | a delivery landing (§2) |
| BLOCKED (pls full) | pls being drained |

---

## 4. Parameters (sized against Zone 1's 1.16 t/h concentrate)

| param | value | anchor |
|---|---|---|
| MIN_CAMPAIGN | 8 t | kiln minimum charge; creates natural batching |
| rate | Tri(1.0, 1.6, 2.4) t/h | utilization ~73 %, next-tight server after flotation |
| a | Tri(0.9, 1.2, 1.5) t/t | stoichiometry + excess |
| r | Tri(0.90, 0.945, 0.98) | leach recoveries |
| PLS_CAP | 30 t REO | ~2 days of cascade feed |
| feed cap | 40 t | surge before the kiln |
| acid: LOT / reorder / tank | 350 / 300 / 650 t | ~10.5 d per lot; cap > reorder + lot ⇒ no clamping |
| lead | LogNorm(4 d, 0.5) | P95 ≈ 10.7 d |

## 5. Sanity checks

- Steady state: 1.16 t/h conc → 0.64 t/h REO → PLS 0.60 t/h REO, residue ≈ 0.9
  t/h (≈ 0.78 t/t concentrate ✓ §1), acid burn 33 t/day.
- Conservation per element: campaign REO = PLS REO + residue REO, exact by the
  map. Acid: deliveries = consumed + Δstock.
- Acid sawtooth: order at 300 t, 4 d lead burns 134 t → lands at 166, +350 → 516.
  Stockout needs lead > 9 d ≈ 5 % of orders ≈ one event / 7 months, 1–4 days long.
- Delay experiment, spread speeds: tank dry → digest freezes → PLS (30 t)
  empties in **~2 days** (cascade starves); feed + Q_CONC (192 t) absorb
  **~6.9 days** before flotation blocks; recovery at 1.6 − 1.16 t/h, i.e.
  ~2.6× slower than the outage lasted.

## 6. Dropped on purpose (one line each)

Campaign internals (temp profiles, wash ratios); per-element leach deltas
(Ce⁴⁺, HREE double-sulfates); PLS as a real solution (m³, g/L, acidity,
impurities); off-gas/fluoride; thorium regulation; water as a reagent;
multiple suppliers/expediting; kiln failures. The long version of this ledger,
sources, and engine-integration notes were in the previous draft of this file —
ask if you want any of it back.
