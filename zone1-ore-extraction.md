# Zone 1: Ore extraction — how it is done in reality, and how it is simplified

This document covers the first step of the chain — turning run-of-mine ore into
mineral concentrate (`ORE_SRC → PREP → Q_CONC`). It has three parts:

1. **Reality** — what the step actually is in a real concentrator: the unit
   operations, the physics/chemistry, real plants and their numbers.
2. **Simplification** — the mathematical object the simulation replaces all of
   that with (equations, distributions, parameters, engine semantics).
3. **The ledger** — every simplification, what it throws away, and when that
   matters.

> **Research status.** Live web search/fetch was unavailable while writing this
> (HTTP 402 on the local search API, verified twice). All quantitative claims
> below are literature-typical values from standard references
> (Gupta & Krishnamurthy 2016; Zhang & Edwards 2012; Jordens et al. 2013;
> Castor 2008; BGS 2011; MP Materials / Lynas filings; Iluka HMS overview;
> Wills' *Mineral Processing Technology*), cross-checked against the sources
> already cited in `mermaid-explain.md`. Confidence is flagged **[H]/[M]/[L]**
> (high / medium / low). Treat exact figures as calibration ranges, not plant
> data. The raw research brief is in `research.md`.

> **Terminology.** In industry this step is called **beneficiation** or
> **(ore) concentration** / mineral processing. "Extraction" alone usually
> means the solvent-extraction chemistry of Zone 3. Here "ore extraction" =
> the Zone 1 pair ORE_SRC + PREP: mining (as a stochastic source) plus
> concentration (as the PREP processor).

---

## 1. What the step is — in one sentence

Upgrade rock that contains ~5–10 % REO into a mineral concentrate of ~50–60 %
REO **while preserving the element ratios of the ore**.

The ratio preservation is the physical root of the whole assignment: this step
separates **minerals** (bastnäsite grains from calcite/barite gangue), not
**elements**. A bastnäsite grain is a single crystal whose lattice holds the
whole lanthanide basket in fixed stoichiometric proportions; you cannot float
the Nd out of it and leave the Ce. Whatever the mine feeds, the concentrate
inherits — which is why the plant must process the entire "basket" (mostly
low-demand Ce, La) to get at the few wanted elements (Nd, Pr), and why demand
imbalance turns into inventory, not into mining choices.

---

## 2. How it is done in reality

### 2.1 The feed: what the rock looks like

The commercially dominant hard-rock REE ores are **carbonatite-hosted
bastnäsite** deposits:

* **Bastnäsite** (Ce,La,Nd,Pr…)CO₃F — a fluorocarbonate, ~70–75 % REO as a
  pure mineral **[H]**. The workhorse at Mountain Pass (USA), Bayan Obo
  (China), Maoniuping (China).
* **Monazite** (Ce,La,…)PO₄ — a phosphate, ~55–70 % REO + 4–10 % ThO₂
  **[H]**. Occurs in carbonatites as a minor phase and, separately, as a
  heavy-mineral-sand mineral. The thorium makes monazite concentrates a
  regulatory liability (many Western sand plants reject it to tailings).
* **Gangue** at a bastnäsite carbonatite: calcite/dolomite (~40–50 % of rock),
  barite (~10–20 %), silicates **[H]**. This is the whole difficulty:
  calcite is also a carbonate with nearly identical surface chemistry, and
  barite has nearly identical density.

Head grades (REO in run-of-mine ore): Mountain Pass ~7–9 % (historical,
~8.6 % often quoted) **[M]**, modern ~6–7 % **[M]**; Bayan Obo ~5–6 % **[M]**;
Mount Weld (Australia, supergene-enriched) ~8–15 % — among the world's
highest **[M]**.

The basket of a bastnäsite concentrate is fixed by mineral stoichiometry —
Mountain Pass-like **[M]**:

| La₂O₃ | CeO₂ | Pr₆O₁₁ | Nd₂O₃ | Sm… | Dy and heavier |
|---|---|---|---|---|---|
| ~33 % | ~50 % | ~4 % | ~11–13 % | ~1.3 % | **trace (<0.1 %)** |

Note the last column: **hard REEs (Dy, Tb, Y) are essentially absent from
bastnäsite.** Real Dy supply comes from ion-adsorption clays (China, Myanmar)
and xenotime, not from carbonatite concentrates.

### 2.2 Mining and the ROM pad

Open pit (Mountain Pass, Bayan Obo): drill, blast, haul by truck to a
run-of-mine (ROM) pad. Grade control runs on blast-hole assays; different
mining faces and stockpile blending decide the feed grade on a scale of
**days**, i.e. real grade is *autocorrelated drift*, not independent noise
**[M]**. The mine, not the concentrator, is the origin of feed variability —
the concentrator mostly reacts.

### 2.3 Comminution (crushing and grinding): liberation

Comminution has exactly one purpose: **detach mineral grains from gangue**
(liberation), because all downstream separations act on whole particles. A
typical hard-rock flowsheet **[H]**:

1. **Primary crushing** — gyratory crusher, product ~150–250 mm.
2. **Coarse stockpile** (hours–days of buffer; reclaimed at a controlled rate).
3. **Secondary + tertiary cone crushing**, in closed circuit with vibrating
   screens, to ~10–20 mm ("crusher fines").
4. **Fine-ore bins** (minutes–hours).
5. **Grinding** — rod mill (open) then ball mill in closed circuit with
   hydrocyclones, to a flotation feed of roughly **P80 ≈ 45–105 µm**
   (Mountain Pass descriptions quote ~70 % passing 74 µm) **[M]**.
6. **Desliming cyclones** cut ~10–20 µm slimes, which would otherwise consume
   collector and wreck selectivity **[M]**.

Comminution is the plant's dominant energy consumer (order 10–20 kWh/t; Bond's
law E ∝ 1/√P₈₀ context only). Liner wear and mill availability are the main
mechanical-failure sources **[M]**.

