# Pricing Engine

A C++ library with python interface which, starting from QuantLib's local volatility framework, introduces a robust calibration engine under the Buehler discrete-dividend model, as well as fast Monte Carlo path generation and a nonparametric approach to simulate local stochastic volatility (LSV) paths.

**References:**
Hans Buehler, *[Volatility and Dividends — Volatility Modelling with Cash Dividends and Simple Credit Risk](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1141877)*  
Anthonie W. van der Stoep, Lech A. Grzelak & Cornelis W. Oosterlee, *[The Heston Stochastic-Local Volatility Model: Efficient Monte Carlo Simulation](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2278122)*

You need a **pre-built QuantLib**.

---



## Why Buehler and not a flat dividend yield

Classical equity models that replace dividends with a continuous yield are a poor fit for single names: the market pays discrete cash dividends on an ex-date calendar, often together with proportional adjustments. Buehler’s paper derives the no-arbitrage spot dynamics compatible with that schedule. Spot splits into a forward/dividend floor plus a martingale **X**:

**S = (F − D) X + D**

with **X(0) = 1**, **F** the forward and **D** the discounted value of future cash dividends (the “floor”). Volatility is modeled and the smile is calibrated on **X**. Derivatives prices are generally computed simulating Monte Carlo paths in X and mapping them to S. However, for simple products, the price in S is directly linked to the one in X after an affine transformation of the contractual parameters is performed.

---



## Models

Both dynamics live on the Buehler **pure coordinate X** (see above). The implementation offers two layers on top of the same calibrated smile.

**Local volatility** is how the engine reproduces today's market. You load a Black implied-vol grid in **S**; preprocessing maps it to implied vol in **X**, smooths it into a bicubic surface, and runs Dupire on a dense (T, k_x) grid to obtain a fixed local vol σ_LV(t, x).

**Local stochastic volatility**: after LV is fixed a one-factor Bergomi-style driver is added on **X**. The simulator uses the bins technique explained in [van der Stoep, Grzelak & Oosterlee, 2014](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2278122). This project does not focus on the estimation of the stochastic volatility parameters, that could be a natural extension.

---



## How it works

End-to-end flow:

```
MarketData  →  preprocessing  →  calibration (σ_X, σ_LV)
           →  simulateFixingPaths (LV or LSV) →  product pricers
```

1. `MarketData` — spot, risk-free and repo curves, dividend schedule, implied-vol grid in **S** (cleaned mids; no bid–ask repair here). **Python / JSON:** `PricingContext.from_tables(**market)` → `MarketData::loadFromTables()`. **C++ showcase:** `loadSampleMarketSnapshot()` (hardcoded sample) or `loadConstantMock()` (flat BS regression).
2. `BuehlerModel::preprocessing()` — business-day grid, forwards F(0,T), dividend floor D(T).
3. `calibration()` — nodal σ_X → bicubic surface → Dupire σ_LV on a dense grid; `check_static_arbitrage` samples the bicubic σ_X for butterfly and calendar violations.
4. `simulateFixingPaths` — builds a `BuehlerFixingSavePath` under LV (QuantLib `PathGenerator` or fast tabulated σ_LV) or LSV. OpenMP can parallelise the fast LV and LSV evolve (`BUEHLER_MC_OPENMP`).

**Pricing.** Under LV, Europeans and digitals can also be priced by FD on the pure-**X** grid after a strike transform (`LvEuropean`*, `LvDigital*`; Python: `price_european_fd` / `price_digital_fd`). The JSON book / `price_all` path is Monte Carlo: Europeans, digitals, digital accrual, Asian, barrier, lookback, and autocall evaluate payoffs on a `BuehlerFixingSavePath` via `priceFromSavePath`.


