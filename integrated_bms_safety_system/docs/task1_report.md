# Modular Battery Management Engine — Design Report

## 1. Overview

This engine is a small, dependency-free C module (`bms_engine.c` / `bms_engine.h`)
that continuously analyzes a battery pack's cell array. It answers four questions
every control-loop cycle:

1. Which cell is weakest and which is strongest right now?
2. How large is the voltage imbalance between them?
3. Is that imbalance getting better or worse (trend)?
4. Is the current imbalance actually a problem, given today's SoC and load
   (adaptive threshold), or is it normal electrochemical/IR-drop noise?

The whole module is driven by **one compile-time constant, `NUM_CELLS`**
(`bms_config.h`). No other file hard-codes a pack size — resizing the pack is a
recompile, not a rewrite.

## 2. Architecture

```
 ┌─────────────────────┐        ┌────────────────────────────┐
 │   Sensor / CAN /     │  push  │   bms_update_cell()         │
 │   ADC layer          ├───────►│   bms_update_all()           │  O(1) per cell
 └─────────────────────┘        └──────────────┬───────────────┘
                                                 │ s_cells[NUM_CELLS]
                                                 ▼
                                  ┌───────────────────────────┐
                                  │   bms_process(c_rate)      │  O(NUM_CELLS)
                                  │  - min/max scan            │
                                  │  - imbalance = max - min   │
                                  │  - adaptive threshold      │
                                  │  - trend vs. last cycle    │
                                  │  - status verdict          │
                                  └──────────────┬──────────────┘
                                                 │
                                                 ▼
                                  ┌───────────────────────────┐
                                  │   bms_get_snapshot()       │  O(1) copy-out
                                  │   (+ narrow accessors)     │
                                  └──────────────┬──────────────┘
                                                 │
                       ┌─────────────────────────┼─────────────────────────┐
                       ▼                         ▼                         ▼
              Cell-balancing task        Fault / logging task      Telemetry / UI task
              (reuses same snapshot,    (reuses same snapshot,    (reuses same snapshot,
               no re-derived logic)      no re-derived logic)      no re-derived logic)
```

**Why this shape:**

- **Input is decoupled from analysis.** `bms_update_cell()` is O(1) and side-effect
  free beyond writing one struct, so it can be called from an ISR or a CAN RX
  callback at whatever rate the hardware delivers samples. The O(n) scan only
  happens inside `bms_process()`, which the control loop calls once per tick —
  so sampling rate and analysis rate are independent.
- **One state machine, many consumers.** Everything a downstream task could need
  (weakest/strongest index and voltage, imbalance, trend, adaptive threshold,
  status, the full per-cell array) is exposed through a single `BMS_Snapshot`
  struct returned by `bms_get_snapshot()`, plus narrow accessors for callers that
  want a single field. Balancing logic, fault logging, and telemetry all read the
  *same* computed result — none of them re-implements "find the weakest cell."
- **Static, bounded memory.** `s_cells[NUM_CELLS]` is a fixed-size static array,
  not a heap allocation — no `malloc`, no fragmentation risk, deterministic
  footprint, appropriate for an MCU-class BMS target.

## 3. Adaptive imbalance threshold (no fixed limits)

A fixed mV threshold is the wrong model for two well-known battery behaviors:

- **SoC-dependent OCV slope.** A Li-ion cell's open-circuit-voltage curve is flat
  in the 20–80% SoC band but steep below 20% and above 80%. Near those
  extremes, a small real charge difference between cells produces a
  disproportionately large voltage gap — a fixed threshold either false-alarms
  at the edges or is too loose in the flat middle.
- **Load-dependent IR drop.** Under a discharge/charge C-rate, cell-to-cell
  differences in internal resistance (manufacturing tolerance, aging) create a
  transient voltage spread that has nothing to do with actual state-of-charge
  imbalance. A threshold that ignores C-rate will trip on every acceleration
  event.

`compute_adaptive_threshold()` in `bms_engine.c` starts from a base threshold
(`BMS_BASE_THRESHOLD_MV`, 30 mV) and applies two multiplicative adjustments:

- **×2.0** when average pack SoC is ≤20% or ≥80% (the OCV "knee" zones).
- **+0.8× per 1.0 C** of discharge/charge rate, clamped at 5C.

The result is clamped to `[BMS_MIN_THRESHOLD_MV, BMS_MAX_THRESHOLD_MV]` (15–120 mV)
so it can never collapse to zero or run away unbounded. All math is integer,
fixed-point (×10), by design — no floating point required, which matters for
MCU targets without a fast FPU.