### 2.4 Why separation is hard (the physics)

Three candidate physical properties, and why each is only partially usable:

* **Density**: bastnäsite SG ≈ 5, barite ≈ 4.5, calcite ≈ 2.7. Gravity
  (spirals, tables) works on sands and weathered ores but cannot split
  bastnäsite from barite **[H]**.
* **Magnetism**: bastnäsite is weakly paramagnetic; monazite more usefully so.
  Wet high-intensity magnetic separation (WHIMS) is a real process route for
  monazite; at Bayan Obo low-intensity separation first pulls the magnetite
  **[H]**.
* **Surface chemistry**: the battleground. Bastnäsite and calcite are both
  semi-soluble carbonates — the same collectors that stick to one stick to the
  other. Getting selectivity is the entire art of REE flotation **[H]**.

### 2.5 Froth flotation — the core machine for bastnäsite

**Mechanism.** In an agitated cell, air is dispersed as bubbles. Particles whose
surface is made **hydrophobic** by a *collector* attach to bubbles and rise
into a froth (stabilized by a *frother*), which overflows as concentrate;
hydrophilic particles sink as tailings. It is a **rate process**: recovery of a
particle depends on its floatability, size, and time in the bank — never 100 %.

**Reagent suite** (Mountain Pass archetype) **[M]**:

* Collector: **fatty acids** (oleic acid, tall oil) ~0.3–1.0 kg/t, or more
  selective but slower/dearer **alkyl hydroxamates** (C7–C9).
* Depressants: **sodium silicate** (silicates, barite), **starch,
  quebracho, lignin sulfonate** (calcite).
* pH ~8–9.5 with soda ash.
* The Mountain Pass signature: **hot flotation** — pulp is thickened to
  ~45–55 % solids and **conditioned with direct steam to ~60 °C**, which
  sharply improves bastnäsite/calcite selectivity and is kept through roughing
  **[H]**.

