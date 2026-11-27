# Zone 2: Acid digestion + chemical supply — pseudocode + reality

Two objects:

- **CHEM_SUP** — a contracted delivery stream for one consumable reagent
  (H₂SO₄): scheduled arrivals, occasionally suspended by market/logistics
  events.
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

Acid is not "ordered" — it arrives under a standing contract on a fixed
schedule of rail cars / tanker trucks into a tank farm holding days–weeks of
cover; occasionally the market or the railway suspends the stream (the
2022–23 global acid crunch is the canonical example) and the front end
freezes while the SX cascade keeps drinking the PLS tank. That is the
assignment's "delayed chemical deliveries" scenario, and it lives entirely
in these two objects.

---

## 2. CHEM_SUP — regular contracted deliveries + a planning loop

Two timescales over one stream:

- **The stream is regular.** Fixed car size, steady cadence. How much arrives
  per week is a *planning decision*, not a reflex.
- **Planning (slow loop).** Monthly, procurement looks at trailing burn and
  the tank level and adjusts the contracted rate within the contract's flex
  band: "we're low → more per week, from next month on."
- **Disruptions (exogenous).** Market/logistics events suspend the stream for
  days–weeks; planning can't prevent them, only absorb them.

```
state:
    stock             # t H2SO4 in the tank farm
    next_due          # next contracted delivery
    planned_rate      # t/day the contract delivers — the planning decision
    next_review       # monthly procurement review
    disrupted_until   # end of active supply event (-inf = none)
    next_event        # next disruption onset

on delivery (t == next_due):
    if t < disrupted_until:              # event active: car slipped
        next_due = disrupted_until       # stream resumes at recovery
    else if stock + LOT > TANK_CAP:      # tank full: car waits in the siding
        next_due += LOT / planned_rate
    else:
        stock    += LOT                  # 85 t, one rail car [W]
        next_due  = t + LOT / planned_rate

on monthly review (t == next_review):
    burn30 = acid burned over trailing 30 d / 30         # trailing actual
    want   = burn30 + (TARGET - stock) / CATCHUP_TIME    # close the deficit
    planned_rate = clamp(want, 0.8 * burn30, 1.25 * burn30)  # flex band
    next_review = t + 30 d
    # the new rate governs deliveries from here on; add an explicit ~3-week
    # negotiation lag if you want the full S&OP inertia

on disruption onset (t == next_event):
    disrupted_until = t + LogNormal(median = 6 d, sigma = 0.6)
    next_event      = t + Exponential(mean = 120 d)

consumption:
    DIGEST subtracts from stock when it charges a campaign
```

**Reality:** a plant burning 33 t/day receives one 85 t rail car every ~2.5
days — that regular stream is the contract. The monthly review is the S&OP
loop: real acid contracts are annual take-or-pay volumes with a ±15–25 %
flex band; a sustained shortage is answered by renegotiating the band, which
takes review + negotiation + catch-up ≈ **~2 months** — the "can't respond
quickly" property extends to reagents. The deficit term also ramps the
contract with the plant (trailing burn ≈ 0 during náběh) and speeds recovery
after a stockout, capped by the band. Disruptions (2022–23 acid squeeze:
allocations, ~3× price) are what the band and the tank farm exist to absorb.

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
| acid: LOT / planned rate | 85 t / 33.4 t/day | one rail car [W]; nominal cadence 2.55 d |
| planning | review 30 d, catch-up 30 d, flex 0.8–1.25× burn | S&OP loop; contract tolerance band |
| tank / target | 400 / 200 t | ~6 d of cover at target |
| disruption | onset Exp(120 d), length LogNorm(6 d, σ 0.6) | a few market/logistics events per year |

## 5. Sanity checks

- Steady state: 1.16 t/h conc → 0.64 t/h REO → PLS 0.60 t/h REO, residue ≈ 0.9
  t/h (≈ 0.78 t/t concentrate ✓ §1), acid burn 33 t/day.
- Conservation per element: campaign REO = PLS REO + residue REO, exact by the
  map. Acid: deliveries = consumed + Δstock.
- Acid stock: regular ~2.55 d sawtooth (amplitude 85 t) around the 200 t
  target; the planning loop holds it there (rate = burn + deficit/30 d, flex
  ≤ 1.25×). Stockouts: a median 6 d event defers ~1.4 cars (120 t) — the tank
  at ~200 t runs (nearly) dry exactly as the stream resumes; a P90 event
  (~13 d) defers ~4 cars → dry after ~6 d → ~7 d frozen front end, then ~1
  month of lean recovery (catch-up capped at +25 %). ~15 events / 5 yr,
  roughly a third bite. Response to any sustained change ≈ 2 months.
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
