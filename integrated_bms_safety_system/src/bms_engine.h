/**
 * bms_engine.h
 * -----------------------------------------------------------------------
 * Public interface for the Modular Battery Management (BMS) analysis
 * engine. This header is the ONLY thing downstream modules (balancing
 * control, fault logging, telemetry, UI, etc.) need to include in order
 * to reuse this engine — no internal state or helper is exposed, so the
 * implementation can change freely without breaking callers.
 *
 * Design goals reflected in this API:
 *   1. Scalable  - every data structure is sized from NUM_CELLS
 *                  (bms_config.h). Recompiling with a new NUM_CELLS is
 *                  the entire scaling effort.
 *   2. Continuous - bms_update_cell() feeds new samples in; bms_process()
 *                  runs the analysis pass. Callers control the cadence.
 *   3. Adaptive   - thresholds are derived from SoC / C-rate, not fixed.
 *   4. Reusable   - a single read-only snapshot struct (BMS_Snapshot)
 *                  exposes everything a consumer could need, so no
 *                  caller has to re-implement weakest/strongest/trend
 *                  logic itself.
 * -----------------------------------------------------------------------
 */

#ifndef BMS_ENGINE_H
#define BMS_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Trend classification for pack imbalance --------------------------- */
typedef enum {
    BMS_TREND_UNKNOWN = 0,   /* not enough history yet (first sample) */
    BMS_TREND_STABLE,
    BMS_TREND_INCREASING,
    BMS_TREND_DECREASING
} BMS_Trend;

/* ---- Overall pack health verdict from the last process() pass --------- */
typedef enum {
    BMS_STATUS_OK = 0,       /* imbalance within adaptive threshold */
    BMS_STATUS_WARNING,      /* imbalance above threshold, trend improving/stable */
    BMS_STATUS_CRITICAL      /* imbalance above threshold AND still increasing */
} BMS_Status;

/* ---- Per-cell input sample --------------------------------------------
 * Callers push these in via bms_update_cell(). Only voltage is required
 * every cycle; soc_percent/temperature are optional context used by the
 * adaptive threshold and can be left at last-known values.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint16_t voltage_mv;     /* cell terminal voltage, millivolts        */
    uint8_t  soc_percent;    /* 0-100, estimated state of charge         */
    int16_t  temperature_c;  /* degrees C (signed, can go below 0)       */
    bool     valid;          /* false until first sample is written      */
} BMS_CellSample;

/* ---- Read-only snapshot handed back to callers -------------------------
 * This is the single "clean interface" surface: every field a downstream
 * task needs (weakest/strongest cell, imbalance, trend, adaptive
 * threshold, per-cell array) lives here so no logic has to be duplicated
 * elsewhere.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint16_t   num_cells;                    /* == NUM_CELLS, exposed for generic callers */
    BMS_CellSample cells[NUM_CELLS];          /* full per-cell array (read-only copy)      */

    uint16_t   weakest_index;                 /* index of lowest-voltage cell   */
    uint16_t   strongest_index;                /* index of highest-voltage cell */
    uint16_t   weakest_voltage_mv;
    uint16_t   strongest_voltage_mv;

    uint16_t   imbalance_mv;                  /* strongest - weakest, this cycle */
    int32_t    imbalance_delta_mv;            /* signed change vs previous cycle */
    BMS_Trend  trend;

    uint16_t   adaptive_threshold_mv;         /* threshold used THIS cycle       */
    BMS_Status status;

    uint32_t   cycle_count;                   /* number of process() calls so far */
} BMS_Snapshot;

/* ---- Lifecycle ---------------------------------------------------------- */

/**
 * Resets all internal state (cell samples, history, cycle count).
 * Must be called once before the first bms_update_cell()/bms_process().
 */
void bms_init(void);

/* ---- Input ---------------------------------------------------------------
 * bms_update_cell() is intentionally cheap (O(1), no analysis) so callers
 * can stream samples in from an ADC/CAN ISR at whatever cadence they like;
 * the expensive O(n) scan only happens inside bms_process().
 * ---------------------------------------------------------------------- */

/**
 * Write/overwrite the latest sample for one cell.
 * cell_index must be in [0, NUM_CELLS). Out-of-range indices are ignored.
 */
void bms_update_cell(uint16_t cell_index, uint16_t voltage_mv,
                      uint8_t soc_percent, int16_t temperature_c);

/**
 * Convenience bulk loader: copies NUM_CELLS voltage/SoC/temperature
 * readings in one call (e.g. after a synchronous ADC scan of the pack).
 */
void bms_update_all(const uint16_t voltages_mv[NUM_CELLS],
                     const uint8_t  soc_percent[NUM_CELLS],
                     const int16_t  temperature_c[NUM_CELLS]);

/* ---- Analysis ------------------------------------------------------------
 * bms_process() is the single analysis pass: O(NUM_CELLS) per call.
 * It (a) finds weakest/strongest cells, (b) computes imbalance,
 * (c) compares against the previous cycle to classify the trend,
 * (d) recomputes the adaptive threshold from average SoC / discharge
 * rate, and (e) derives the overall status. Call it once per control
 * loop tick after the cell samples for that tick are loaded.
 *
 * discharge_rate_c_x10 is the pack discharge rate in C-rate, fixed point
 * x10 (e.g. 15 == 1.5C). Pass 0 for "resting"/unknown.
 * ---------------------------------------------------------------------- */
void bms_process(int16_t discharge_rate_c_x10);

/* ---- Output / reuse interface -------------------------------------------
 * Every downstream module should consume the engine ONLY through this
 * function (or the narrower accessors below) rather than reaching into
 * internal state, so the analysis logic is written exactly once.
 * ---------------------------------------------------------------------- */

/** Returns a full read-only snapshot of the latest processed state. */
BMS_Snapshot bms_get_snapshot(void);

/* Narrow convenience accessors, implemented on top of bms_get_snapshot(),
 * provided for callers that only need one field and want to avoid
 * copying the whole struct. */
uint16_t   bms_get_weakest_index(void);
uint16_t   bms_get_strongest_index(void);
uint16_t   bms_get_imbalance_mv(void);
BMS_Trend  bms_get_trend(void);
uint16_t   bms_get_adaptive_threshold_mv(void);
BMS_Status bms_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* BMS_ENGINE_H */