**Circuit topology** — this is a *network*, not one machine **[M]**:

```
conditioners (steam, 5–20 min)
   └─> ROUGHER bank (10–30 min; bastnäsite floats slowly)
          ├─ rougher tails ──> SCAVENGER bank (10–20 min)
          │       ├─ scavenger tails ──> tailings thickener / dam
          │       └─ scavenger conc ─┐ (middlings)
          └─ rougher conc ──────────┤
                        v           v
                 CLEANER 1 (8–15 min) ── cleaner tails back to rougher feed
                        v                  (sometimes via regrind P80 20–35 µm)
                 CLEANER 2 / recleaner
                        v
                 concentrate thickener (2–6 h) ── filter (0.5–2 h) ── dryer
                        v
                 concentrate storage / shipping (~60 % REO)
```

The recirculating **middlings** (cleaner tails + scavenger concentrate) are
typically a large fraction of the circulating load — the circuit is a
steady-state loop, in continuous operation 24/7. There is no "batch" anywhere:
material streams as slurry. Total pulp-to-concentrate circuit time is of
order **1–3 h** **[M]**.

### 2.6 The other routes (monazite, Bayan Obo)

* **Heavy-mineral sands** (the monazite route): wet gravity plant (rougher →
  cleaner → recleaner **spiral** stages; a start handles ~1–4 t/h) upgrades
  2–10 %-HM sand to a 90–97 % heavy-mineral concentrate at 85–95 % HM
  recovery; then a **dry mill**: low-intensity magnets remove magnetite,
  high-tension rolls split conductors (ilmenite/rutile) from non-conductors,
  WHIMS/tables pull the paramagnetic **monazite** (~55–60 % REO + Th)
  **[H]**. Gravity + magnetism, no flotation chemistry.
* **Bayan Obo**: an iron mine first. Weak-intensity magnetic separation takes
  magnetite; REE minerals (fine bastnäsite + monazite intergrowths) are
  **floated off the iron tailings** (naphthenic-acid soaps historically,
  hydroxamate-type collectors later, e.g. "H205"), regrinding to ~95 % –44 µm
  for liberation. Product ~50 % REO concentrate **[M]**. Because the plant is
  optimized for iron, deposit-wide REE recovery was historically very low
  (<10–15 %) — most of the REE sits in the tailings dam, a known future
  resource **[M]**.
* **Mount Weld / Lynas**: supergene secondary monazite, crushed, ground,
  rougher/cleaner flotation with fatty-acid/hydroxamate + sodium silicate;
  ships a ~40 % REO concentrate to a separate cracking plant **[M]**.

### 2.7 What real plants achieve

| Plant | Head grade % REO | Concentrate % REO | Concentrator REE recovery | Archetype |
|---|---|---|---|---|
| Mountain Pass, historical | 7–9 | ~60 | ~70–85 **[M]** | steam-hot fatty-acid flotation |
| Mountain Pass, modern (MP Materials) | ~6–7 | ~60 | ~70–80 **[M/L]** | same, refined reagents |
| Bayan Obo / Baotou | 5–6 | ~50 | stage 50–75; deposit-wide historically <15 **[M]** | REE floated off iron tailings |
| Mount Weld (Lynas) | 8–15 | ~40 | ~80–90 **[M]** | flotation of secondary monazite |
| Mineral-sands monazite byproduct | 2–10 % HM | 55–60 | 50–90 where recovered | gravity + magnetics |

**Worked mass balance** (the reality check the model must reproduce): 100 t of
ore at 8.6 % head grade, 85 % recovery, 60 % concentrate grade →
100 × 0.086 × 0.85 / 0.60 ≈ **12.2 t of concentrate**; the other ~88 t goes to
the tailings dam, carrying ~1.3 t of unrecovered REO. Mass fraction to
concentrate ≈ (head × recovery) / concentrate grade.

### 2.8 Where the randomness really comes from