| Layer  | Types                                                                                                | Role                                                      |
| ------ | ---------------------------------------------------------------------------------------------------- | --------------------------------------------------------- |
| Market | `MarketData`                                                                                         | Curves, spot, vol grid, dividends                         |
| Model  | `BuehlerModel`                                                                                       | Preprocess, calibrate, LSV, `mapXtoS`, path bank          |
| FD     | `LvEuropean*`, `LvDigital*`                                                                          | Finite Difference on **X** under LV                       |
| MC     | `EuropeanMc*`, `DigitalMc*`, `DigitalAccrualMc*`, `AsianMc`, `BarrierMc`, `LookbackMc`, `AutocallMc` | Payoffs on `BuehlerFixingSavePath` (LV or LSV)            |
| Checks | `benchmark.h`, `verify_fit.h`                                                                        | Smile verify, σ_X arbitrages, LSV vs LV, regression tests |


---



## Market data (JSON)

Recommended workflow: load market and book from JSON in Python, then call the C++ engine through `pricing_engine`.


| File                        | Purpose                                                                                                                                       |
| --------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `data/market_snapshot.json` | `asof`, `spot`, rates (`rfr_*`, `repo_*`), vol surface (`vol_tenor_years`, `vol_strikes`, `implied_vols`), dividends, optional Bergomi params |
| `data/options_book.json`    | option specs for Monte Carlo pricing (`options` list)                                                                                         |


```python
import json, sys
sys.path.insert(0, "build-std")
import pricing_engine as pe

market = json.load(open("data/market_snapshot.json"))
book = json.load(open("data/options_book.json"))["options"]

ctx = pe.PricingContext.from_tables(**market)
```

`PricingContext.from_tables(...)` maps the JSON fields to internal `MarketDataTables` and builds the QuantLib objects in memory.

---



## Validation

Main checks available in the codebase:

- `verify_lv_bs_consistency`: LV smile-fit consistency on market pillars.
- `verify_lsv_mc_vs_lv_fd`: LSV vs LV consistency check on a shared grid.
- `check_static_arbitrage`: butterfly and calendar checks on implied vol in X.
- `benchmark_*` routines in `src/core`: performance and regression checks for path generation/pricing flows.

---



## Build and run

Prerequisite: a pre-built QuantLib tree and `QUANTLIB_ROOT` pointing to the parent directory that contains `QuantLib/`. CMake expects

`QuantLib/build/<QUANTLIB_BUILD_PRESET>/ql/libQuantLib.dylib`

with default preset `apple-arm64-noxad-ninja-release` (override with `-DQUANTLIB_BUILD_PRESET=...`).

```bash
export QUANTLIB_ROOT=/path/to/quantlib-parent
cmake -S . -B build-std
cmake --build build-std
./build-std/pricing_engine
```

---



## Python

The engine is exposed via pybind11 as module `pricing_engine`, and this is the main interface for JSON-driven calibration/simulation/pricing.

- JSON schema reference: `[data/pricing_json_schema.yaml](data/pricing_json_schema.yaml)`
- Notebook examples: `[python/notebooks/pricing_from_json.ipynb](python/notebooks/pricing_from_json.ipynb)`, `[python/notebooks/test_fit.ipynb](python/notebooks/test_fit.ipynb)`

Build the module:

```bash
cmake -S . -B build-std -DBUILD_PYTHON_MODULE=ON
cmake --build build-std --target pricing_engine_py
```

Minimal usage (the `.so` is under `build-std/`; add it to `sys.path` as in the notebooks):

```python
import json, sys
sys.path.insert(0, "build-std")
import pricing_engine as pe

market = json.load(open("data/market_snapshot.json"))
specs = json.load(open("data/options_book.json"))["options"]

ctx = pe.PricingContext.from_tables(**market)
ctx.preprocessing()
ctx.calibration()
ctx.simulate_paths("2027-12-01", mc_samples=100_000, dynamics="lsv")
results = ctx.price_all(specs)
```

Supported `dynamics`: `"lv"` or `"lsv"` (default `"lsv"`).

---



## Repository layout

```
data/
  market_snapshot.json       JSON market snapshot used by Python workflow
  options_book.json          JSON option book for MC pricing
  pricing_json_schema.yaml   JSON fields and product schema
python/
  pricing_engine_pybind.cpp  pybind11 bindings (`pricing_engine` module)
  notebooks/                 usage and validation notebooks
src/
  main.cpp                   LV/LSV path-generation timing harness
  core/                      market/model/calibration/simulation/benchmark code
  options/                   FD and MC product pricers
CMakeLists.txt
```

---

