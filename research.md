# Research: Rare-Earth Ore Beneficiation (Concentration) for a Discrete-Event Simulation

> **TOOL FAILURE NOTICE (read first):** All `web_search` calls failed with HTTP 402 "Add usage credits to use web search"
> (ref IDs: 55628419…, 55e8250a…, ba2f25ca…, 66450cae…, ed2eb620…). No live web sources could be fetched or verified.
> **Everything below is from the model's training knowledge and must be treated as UNVERIFIED.** Each numeric claim is
> tagged with a confidence marker: **[H]** = widely reported in standard literature, likely robust;
> **[M]** = recalled but numbers may be off by ±20–50%; **[L]** = weak recall, verify before use.
> Recommended primary sources to verify against are listed in *Sources*.

## Summary
Rare-earth concentration is essentially always a *mineral* separation, not an element separation: bastnäsite ores (Mountain Pass, Bayan Obo, Maoniuping) follow crush → rod/ball mill grind to roughly 50–105 µm P80 → steam-heated (~60 °C) fatty-acid/hydroxamate rougher–cleaner–scavenger flotation to a ~50–60 % REO concentrate; monazite-bearing heavy-mineral sands use wet gravity spiral plants followed by dry magnetic/electrostatic/gravity separation. Typical head grades are 3–9 % REO, concentrate grades 40–60 % REO, and plant recoveries ~60–90 % depending on ore and era. Recovery is driven by mineral (bastnäsite vs. monazite vs. fine intergrowths), with only modest element-by-element selectivity within a given mineral — so a DES model should key mass splits to minerals, not to individual lanthanides.

---

## Findings

### 1. Unit operations sequence for bastnäsite ores (Mountain Pass archetype)

1. **Ore and gangue mineralogy** — Mountain Pass (California, USA) is a carbonatite; the ore mineral is bastnäsite
   ((Ce,La,Nd)CO3F, ~75 % REO as mineral) with minor monazite/parasite/synchysite; gangue is dominated by
   calcite (~40–50 %), barite (~10–20 %), dolomite/ankerite, with minor quartz, biotite/phlogopite. The
   calcite + barite gangue is the key flotation problem (all three are floatable with fatty acids).
   Historical run-of-mine head grade ~7–9 % REO (avg ~8.6 % often quoted). **[H]** [Castor 2008; BGS 2011]
2. **Comminution sequence (typical hard-rock REE flowsheet)** —
   ROM ore → **primary gyratory crusher** (product ~150–250 mm) → coarse stockpile (DES: buffer with reclaim) →
   **secondary cone crushing** → **tertiary cone crushing in closed circuit with vibrating screens** to
   ~10–19 mm (≈ ½–¾ in) crusher product → fine ore bins → grinding. **[H]** [Gupta & Krishnamurthy 2005/2016]
3. **Grinding** — **rod mill (open circuit) + ball mill in closed circuit with hydrocyclones**; flotation feed
   typically ~60–90 % passing 74 µm, i.e., **P80 ≈ 45–105 µm**; many descriptions of Mountain Pass quote roughly
   "70 % – 200 mesh". Some bastnäsite circuits include a **desliming cyclone step** (~10–20 µm cut) ahead of
   conditioning because slimes consume collector and depress selectivity. **[M]** [Gupta & Krishnamurthy; Zhang & Edwards 2012]
4. **Hot (steam) flotation — the Mountain Pass signature** — pulp is thickened to ~45–55 % solids and
   **conditioned in agitated tanks with direct steam injection to ~60 °C (140 °F)**; the elevated temperature
   is maintained through rougher flotation (and cleaners in some descriptions) and is credited with sharply
   improving bastnäsite/calcite selectivity (temperature range cited 55–80 °C; ~60 °C is the canonical figure).
   **[H]** [Gupta & Krishnamurthy 2005/2016; Zhang & Edwards 2012]