| Real-world source | Character |
|---|---|
| Feed grade & basket | geology + blending — slow, **autocorrelated** drift over days (faces), near-constant for a given deposit |
| Recovery | particle size/liberation distribution, slimes, reagent freshness and dosing, water chemistry, operator setpoints — per-shift fluctuation around a circuit-tuned mean |
| Timing | continuous flow; disruptions come from mechanical availability (crusher liners, mill motors, pumps), not from per-batch variation |
| Failures | wear items and pumps, hours–days to repair; hot-flotation steam supply is a single point of failure |

---

## 3. The simplification: one map, two servers, one buffer

### 3.1 The abstraction

Replace mine + concentrator with:

```
ORE_SRC          CRUSHER            FLOTLINE                Q_CONC
(batch source)   (server, T_c)      (server, T_f)           (buffer, 150 t)
   Δ ~ Exp(10h)    M ~ U(80,120)t     T_f ~ Tri(5,7,11)h      ↓ to Zone 2
   M, g, x ~ ...   T_c ~ U(1,1.5)h    mass-balance map        capacity gating
                                       (equations below)
```

Three claims:

* **The transformation is a constant-grade, ratio-preserving mass map.** All
  of flotation chemistry collapses into one random scalar: recovery.
* **All dynamics is queueing.** Two tandem single servers + a capacity-limited
  output buffer produce every phenomenon the assignment cares about upstream
  (blocking when downstream is slow, starvation, propagation of outages).
* **All stochasticity is geology + process noise**, reduced to five
  independent draws per batch plus arrival times.

Formally: PREP ≈ a tandem M/G/1 · M/G/1 queue whose service completion applies
a random diagonal mass map, followed by a finite store. That is the whole
step.

### 3.2 The math

For each ore batch *k*:

**Arrival process** (the mine):

1. Inter-arrival: Δₖ ~ Exp(λ = 1/10 h) — a Poisson mine (memoryless gaps).
2. Batch mass: Mₖ ~ U(80, 120) t.
3. Head grade: gₖ ~ TruncNormal(μ = 0.08, σ = 0.01; [0.05, 0.11]) —
   truncated so the grade stays physical.
4. Element basket: xₖ ~ Dirichlet(α), α = (La, Ce, Pr, Nd, Dy) =
   (2.4, 5.0, 0.55, 1.7, 0.18). Drawn as five Gamma(α_j, 1) variates,
   normalized (see `ore_concentration.cc::DrawBasket`).

**Process** (the concentrator):

5. Service times: T_c ~ U(1.0, 1.5) h (crushing), T_f ~ Tri(5, 7, 11) h
   (flotation line).
6. Recovery: r ~ Tri(lo = 0.72, mode = 0.80, hi = 0.88).

**The mass map** (applied at flotation completion):

7. REO captured:  m_REO = M · g · r
8. Concentrate mass:  m_conc = m_REO / g_c,  with constant g_c = 0.55
9. Per-element mass:  m_e = m_REO · x_e  for each e ∈ {La, Ce, Pr, Nd, Dy}
   (Σ_e x_e = 1, so Σ_e m_e = m_REO — mass-balance invariant)
10. Implicit tailings: m_tail = M − m_conc (never instantiated; it is the
    conservation remainder).

**Composition passes through unchanged**: the concentrate's element ratios
equal the ore's basket, x, exactly. The step raises *grade* (g → g_c), never
*selectivity*. Element separation is deferred entirely to Zone 3, which is
where it happens in reality too.

**Why each distribution was chosen:**

| Choice | Justification |
|---|---|
| Exponential arrivals | standard for an external, uncoordinated supply; gives bursty feed the concentrator must absorb |
| Uniform batch mass | "a truck-train of roughly known size" — simplest bounded mass |
| Truncated normal grade | bell around a deposit mean, bounded to physical range |
| **Dirichlet basket** | the natural distribution over *compositions*: non-negative, sums to 1, one knob (α_j) per element for the mean, one global knob (Σα = tightness) for geological variance; per-batch fluctuation without ever breaking Σx = 1 |
| Triangular recovery | bounded (no >100 % or negative recovery), expert-range shape (min/typ/max), no false tail precision |
| Triangular/Uniform times | same rationale for best/nominal/worst cycle times |

