To build a clean, faithful discrete-event (**SHO**) model of the rare earth processing chain without drowning in low-level computational fluid dynamics, you only need a small set of **probability distributions**, **time/scheduling rules**, and **lightweight transfer/state functions**.

Below is a practical map of **which functions to use for which process**.

---

## 1. Probability Distributions (Stochastic Inputs, Times & Faults)

### Ore & Basket Composition (`ORE_SRC`, `PREP`)

* **Dirichlet distribution** for the element "basket" vector $\mathbf{x} = (x_{\text{La}}, x_{\text{Ce}}, x_{\text{Nd}}, x_{\text{Pr}}, x_{\text{Dy}}, \dots)$ with $\sum_j x_j = 1$. It naturally keeps percentages non-negative and summing to `1.0` while letting you tune geological variance via a concentration parameter $\boldsymbol{\alpha}$ (higher $\boldsymbol{\alpha}$ = tighter geological ratio).
* **Truncated Normal or Beta distribution** for ore grade (e.g., REO % in feed) and recovery efficiency in crushing/flotation/magnetic separation, avoiding impossible values like negative grade or $>100\%$ recovery.

### Operation & Residence Times (`DIGEST`, `PREC_*`, `REDUCT`, `CAST_MILL`, `PRESS_SINT`)

* **Triangular or PERT distribution** for batch/kiln/furnace cycle times when you know typical best, nominal, and worst-case durations from process specs (e.g., acid bake or sintering cycles).
* **Log-normal distribution** for operation or maintenance times with right-skewed tail delays (e.g., stubborn batch leaching, furnace heat-up/cool-down anomalies, or milling delays).

### Chemical Supply Delays (`CHEM_SUP`, `ADDITIVES`)

* **Exponential distribution** for time between unplanned supply disruptions or random delivery delays (memoryless arrival/delay intervals).
* **Log-normal or Gamma distribution** for the *duration* of a shipping/reagent delay once a delay occurs (keeps delay length positive and right-skewed).

### Stage Breakdowns & Repairs (`S1..SN`, servers)

* **Exponential distribution** for **Time-Between-Failures (TBF)** under a constant hazard rate $\lambda$ (especially useful when stressing a stage in the middle of the cascade).
* **Log-normal or Weibull distribution** for **Time-To-Repair (TTR)** or wear-out behavior across stages and furnaces.

---

## 2. Timing & Flow Functions (Discrete-Event / SHO Mechanics)

### Event Scheduling & Time Advance (Core Engine)

* **Next-Event / Priority Queue timing:** Advance simulation clock $t$ to the minimum timestamp $\min_k (t_{\text{next}, k})$ across batch completions, reagent arrivals, stage breakdowns, and repairs.
* **Fixed-step ($\Delta t$) or hybrid synchronization for the cascade:** Because a counter-current cascade of $N$ mixer-settlers acts like coupled stages with residence lag, either step coupled stage-transfer events at a small $\Delta t$ (e.g., hours/shifts) or schedule stage-to-stage transfer events conditioned on upstream/downstream buffer availability.

### Blocking, Starvation & Recycle Timing

* **Capacitated queue gating (`WIP < Cap`):** A processor cannot release output into downstream buffer $B$ if $\text{level}(B) \ge \text{cap}(B)$ (upstream blocking); it cannot start a new batch if required input buffer or reagent stock is below batch size (starvation).
* **Transport/lag delay function:** Add a deterministic or stochastic lag $\tau_{\text{lag}}$ for internal cascade residence and off-spec recycle loops (`RECYCLE_PURE` $\to$ `Q_PLS`, `SCRAP_LOOP` $\to$ `Q_PLS`).

---

## 3. Separation, State & Economic Functions

### Cascade Purity & Separation Function (`S1..SN`)

You do not need full equilibrium thermodynamics to capture the *system dynamics* of hundreds of stages:

* **Stage-wise separation factor ($\beta$) / mass-balance update:** At stage $i$, model how each element $j$ partitions between the forward aqueous stream and counter-current organic stream using a separation factor $\beta_{a,b}$ (around $1.3\text{--}2.0$ for adjacent lanthanides) and stage efficiency $E_i \in [0, 1]$ (where $E_i = 0$ if stage $i$ is down).
* **Why it works for your experiments:** Small $\beta$ close to $1$ forces many stages $N$ to reach target purity, creates multi-week propagation lag from cold start, and makes a failure at stage $k$ block/starve neighbors while shifting downstream purity.

### Routing, Yield & Accumulation Functions (`SPLIT`, `STORE_UNW`, `MAG_OUT`)

* **Proportional split + threshold routing:** Split incoming stream by basket fractions into target elements (`Nd`, `Pr`, `Dy`) and low-demand elements (`Ce`, `La`). If output purity $< \text{purity}_{\text{target}}$, route to `RECYCLE_PURE` instead of `PREC_ND`.
* **Yield loss multiplier:** Multiply batch output by a process yield factor $(1 - \text{loss}_{\text{rate}})$ at sintering/machining, routing scrap fraction to `SCRAP_LOOP` and waste fraction to sink.
* **Cumulative inventory & holding cost integral/sum:** Track warehouse stock $W_j(t)$ of unwanted elements over time $t$ and accumulate storage/disposal cost $\sum_j c_{\text{hold}, j} \cdot W_j(t) \cdot \Delta t$.

### Economic / Break-Even Function (Final Synthesis)

* **Cumulative cost vs. revenue balance:** Over simulation horizon $T$, sum capital/ramp-up amortization, operating/reagent/energy costs, and unwanted-element storage costs, then solve for the required selling price of target magnet/REE output needed to reach break-even given the demand mix.

---

## Quick Summary Table by Process

| Process / Zone | Timing & Engine | Key Distributions | Key State / Math Functions |
| --- | --- | --- | --- |
| **Ore & Prep (`ORE_SRC`, `PREP`)** | Batch/interval arrivals + capacity gating | Dirichlet (basket), Truncated Normal/Beta (grade/recovery) | Composition vector normalization & recovery scaling |
| **Acid Digestion (`DIGEST`)** | Batch completion events + reagent hold | Triangular/PERT (bake/leach time), Log-normal/Exp (reagent delays) | Stoichiometric reagent consumption & PLS volume/conc update |
| **Extraction Cascade (`S1..SN`)** | Coupled step/event transfers + lag | Exponential (TBF), Log-normal/Weibull (TTR) | Separation factor $\beta$, stage efficiency $E_i$, bidirectional blocking/starvation |
| **Precip, Split & Reduction (`Z4`)** | Parallel batch servers + purity check | Triangular/Log-normal (furnace/precip cycles) | Basket split, purity threshold routing, `Ce/La` warehouse accumulation |
| **Magnet Sintering (`Z5`)** | Discrete lot routing + scrap loop | Triangular/Log-normal (milling/sintering cycles) | Yield loss factor, scrap feedback, cumulative break-even cost |

If you tell me which language you plan to write your tool or DSL in (e.g., Python, Rust, TypeScript), I can sketch the core data structures and step/event loop around these exact functions.