Measured behavior (from `main.c`'s demo, fixed 25 mV imbalance held constant):

| Scenario                    | Imbalance | Adaptive threshold | Verdict |
|------------------------------|----------:|--------------------:|---------|
| Mid-SoC, resting             | 25 mV     | 30 mV                | OK |
| Mid-SoC, 3.0C discharge       | 25 mV     | 102 mV               | OK |
| Low-SoC knee (10%), resting  | 25 mV     | 60 mV                | OK |
| High-SoC knee (95%), resting | 25 mV     | 60 mV                | OK |

The *same* 25 mV gap is correctly treated as more or less significant depending
on operating conditions, instead of triggering a fixed alarm regardless of context.

## 4. Trend detection

Each `bms_process()` call compares this cycle's imbalance to the previous
cycle's (`s_prev_imbalance_mv`), with a small deadband
(`BMS_TREND_DEADBAND_MV`) so sensor noise doesn't flip the classification every
tick. The result is one of `STABLE`, `INCREASING`, `DECREASING`, or `UNKNOWN`
(only on the very first cycle, before any history exists). Combined with the
adaptive threshold, this produces a three-level status:

- **OK** — imbalance within the adaptive threshold.
- **WARNING** — over threshold, but stable or improving.
- **CRITICAL** — over threshold *and* still increasing.

This lets a balancing controller distinguish "already balancing itself out,
leave it alone" from "getting worse, act now" — a fixed-threshold-only design
cannot make that distinction.

## 5. Scaling from 4 cells to 16 cells

The only change required is the build flag: `-DNUM_CELLS=16` (or editing the
default in `bms_config.h`). No source line in `bms_engine.c` or `bms_engine.h`
mentions a cell count. Measured results, built with GCC 13, `-O2`, on this
environment:

### 5.1 Memory

| NUM_CELLS | `sizeof(BMS_CellSample)` | Cell array size | `sizeof(BMS_Snapshot)` |
|----------:|---------------------------:|------------------:|--------------------------:|
| 4         | 8 bytes                    | 32 bytes           | 64 bytes                   |
| 8         | 8 bytes                    | 64 bytes           | 96 bytes                   |
| 16        | 8 bytes                    | 128 bytes          | 160 bytes                  |

`BMS_CellSample` is fixed at 8 bytes (uint16 voltage, uint8 SoC, int16 temp,
bool valid, plus padding) regardless of pack size. The only structure that
grows with `NUM_CELLS` is the cell array itself — every other field in
`BMS_Snapshot` (indices, imbalance, trend, threshold, status, cycle count) is
O(1). Growth is linear and small: going from 4 → 16 cells (4×) costs about
96 extra bytes of static RAM — trivial even for a small MCU. There is no
dynamic allocation anywhere in the module, so this growth is fully
deterministic and knowable at compile time (`sizeof` works before `main()`
even runs).

Compiled binary size (the whole demo executable, including analysis code and
test harness) was identical — 16,904 bytes — across NUM_CELLS = 4, 8, and 16,
because the analysis code itself (the loop body) doesn't change size; only the
data it loops over does.

### 5.2 Execution time

`bms_process()` is a single O(NUM_CELLS) pass (min/max/SoC-sum in one loop),
followed by O(1) threshold/trend/status computation. Benchmarked at 2,000,000
calls per configuration on this environment:

| NUM_CELLS | ns / call (approx.) |
|----------:|----------------------:|
| 4         | ~8.5 ns |
| 8         | ~11.6 ns |
| 16        | ~19.2 ns |
| 32        | ~36.8 ns |

The growth is linear in cell count, consistent with the single-pass O(n)
design (a small fixed per-call overhead plus a per-cell cost). On a
typical automotive/industrial control loop (10–100 Hz), even a much larger
pack than 16 cells stays several orders of magnitude below the loop budget —
this is not the bottleneck in a real BMS.

### 5.3 Data structures — why they scale cleanly

- **Fixed-size static array (`s_cells[NUM_CELLS]`)** instead of a linked list
  or dynamic array: O(1) indexed access for `bms_update_cell()`, cache-friendly
  sequential scan for `bms_process()`, zero allocation overhead, and a
  compile-time-known memory footprint — important for safety-critical/embedded
  certification where dynamic allocation is often disallowed.
- **No per-cell history buffer.** Trend detection only needs *one* previous
  imbalance value (`s_prev_imbalance_mv`), not a rolling history per cell, so
  memory for trend tracking is O(1) regardless of pack size — it would have
  been tempting (and wasteful) to keep a voltage history array per cell.
- **Single combined scan.** Weakest cell, strongest cell, and average SoC (for
  the adaptive threshold) are all computed in the *same* O(n) loop rather than
  three separate passes, keeping the constant factor low as `NUM_CELLS` grows.

### 5.4 What would need to change beyond 16 cells

For much larger packs (e.g. 96–100+ cells in an EV pack, often organized into
modules), this same architecture still holds, with two practical
adjustments that don't change the public interface:

- Group cells into modules and run `bms_process()` per module plus one
  pack-level rollup, so the "weakest/strongest" search stays local and
  parallelizable across modules rather than one 100-element linear scan.
- Widen `weakest_index`/`strongest_index` types if `NUM_CELLS` ever exceeds
  65,535 (not a realistic concern for battery packs, but worth noting since
  `uint16_t` is used for indices).

Neither change requires touching the adaptive-threshold or trend-detection
logic — they remain correct and reusable as-is.

## 6. Reuse by subsequent tasks

Any later task (cell balancing control, fault logging, telemetry/dashboard,
State-of-Health estimation) should depend only on `bms_engine.h`:

```c
bms_init();
/* ... each cycle ... */
bms_update_all(voltages, socs, temps);
bms_process(discharge_c_rate_x10);
BMS_Snapshot snap = bms_get_snapshot();
/* use snap.weakest_index, snap.imbalance_mv, snap.trend, snap.status, ... */
```

No task needs to re-implement "find the weakest/strongest cell" or "is this
imbalance actually bad right now" — that logic lives in exactly one place.

## 7. Files delivered

| File | Purpose |
|---|---|
| `bms_config.h` | Compile-time constants: `NUM_CELLS`, threshold tuning, cell voltage limits |
| `bms_engine.h` | Public API — the only header downstream tasks should include |
| `bms_engine.c` | Implementation: input, analysis pass, adaptive threshold, trend, snapshot |
| `main.c` | Demo/smoke-test harness (drift-and-recover scenario + threshold scenarios) |
| `Makefile` | `make run NUM_CELLS=4` / `make run NUM_CELLS=16` to prove scaling |