### 3.3 Parameters, provenance, and what they drive

| Parameter | Value | Real-world anchor | Confidence |
|---|---|---|---|
| λ⁻¹ (arrival mean) | 10 h | — (sets mine size) | design |
| M | U(80, 120) t | a mine-haulage dispatch of ~2–4 truckloads | design |
| μ_grade, σ_grade | 0.08, 0.01 on [0.05, 0.11] | Mountain Pass head grade 6–9 % REO | **[M]** |
| g_c (concentrate grade) | 0.55 | real concentrates 40–60 % REO (see §2.7 table) | **[H]** |
| r ~ Tri(0.72, 0.80, 0.88) | mode 0.80 | concentrator recoveries 70–85 % | **[M]** |
| α (basket) | (2.4, 5.0, 0.55, 1.7, 0.18) | bastnäsite pattern Ce > La > Nd ≫ Pr, Dy | **[M]** |
| T_c, T_f | U(1, 1.5) h; Tri(5, 7, 11) h | plant-level per-batch cycle, *not* pulp residence (§4) | design |
| Q_CONC capacity | 150 t | days of surge between mine pace and chemical plant pace | design |

**Implied steady state** (mean-value check): ore 10 t/h → concentrate
10 · 0.08 · 0.80 / 0.55 ≈ **1.16 t/h ≈ 28 t/day**. The Zone-2 stub consumes
20 t / 20 h = 1.0 t/h, so the buffer drifts up ~4 t/day and saturates in
roughly a month — exactly what the current SIMLIB run shows (`Q_CONC` first
exceeds 140 t at t ≈ 400 h, pins at 150 t by t ≈ 720 h, then backs up the
flotation output queue). Utilizations: crusher ≈ 12 %, flotation ≈ 70 % —
flotation is deliberately the tight server, as in reality.

**What the basket implies per 90 days** (cumulative captured REO ≈ 1,380 t):
La ≈ 337 t, Ce ≈ 703 t, Pr ≈ 77 t, Nd ≈ 239 t, Dy ≈ 25 t. For every ton of Nd,
the plant also produces ~2.9 t of Ce and ~1.4 t of La — the co-production
problem the assignment wants demonstrated, in numbers.

### 3.4 Deliberate distortions (known and intentional)

1. **Dy is ~100× over-represented.** Real bastnäsite Dy is trace (<0.1 %);
   the model's E[x_Dy] = 0.0183. Without it, the Dy leg of the chain (and its
   magnet-additive economics) would have no material flow at all.
2. **Nd is boosted ~1.5× at La's expense** (E[x_Nd] = 0.173 vs real ~0.11–0.13;
   E[x_La] = 0.244 vs real ~0.33). Keeps the magnet line fed with a 5-element
   basket.
3. **Basket variance is ~10–100× real.** With Σα = 9.83 the per-batch relative
   σ of shares is 30–60 % (majors) to >200 % (Dy). A real deposit's basket is
   nearly constant; the amplification exists so that "input concentrate
   composition fluctuation" is visible on a 90-day horizon without simulating
   years. To model a tighter geology, scale α up (mean unchanged, variance
   ~ 1/Σα).
4. **Scale is ~1/10 of a real plant** (240 t/day ore vs ~2,400 t/day at modern
   Mountain Pass **[M]**). Fine for the assignment; absolute tonnages are
   arbitrary anyway.

### 3.5 Semantics in the activity-scanning engine (port spec, no code)

Mapping onto `engine-design.md` vocabulary — four processors, in sweep order:

