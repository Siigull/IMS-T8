# Rare Earth Element (REE) Processing Chain: Simulation Reference

This document defines the processor types, state variables, failure modes, and reference sources used in the discrete-event simulation (**SHO**) of the Rare Earth Element (REE) processing chain (Task T8).

---

## 1. Sources & Sinks (Boundary Nodes)

### Stochastic Ore Source (`ORE_SRC`)

* **What it is:** Generates raw mineral batches (e.g., bastnäsite or monazite) with a geologically fixed "basket" ratio of lanthanides plus random variance.
* **Key state / parameters:** Element composition vector (`La, Ce, Nd, Pr, Dy, ...`), ore grade, batch arrival interval, geological variance distribution.
* **Simulation behavior:** Emits batches that force the plant to process unwanted elements alongside high-demand ones.

### Chemical & Reagent Supply (`CHEM_SUP`)

* **What it is:** Supplies sulfuric/hydrochloric acid, water, alkalis, and organic extractants/diluents.
* **Key state / parameters:** Delivery lead time, delay probability/distribution, reagent availability.
* **Simulation behavior:** Can delay or block batch processors when deliveries slip.

### Magnet Alloy Additives (`ADDITIVES`)

* **What it is:** Supplies Iron (`Fe`), Boron (`B`), and heavy REE additives (such as `Dy`) for magnet alloy formulation.
* **Key state / parameters:** Additive availability, lot size.

### Sinks & Warehouses (`WASTE_TH`, `STORE_UNW`, `MAG_OUT`)

* **What it is:** Terminal destinations for waste/gangue/thorium (`WASTE_TH`), accumulating low-demand oxides such as `Ce`/`La` (`STORE_UNW`), and finished NdFeB magnets (`MAG_OUT`).
* **Key state / parameters:** Stored mass/volume, holding cost per unit time, cumulative output count.

---

## 2. State Buffers / WIP Queues

Buffers sit between processors to hold Work-in-Progress (WIP), track material vectors/purities, and enforce upstream blocking when full:

* **`Q_CONC` (Mineral Concentrate WIP):** Holds crushed/beneficiated concentrate (~50–60% REO) with element composition state.
* **`Q_PLS` (Pregnant Leach Solution WIP):** Holds dissolved rare earth ions ($RE^{3+}$) after digestion and impurity removal.
* **`Q_INGOT` (Nd Metal Ingot WIP):** Holds reduced rare earth metal ingots feeding the magnet line.

---

## 3. Processor Types & Reference Sources

### A. Ore Concentration / Beneficiation Processor (`PREP`)

* **What it is:** Crushes, mills, and concentrates raw ore via gravity, magnetic separation, and froth flotation to separate REE-bearing minerals (bastnäsite, monazite) from gangue.
* **Simulation role:** Transforms low-grade raw ore into higher-grade mineral concentrate while preserving the geological basket ratio.
* **Sources:**
* [MDPI Minerals — Flotation of an Ore Containing Bastnäsite and Monazite](https://www.mdpi.com/2075-163x/14/9/906)
* [ResearchGate — Flotation-Magnetic Separation for the Beneficiation of Rare Earth Ores](https://www.researchgate.net/publication/323798206_Flotation-magnetic_separation_for_the_beneficiation_of_rare_earth_ores)
* [JXSC Machine — Beneficiation Methods of Rare Earth Minerals](https://www.jxscmachine.com/new/beneficiation-method-rare-earth-minerals/)



### B. Acid Digestion / Leaching Batch Processor (`DIGEST`)

* **What it is:** Reacts mineral concentrate with acid (such as sulfuric acid baking/leaching) and performs impurity/pH conditioning to produce a pregnant leach solution (PLS).
* **Simulation role:** Batch processor consuming concentrate and chemical reagents over a residence time, subject to reagent supply delays and downtime.
* **Sources:**
* [Google Patents (US20170204499A1) — Acid Leaching of Rare Earth Minerals](https://patents.google.com/patent/US20170204499A1/ko)
* [Ingetecsa — Rare Earth Acid Mixing & Acid Bake Preparation](https://www.ingetecsa.com/landings/acid-mixing/)



### C. Coupled Mixer-Settler Cascade Stage (`S1 ... SMID ... SN`)

* **What it is:** A multi-stage counter-current solvent extraction unit where each mixer-settler contacts an aqueous phase with an organic extractant phase and separates them by gravity across dozens or hundreds of stages.
* **Simulation role:** Coupled bi-directional queue/stage network (`aqueous` forward $i \to i+1$, `organic` counter-flow $i+1 \to i$) with gradual purity shift, long equilibration/residence lag, off-spec recycle (`RECYCLE_PURE`), and bidirectional blocking/starvation when a stage fails.
* **Sources:**
* [ResearchGate — Calculation, Design and Manufacture of System Box-Type Mixer-Settler Used in Rare Earth Solvent Extraction](https://www.researchgate.net/publication/375896787_Calculation_design_and_manufacture_of_system_box-type_mixer-settler_used_in_rare_earth_solvent_extraction_process_at_laboratory_scale)
* [White Rose e-Theses — Liquid-Liquid Extraction of Rare Earth Elements From Sulfuric Acid](https://etheses.whiterose.ac.uk/id/eprint/638/1/uk_bl_ethos_432307.pdf)



### D. Stream Split, Precipitation, Calcination & Reduction Processors (`SPLIT`, `PREC_UNW`, `PREC_ND`, `REDUCT`)

* **What it is:** Splits separated streams into low-demand elements (e.g., `Ce`/`La`) and target elements (e.g., `Nd`/`Pr`), precipitates oxalates/carbonates, calcines them into oxides, and reduces target oxides into metal ingots (electrolytic or metallothermic reduction).
* **Simulation role:** Routing split + parallel batch processors; diverts unwanted oxides to growing inventory (`STORE_UNW`) while converting target streams into metal ingots (`Q_INGOT`).
* **Sources:**
* [ResearchGate — Recovering Rare Earth Oxide from NdFeB Magnet by Leaching and Selective Precipitation](https://www.researchgate.net/publication/324777260_RECOVERING_RARE_EARTH_OXIDE_FROM_NdFeB_MAGNET_OF_WASTE_HDDs_BY_LEACHING_AND_SELECTIVE_PRECIPITATION_METHODS)
* [Google Patents (US20170204499A1) — Precipitation, Calcination & Refining Flowsheet](https://patents.google.com/patent/US20170204499A1/ko)



### E. Discrete Magnet Manufacturing Line (`CAST_MILL`, `PRESS_SINT`, `SCRAP_LOOP`)

* **What it is:** Converts metal ingots and additives into sintered NdFeB magnets via alloy melting/strip casting, hydrogen decrepitation/jet milling, magnetic alignment pressing, vacuum sintering, finishing, and swarf/scrap recycling.
* **Simulation role:** Discrete manufacturing processors with lot/batch cycle times, yield loss, and a scrap/swarf feedback loop (`SCRAP_LOOP`) back toward reprocessing.