5. **Reagent suite** — collector: **fatty acids** (oleic acid / tall oil) ~0.3–1.0 kg/t, or **alkyl hydroxamates**
   (e.g., C7–C9 hydroxamic acids, more selective but slower/dearer); depressants: **sodium silicate ("water
   glass")** for silicates/barite plus **starch, quebracho (tannin), or lignin sulfonate** for calcite;
   pH ~8–9.5 adjusted with soda ash; sodium fluoride/fluorosilicate occasionally reported as modifiers.
   MP Materials holds modern patents refining the hot-flotation reagent regime. **[M]** [Gupta & Krishnamurthy; Jordens et al. 2013; MP Materials patent literature]
6. **Circuit structure (historical Mountain Pass)** — conditioner (steam) → **rougher bank → scavenger bank** on
   rougher tails; rougher concentrate re-cleaned **2–3 times** (cleaner/recleaner); cleaner tails and scavenger
   concentrate (middlings) recirculated to the conditioner/rougher feed (regrind of middlings optional).
   A separate **barite byproduct flotation circuit** existed historically (barite sold for drilling mud).
   Final concentrate thickened, filtered, dried, and shipped as ~60 % REO concentrate. **[M]** [Gupta & Krishnamurthy 2005/2016; Zhang & Edwards 2012]

### 2. Monazite from heavy-mineral sands; Bayan Obo flowsheet

7. **Heavy-mineral sand (HMS) wet plant** — unconsolidated strandline sand with **2–10 % heavy minerals (HM)**
   (typical "ore" 3–6 % HM). Sequence: mining (dredge or dozer/truck) → **trash screen** (~1–2 mm cut) →
   **desliming cyclones** (–38/45 µm to pond) → **spiral gravity circuit in rougher → cleaner → recleaner
   stages** (Humphrey's/Reichert spirals or cones) producing a **heavy-mineral concentrate (HMC) at ~90–97 % HM
   with ~85–95 % HM recovery**. Individual spiral starts handle ~1–4 t/h each (DES building block). **[H]** [Iluka "Mineral Sands: An Overview of the Industry"; Jordens et al. 2013]
8. **HMS dry separation plant (mineral splitting)** — HMC dried → **low-intensity magnetic separation (LIMS)**
   removes magnetite → **high-tension roll (HTR) electrostatic** splits conductors (ilmenite, rutile,
   leucoxene) from non-conductors → **rare-earth roll / induced-roll / WHIMS magnetic stages** further split
   paramagnetic species → **wet shaking tables (e.g., Wilfley)** and additional WHIMS polish. **Monazite is
   the paramagnetic, high-SG (~5) phosphate**: it reports to magnetic middling streams and is recovered as a
   byproduct (often <1 % of HMC); many Western plants reject it to tailings for radioactivity (Th) reasons,
   while Indian (IREL) and Brazilian operations historically produced it. **Monazite concentrate ≈ 90–98 %
   monazite mineral → ~55–60 % REO + 4–10 % ThO2** (the mineral itself assays ~60–70 % REO+Th). **[H]** [Iluka overview; Gupta & Krishnamurthy]
9. **Bayan Obo (Baotou, Inner Mongolia, China)** — Fe–REE–Nb carbonatite-related deposit; **ore ~35 % Fe and
   ~5–6 % REO** (mined primarily for iron). REE minerals are fine-grained **bastnäsite + monazite** intergrown
   with magnetite/hematite, fluorite, aegirine, dolomite. Flowsheet at the Baotou concentrators: crush →
   grind (~85–90 % –74 µm) → **weak-intensity magnetic separation (magnetite)** → iron recovery
   (magnetite/hematite) → **REE flotation from the iron tailings** (tailings re-ground finer, often quoted
   ~95 % –44 µm for liberation) → REE rougher/cleaner flotation using historically **oxidized paraffin soap /
   naphthenic acid collectors**, later **hydroxamic-acid-type collectors (e.g., "H205" class)** with sodium
   silicate and carbonate pH control → **REE concentrate ~50 % REO** (historical product specs ranged 30–50 %).
   Fluorite is a further potential byproduct; Nb is largely lost. **[M]** [Zhang & Edwards 2012; Gupta & Krishnamurthy]
10. **Bayan Obo recovery reputation** — because iron drives the plant, **overall REE recovery from the deposit
    was historically very low, commonly cited as <10–15 %**, with the majority of REE reporting to the huge
    Baotou tailings facility (a well-known future REE resource). REE-flotation-stage recoveries of ~50–75 %
    on the treated tailings are reported; overall figures improved in the 2010s. **[M]** [Zhang & Edwards 2012; academic Bayan Obo literature]
11. *(Context)* Other bastnäsite ores: **Maoniuping (Sichuan)** uses simpler gravity+flotation on weathered ore
    with hydroxamate collectors; **Mount Weld (Australia)** is a supergene-enriched carbonatite whose REE host
    is secondary **monazite**, concentrated by crush → SAG/ball grind → rougher/cleaner flotation
    (fatty-acid/hydroxamate + sodium silicate). **[M]** [Lynas disclosures; Gupta & Krishnamurthy]

### 3. Head grades, concentrate grades, recoveries (real plants)

| Plant (era) | Head grade (% REO in ROM) | Concentrate grade (% REO) | REE recovery (concentrator) | Notes | Conf. |
|---|---|---|---|---|---|
| Mountain Pass, historical (Molycorp/Unocal/Chevron, 1950s–2002) | ~7–9 (avg ~8.6 often quoted) | ~60 (55–62) | ~85–90 commonly cited; some later-year reports 60–75 | Carbonatite bastnäsite; hot flotation | **[M]** |
| Mountain Pass, modern (MP Materials, 2017–now) | ~6–7 (mining zones ~6.5) | ~60 (sold as ~60 % REO concentrate) | ~70–85 (ramping/improved; company claims stage-wise improvements) | ~40–45 kt REO/yr in concentrate; ~1 Mt/yr ore | **[M/L]** |
| Bayan Obo / Baotou (China) | 3.5–6 (main orebody ~5) | ~50 (product grades 30–50) | Flotation-stage ~50–75; **deposit-wide <10–15 % historically** | Fe-driven plant; REE from iron tailings | **[M]** |
| Mount Weld (Lynas, Australia) | 8–15 (avg ~9–10; among world's highest) | ~40 (38–42) | ~80–90 (concentrator) | Secondary monazite in laterite; concentrate shipped to cracking plant | **[M]** |
| Generic HMS monazite byproduct | 2–10 % HM in sand; monazite <1 % of HMC | ~55–60 % REO (near-pure monazite) | 50–90 % of contained monazite where recovered; often deliberately rejected | Th-bearing; regulatory drag | **[M]** |

Sources for these rows (all unfetched, from knowledge): [Castor 2008; BGS 2011; Gupta & Krishnamurthy 2005/2016;
Zhang & Edwards 2012; MP Materials SEC filings/S-K 1300 technical report summary; Lynas annual reports; USGS
Mineral Commodity Summaries]. Mass-balance sanity check for a DES: concentrate mass fraction = (head % ×
recovery) / concentrate % — e.g., Mountain Pass 8.6 % × 0.88 / 0.60 ≈ **12.6 wt % of ore reports to concentrate**.

### 4. Element-uniformity of flotation recovery (LREE vs HREE)

12. **Recovery is mineral-driven, not element-driven** — flotation collects mineral particles; within a single
    bastnäsite or monazite particle the lanthanide distribution is fixed by mineral stoichiometry, so
    *elemental* recoveries differ only to the extent that (a) different REE minerals are present and (b) small
    element-specific surface-chemistry effects exist. **[H]** [Gupta & Krishnamurthy; Jordens et al. 2013]
13. **LREE vs HREE divergence is real and large across minerals** — bastnäsite and monazite are Ce-group
    (light) REE minerals: typical Mountain Pass concentrate pattern ≈ CeO2 ~50 %, La2O3 ~33 %, Pr6O11 ~4 %,
    Nd2O3 ~11–13 %, Sm2O3 ~1 %, Eu2O3 ~0.1 %, Gd2O3 ~0.2–0.5 %, Y2O3 <0.1 %. HREE carriers (xenotime,
    churchite, synchysite/parisite fractions) largely report to tailings in LREE circuits, so **Y/Dy/Tb
    recoveries at bastnäsite plants are effectively much lower than LREE recoveries**. Mount Weld's monazite
    carries somewhat more middle/HREE but the same principle holds. **[M]** [BGS 2011; Gupta & Krishnamurthy]
14. **Element-specific (within-mineral) effects are small but measurable** — bench studies (collector-type and
    pH series) report a few percent spread in La/Ce/Pr/Nd recovery within a given mineral, and plant data show
    the bastnäsite/monazite split matters (e.g., Ce:La ratios in concentrate differ from ore because the two
    minerals float at different rates in fine-grained ores like Bayan Obo). Historical note: Mountain Pass
    was for decades the world's main **europium** source (Eu2O3 ~0.1 % in concentrate), i.e., Eu recovery
    tracked bastnäsite recovery essentially uniformly. **[M]** [Jordens et al. 2013 and related McGill/Minerals Engineering literature]
15. **DES recommendation** — key recovery to *minerals* (e.g., bastnäsite 85–90 %, monazite 60–75 % in a
    bastnäsite circuit, parisite/synchysite and slimes-hosted REE ~0–50 %), then assign element assays per
    mineral. Apply element-specific deltas only if you want second-order realism (±2–5 % between individual
    LREE). **[M — modeling judgment]**

### 5. Residence times, throughputs, circuit structure

16. **Typical residence times (REE concentrator practice, mechanical cells)** —
    - Conditioning (reagent + steam, agitated tanks, often 2–4 in series): **5–20 min**
    - Rougher bank: **10–30 min** (bastnäsite is comparatively slow-floating → long banks, e.g., 8–14 cells of 5–40 m³)
    - Cleaner stages (2–3 in series): **8–15 min each**
    - Scavenger bank: **10–20 min**
    - Middlings regrind (ball mill + cyclone, P80 ~20–35 µm) if present: mill residence **10–20 min**
    - Concentrate thickener: **2–6 h**; pressure/vacuum filter: **0.5–2 h per batch/cycle**
    - Total pulp-to-concentrate circuit time including recirculation: **~1–3 h** typical.
    These are industry-typical design values, not plant-verified. **[M]** [mineral-processing design texts (e.g., Wills' Mineral Processing Technology); Gupta & Krishnamurthy]
17. **Throughputs** — Mountain Pass historical: ~500–1,200 tpd ore (consistent with ~20 kt REO/yr at ~8.6 % head
    and 85–90 % recovery). Mountain Pass modern (MP Materials): **~0.9–1.0 Mt/yr ore ≈ 2,400–2,900 tpd**,
    producing ~40–45 kt REO in concentrate (2022 ≈ 42.5 kt; 2024 ≈ 45.5 kt). Mount Weld concentrator:
    ~300–450 kt/yr ore scale (≈1,000–1,300 tpd), concentrate on the order of 100+ kt/yr. Bayan Obo/Baotou is
    iron-plant scale (tens of Mt/yr mined ore) with the REE flotation plants processing large iron-tailings
    streams. **[M/L]** [USGS MCS; MP Materials filings; Lynas filings]
18. **Canonical rougher–cleaner–scavenger topology for the DES model** (matches Mountain Pass descriptions and
    standard REE practice) —
    - Conditioner(s) → **rougher bank**; rougher tails → **scavenger bank**; scavenger tails → tailings
      thickener/TSF (final sink).
    - Rougher concentrate (+ scavenger concentrate) → **cleaner 1 → cleaner 2 → (recleaner)**; cleaner tails
      and scavenger concentrate = **middlings recirculated** either directly to conditioner/rougher feed or via
      a **middlings regrind** loop.
    - Cleaner/recleaner concentrate → concentrate thickener → filter → dryer → concentrate storage (final
      product), tailings stream to TSF, process-water recirculation loops from thickeners.
    - Typical internal recirculation magnitudes for modeling: cleaner-1 tails ≈ 30–60 % of cleaner-1 feed
      mass; scavenger concentrate ≈ 5–15 % of rougher tonnage (directional only, **[L]**). Barite byproduct
      flotation was a parallel circuit at historical Mountain Pass.
    - Upstream buffers for the DES: ROM stockpile, coarse-ore stockpile (hours–days), fine-ore bins
      (minutes–hours), thickeners (hours). **[M]**

---

## Sources

**No live sources could be fetched — web search returned HTTP 402 (out of credits) on every call, including
retries.** The following are the primary sources this knowledge is attributed to and the recommended
verification targets (titles/publishers recalled from knowledge; exact URLs/DOIs not verified):

- Kept (as verification targets, unfetched):
  - Gupta, C.K. & Krishnamurthy, N., *Extractive Metallurgy of Rare Earths*, CRC Press (1st ed. 2005, 2nd ed. 2016) — the standard reference for Mountain Pass hot flotation, Bayan Obo, and monazite-sands flowsheets and grades.
  - Zhang, J. & Edwards, C., "A Review of Rare Earth Mineral Processing Technology," 44th Annual Canadian Mineral Processors Operators' Conference, Ottawa (2012) — flowsheet diagrams and grades for Mountain Pass and Bayan Obo.
  - Jordens, A., Sheridan, R.S., Rowson, N.A. & Waters, K.E., "Processing a rare earth element deposit using gravity and magnetic separation," *Minerals Engineering* (2013) and related Jordens/McGill reviews on REE flotation chemistry — mineral-specific recovery behavior.
  - Castor, S.B., "The Mountain Pass rare-earth carbonatite and associated ultrapotassic rocks, California," *The Canadian Mineralogist* 46(4) (2008) — Mountain Pass ore grades/mineralogy.
  - British Geological Survey, "Rare Earth Elements" Mineral Profile (2011) — deposit head grades, production history.
  - MP Materials — S-K 1300 Technical Report Summary and Form 10-K/annual reports (SEC EDGAR) — modern Mountain Pass grades, concentrate grade, production tonnages, process description.
  - Lynas Rare Earths — annual reports / Mt Weld factsheets — ore grade (~8–10 % REO), ~40 % REO concentrate, plant scale.
  - Iluka Resources, "Mineral Sands: An Overview of the Industry" (widely circulated PDF) — wet plant/dry mill flowsheets, HM grades, spiral capacities.
  - USGS, *Mineral Commodity Summaries* (rare earths chapter, various years) — production tonnages for mass-balance checks.
- Dropped: none — no search results were returned at all due to the 402 credits failure (no SEO-spam filtering was possible or needed).

## Gaps

- **Everything numeric is unverified** due to the search-tool failure; the highest-priority verifications are:
  (1) Mountain Pass historical recovery (85–90 % vs. 60–75 % claims) and exact grind P80; (2) modern MP Materials
  concentrate grade and flotation recovery from their S-K 1300 report; (3) Mount Weld concentrate grade/recovery
  from Lynas disclosures; (4) Bayan Obo REE concentrate grade (~50 %) and deposit-wide recovery (<10–15 %)
  from Chinese-language or review literature; (5) exact Mountain Pass reagent suite (fatty acid + sodium
  silicate + quebracho/lignin sulfonate vs. other depressants) and conditioning temperature/steam details.
- Element-specific recovery deltas within a mineral (finding 14) are the weakest-sourced item — seek a recent
  *Minerals Engineering* / *Minerals Engineering Reviews* paper presenting per-element recovery tables for
  bastnäsite flotation.
- No plant-level equipment lists (cell counts, volumes, mill dimensions) could be cited with confidence —
  needed for a detailed DES; MP Materials' technical report and Zhang & Edwards (2012) are the best bets.
- Bayan Obo is treated as a black box by most English-language sources; Chinese journal papers (translated)
  would be needed for its true stage counts, reagent doses, and residence times.
- Suggested next step: re-run this research once web-search credits are restored, prioritizing the five
  verification items above.

## Supervisor coordination

No supervisor contact was needed: the assigned task (research + write `research.md`) completed, with the caveat
that the search tool failed with a 402 credits error, so the brief is knowledge-based and clearly marked
unverified throughout. No decision from the supervisor is required to accept or reject this deliverable, but
they should be aware of the tool failure and the *Gaps* section before treating any figure here as fact.