* **ORE_SRC** — timer at t + Δ. Fires: create a batch POD
  (`available_at = t + τ_pipe`, tons = M, `elem[8]` = M·g·x), enqueue on
  CRUSHER's queue, draw next Δ. Contributes one time to `next_event_time()`.
* **CRUSHER** — states STARVED / RUNNING / DONE_BLOCKED. Starts when its queue
  head is available (available_at ≤ t) and it is idle; RUNNING holds a
  completion timer (T_c). On completion, emits the (unchanged) batch to
  FLOTLINE's queue with available_at = t + τ_pipe.
* **FLOTLINE** — same, with completion time T_f. On completion, applies the
  mass map (equations 7–9) — **this is the only place composition and mass
  change** — and hands the concentrate batch to Q_CONC. If free space
  (capacity − level) < batch tons → DONE_BLOCKED: the finished batch is held
  inside; the quiescence sweep retries as Zone 2 frees space.
* **Q_CONC / consumer (Zone 2 or stub)** — ring buffer, capacity 150 t; the
  consumer takes FIFO batches with available_at ≤ t.

Details that must be pinned for bit-identical runs and to satisfy engine
invariants:

* **RNG draw order per batch** (fixed by constructor + sweep order):
  Δ_arrival → M → g → r → five Gamma(α_j) for the basket → T_c → T_f.
  One draw stream, single `std::mt19937_64`.
* **Every transfer gets τ > 0** (minutes of pipe/conveyor). The SIMLIB model
  has zero-delay handoffs between crusher → flotation → buffer; the new engine
  forbids them. Add a small constant or sampled τ per hop.
* **The SIMLIB `DigestStub` polls hourly** (`Wait(1.0)` starvation loop); in
  the engine this becomes a proper timer (recheck at each Q_CONC level change,
  or schedule "batch ready when level ≥ 20 t").
* **Blocking bookkeeping differs, behavior must not**: SIMLIB blocks the batch
  *inside* `Enter(QConc, …)` — it shows up in the store's queue length. The
  engine's DONE_BLOCKED holds it inside the processor. Same physics (upstream
  stops producing when the buffer is full), different counter; align the
  metric when comparing traces.
* **Element count**: engine batches carry `double elem[8]`; the Zone-1 model
  uses 5 (La, Ce, Pr, Nd, Dy). Pad or extend — decide once, keep the draw
  order stable.
* **Units**: Zone-1 times are hours; the engine clock is integer ms. 1 h =
  3.6e6 ms; 90 days ≈ 7.8e9 ms, comfortable in i64.
* **Validation target**: reproduce the distributions of the SIMLIB run
  (`ore_concentration.dat`) — per-batch grade/recovery/mass histograms, Q_CONC
  fill time (~t = 400–720 h), blocking onset, cumulative concentrate
  (~2,400 t / 90 d) — before adding new effects.

### 3.6 Upgrade options, by payoff

1. **Per-element recovery vector r_e** (cheap, high realism payoff): in
   reality recovery is mineral-driven; within one mineral the deltas are small
   for adjacent LREE but heavy REEs largely report to tailings. A vector
   (e.g. r_Dy ≈ 0.5·r_La) fixes the "Dy flows too easily" distortion.
2. **Grade–recovery trade-off**: make g_c a chosen setpoint with
   r = f(g_c) monotone decreasing (a concave curve through
   (0.40, 0.88), (0.55, 0.80), (0.70, 0.60), say). Lets experiments ask
   "what if we push concentrate grade" — currently impossible by construction.
3. **Autocorrelated grade** (AR(1) or two-state Markov mining faces): replaces
   iid head grades with geology-like drift; makes the *plant's* buffer levels
   respond to multi-day mine swings instead of per-batch noise.
4. **Failures**: MTBF ~ Exp, TTR ~ Lognormal on CRUSHER/FLOTLINE per
   `engine-design.md` (DOWN state, in-flight material held). This is where the
   assignment's "failure propagation both directions" story gets its upstream
   half.
5. **Reagent coupling**: flotation consumes collector/frother; tie FLOTLINE
   starts to CHEM_SUP stock, extending Zone-2's delay risk to Zone 1 (in
   reality the concentrator has its own reagent logistics).
6. **Middlings recycle loop**: split FLOTLINE into rougher (recovers r_rough ≈
   0.9) and cleaners (upgrade to g_c) with a recycle stream of cleaner tails
   back to rougher feed. Adds the internal feedback that makes a real circuit
   slow to re-equilibrate — but only do this if Zone-1 internal transients
   matter to an experiment; otherwise it's wasted state.

---

## 4. The ledger: what was simplified away

| # | In reality | In the model | Cost of the simplification |
|---|---|---|---|
| 1 | Continuous slurry flow, 24/7, no batches anywhere | discrete ~100 t batches through two servers | queue/blocking granularity is one batch; smooth rate effects (pipe pack-out, surge capacity) invisible. Acceptable: the plant's *state* changes are batch-shaped downstream anyway |
| 2 | 3-stage crushing + rod/ball milling + cyclones + desliming; liberation physics (P80, Bond energy) | one CRUSHER server, U(1, 1.5) h | no grind-size → recovery link; can't study "finer grind, better recovery" trade-offs |
| 3 | Rougher/scavenger/cleaner circuit with middlings recirculation and regrind | one FLOTLINE server, Tri(5,7,11) h | internal recycle folded into the duration distribution; a mid-circuit failure can't be modeled *inside* Zone 1 (it can in Zone 3, where the assignment actually needs it) |
| 4 | Flotation chemistry: collectors, depressants, pH, steam heating to ~60 °C | single random scalar r | no reagent consumption, no chemistry-driven downtime, no "hot plant vs cold plant" selectivity; selectivity is *constant by construction* |
| 5 | Grade–recovery curve (operator chooses a setpoint) | g_c = 0.55 fixed, r independent | can't ask what pushing grade to 70 % costs in recovery (upgrade #2) |
| 6 | Element-specific recovery (mineral-driven; HREE → tailings) | one r for all elements; basket passes through exactly | Dy/Pr flow overstated; the *first* place selectivity appears is Zone 3 — true in spirit, exaggerated in degree |
| 7 | Geology: basket near-constant per deposit, grade drifts autocorrelated | basket ~ Dirichlet iid per batch, grade ~ truncated normal iid | per-batch basket noise ≫ real; multi-day mine swings absent (upgrades #1, #3) |
| 8 | Tailings dam, process-water recirculation, thickeners | implicit sink (M − m_conc) | no tailings capacity limit, no water balance, no environmental constraints |
| 9 | By-products (barite at Mountain Pass, fluorite at Bayan Obo) and thorium in monazite | none | no by-product revenue stream; radioactivity only enters at Zone 2's waste sink |
| 10 | Mechanical failures (liners, mill motors, pumps), steam supply outage | none in Zone 1 (planned via DOWN state) | Zone-1 failures don't exist yet; the "outage spreads both ways" experiment currently can't start at the mine end (upgrade #4) |
| 11 | Mine: faces, blending, ROM pad dynamics; market-driven mining rate | Poisson source with fixed rate | can't shut the mine or ramp it; feed is exogenous and stationary |
| 12 | Reagent logistics for flotation (collector shipments) | none (CHEM_SUP couples only to Zone 2) | concentrator is unrealistically self-sufficient (upgrade #5) |

**The three structural simplifications, in words:**

1. **Continuous → discrete.** The real step is a steady-state chemical plant;
   the model is a queueing network. What survives: throughput ratios,
   utilization, blocking/starvation, and the mass balance. What dies: any
   dynamics faster than one batch (~hours).
2. **A circuit → a server.** The rougher/scavenger/cleaner loop with
   middlings recirculation is a *feedback system* whose state is the
   composition of ~hours of circulating load. The model compresses that state
   into one duration draw. This is honest *only because* Zone 1's internal
   equilibration is hours — the weeks-long equilibration the assignment
   cares about lives in the Zone-3 cascade, where the model deliberately does
   keep per-stage state.
3. **Chemistry → one scalar.** All selectivity knowledge (hot flotation,
   reagent choice, liberation) is compressed into Tri(0.72, 0.80, 0.88).
   Defensible because the *outputs* the downstream zones see — mass, grade,
   basket, timing — carry no chemistry anyway; recovery's distribution shape
   is the only place chemistry could leak through, and a triangular fit is as
   good as any without plant data.

**What is deliberately kept exact** (this is the point of the simplification):

* ratio preservation of the basket (the co-production constraint — the
  assignment's core narrative),
* mass conservation, per element, to the ton (the engine's checked invariant),
* the grade upgrade 8 % → 55 % (a ~7× mass reduction between mine and chemical
  plant, which sets every buffer's scale),
* stochastic feed structure: bursty arrivals, drifting grade, noisy basket,
* blocking against a finite intermediate store (the mechanism by which a
  slow Zone 2 throttles a fast Zone 1).

---

## 5. Sanity checks and validation

1. **Invariant**: for each element, tons in = tons out + storage + in-transit,
   checked every N sweeps (`engine-design.md`). Zone 1 satisfies it exactly by
   construction (equations 7–9 are the accounting).
2. **Rate check**: predicted concentrate 1.16 t/h vs measured cumulative
   2,392 t / 2,160 h = 1.11 t/h in the SIMLIB run (difference = early ramp +
   blocking) — consistent.
3. **Buffer fill**: predicted saturation of Q_CONC in ~4–6 weeks vs observed
   fill at t = 400–720 h — consistent.
4. **Scale check**: mass fraction ore → concentrate = g·r/g_c = 0.08·0.8/0.55
   ≈ 11.6 % vs real Mountain Pass ≈ 12.6 % (8.6 % × 0.85 / 0.60) — consistent.
5. **Port acceptance**: new engine reproduces the SIMLIB Zone-1 run
   (distributions + fill time + cumulative curve) before any new feature lands.

---

## 6. Sources

Repo-internal: `assignment.txt` (T8), `mermaid-explain.md` (with its linked
sources), `engine-design.md`, `ore_concentration.cc` (the model this document
formalizes), `research.md` (raw research brief; notes the same tool outage).

Standard literature (verification targets — live fetch was unavailable):

* Gupta, C.K. & Krishnamurthy, N., *Extractive Metallurgy of Rare Earths*,
  CRC Press, 2nd ed. 2016 — the canonical reference: Mountain Pass hot
  flotation flowsheet, Bayan Obo, monazite sands, grades and recoveries.
* Zhang, J. & Edwards, C., "A Review of Rare Earth Mineral Processing
  Technology", CMP Operators' Conf. 2012 — flowsheets + grades for Mountain
  Pass and Bayan Obo.
* Jordens, A. et al., reviews in *Minerals Engineering* (2013–2014) — REE
  flotation chemistry, mineral-specific recovery, gravity/magnetic routes.
* Castor, S.B., "The Mountain Pass rare-earth carbonatite…", *The Canadian
  Mineralogist* 46(4), 2008 — deposit geology, head grades, mineralogy.
* British Geological Survey, *Rare Earth Elements* mineral profile, 2011.
* MP Materials technical report summary / SEC filings; Lynas annual reports —
  modern Mountain Pass and Mount Weld figures.
* Iluka Resources, *Mineral Sands: An Overview of the Industry* — wet plant /
  dry mill flowsheets for the monazite route.
* Wills' *Mineral Processing Technology* (8th ed.) — comminution, flotation
  circuit design, residence-time practice.
* MDPI *Minerals* 14(9):906 (2024) — flotation of a bastnäsite/monazite ore
  (already cited in `mermaid-explain.md`).
