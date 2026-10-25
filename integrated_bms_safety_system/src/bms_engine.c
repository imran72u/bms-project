/**
 * bms_engine.c
 * -----------------------------------------------------------------------
 * Implementation of the modular BMS analysis engine declared in
 * bms_engine.h. All internal state is file-scope static, sized entirely
 * from NUM_CELLS (bms_config.h) -- there is no other place in this file
 * where a cell count is hard-coded, which is what makes 4 -> 16 cells a
 * one-line recompile rather than a rewrite.
 * -----------------------------------------------------------------------
 */

#include <string.h>
#include "bms_engine.h"

/* ---- Internal (file-scope) state --------------------------------------
 * Everything here is O(NUM_CELLS) in size. There is exactly one array
 * that grows with pack size (cells[]); every other field is O(1)
 * regardless of NUM_CELLS.
 * ---------------------------------------------------------------------- */
static BMS_CellSample s_cells[NUM_CELLS];

static uint16_t   s_weakest_index;
static uint16_t   s_strongest_index;
static uint16_t   s_prev_imbalance_mv;
static int32_t     s_last_delta_mv;
static bool       s_have_prev_imbalance;
static BMS_Trend  s_trend;
static uint16_t   s_adaptive_threshold_mv;
static BMS_Status s_status;
static uint32_t   s_cycle_count;

/* ------------------------------------------------------------------- */
void bms_init(void)
{
    memset(s_cells, 0, sizeof(s_cells));
    s_weakest_index         = 0;
    s_strongest_index       = 0;
    s_prev_imbalance_mv     = 0;
    s_last_delta_mv         = 0;
    s_have_prev_imbalance   = false;
    s_trend                 = BMS_TREND_UNKNOWN;
    s_adaptive_threshold_mv = BMS_BASE_THRESHOLD_MV;
    s_status                = BMS_STATUS_OK;
    s_cycle_count           = 0;
}

/* ------------------------------------------------------------------- */
void bms_update_cell(uint16_t cell_index, uint16_t voltage_mv,
                      uint8_t soc_percent, int16_t temperature_c)
{
    if (cell_index >= NUM_CELLS) {
        return; /* out-of-range index silently ignored; caller bug */
    }
    s_cells[cell_index].voltage_mv    = voltage_mv;
    s_cells[cell_index].soc_percent   = soc_percent;
    s_cells[cell_index].temperature_c = temperature_c;
    s_cells[cell_index].valid         = true;
}

/* ------------------------------------------------------------------- */
void bms_update_all(const uint16_t voltages_mv[NUM_CELLS],
                     const uint8_t  soc_percent[NUM_CELLS],
                     const int16_t  temperature_c[NUM_CELLS])
{
    for (uint16_t i = 0; i < NUM_CELLS; ++i) {
        bms_update_cell(i, voltages_mv[i], soc_percent[i], temperature_c[i]);
    }
}

/* ---- Internal helper: adaptive threshold -------------------------------
 * The threshold is derived, not fixed, from two independent factors:
 *
 *  1. State of Charge (SoC): a Li-ion open-circuit-voltage curve is steep
 *     near empty (<20%) and near full (>80%) -- a small real energy
 *     difference between cells produces a LARGER voltage difference in
 *     those zones. Using a fixed mV threshold there causes false alarms.
 *     So the threshold is widened (x2.0) inside the "knee" zones and
 *     kept at its base value in the flat mid-SoC region.
 *
 *  2. Discharge C-rate: under high load, internal-resistance IR-drop
 *     differences between cells (due to manufacturing tolerance / aging)
 *     create transient voltage spread that has nothing to do with actual
 *     charge imbalance. The threshold is widened proportionally to
 *     C-rate (+0.8x per 1C) so the engine doesn't flag every hard
 *     acceleration event as a cell-balance fault.
 *
 * Both adjustments are combined multiplicatively, then clamped to
 * [BMS_MIN_THRESHOLD_MV, BMS_MAX_THRESHOLD_MV] so the threshold can
 * never collapse to zero or run away to an unsafe value.
 * All math is integer/fixed-point (x10) -- no floating point required,
 * which matters for MCU targets with no/slow FPU.
 * ---------------------------------------------------------------------- */
static uint16_t compute_adaptive_threshold(uint8_t avg_soc_percent,
                                            int16_t discharge_rate_c_x10)
{
    /* Start at x10 fixed point for precision through the multiplies. */
    uint32_t threshold_x10 = (uint32_t)BMS_BASE_THRESHOLD_MV * 10u;

    /* --- SoC knee adjustment --- */
    if (avg_soc_percent <= BMS_SOC_KNEE_LOW || avg_soc_percent >= BMS_SOC_KNEE_HIGH) {
        threshold_x10 = (threshold_x10 * (uint32_t)BMS_SOC_KNEE_MULTIPLIER_X10) / 10u;
    }

    /* --- Discharge-rate adjustment --- */
    int16_t c_rate_x10 = discharge_rate_c_x10;
    if (c_rate_x10 < 0) {
        c_rate_x10 = -c_rate_x10; /* charge or discharge, magnitude matters */
    }
    if (c_rate_x10 > BMS_CRATE_MAX_C * 10) {
        c_rate_x10 = BMS_CRATE_MAX_C * 10; /* clamp so extreme spikes don't blow up the threshold */
    }
    /* +0.8x threshold per 1.0C -> scale factor (10 + 8*Crate)/10 in x10 units */
    uint32_t crate_scale_x10 = 10u + ((uint32_t)BMS_CRATE_MULTIPLIER_X10PC * (uint32_t)c_rate_x10) / 10u;
    threshold_x10 = (threshold_x10 * crate_scale_x10) / 10u;

    uint16_t threshold_mv = (uint16_t)(threshold_x10 / 10u);

    if (threshold_mv < BMS_MIN_THRESHOLD_MV) threshold_mv = BMS_MIN_THRESHOLD_MV;
    if (threshold_mv > BMS_MAX_THRESHOLD_MV) threshold_mv = BMS_MAX_THRESHOLD_MV;

    return threshold_mv;
}

/* ------------------------------------------------------------------- */
void bms_process(int16_t discharge_rate_c_x10)
{
    uint16_t min_v = 0xFFFFu;
    uint16_t max_v = 0u;
    uint16_t min_idx = 0u;
    uint16_t max_idx = 0u;
    uint32_t soc_sum = 0u;
    uint16_t valid_count = 0u;

    /* Single O(NUM_CELLS) pass: min, max, and SoC average together. */
    for (uint16_t i = 0; i < NUM_CELLS; ++i) {
        if (!s_cells[i].valid) {
            continue; /* skip cells that have never been sampled */
        }
        uint16_t v = s_cells[i].voltage_mv;
        if (v < min_v) { min_v = v; min_idx = i; }
        if (v > max_v) { max_v = v; max_idx = i; }
        soc_sum += s_cells[i].soc_percent;
        valid_count++;
    }

    if (valid_count == 0u) {
        /* No data yet: leave state at safe defaults. */
        s_status = BMS_STATUS_OK;
        return;
    }

    s_weakest_index   = min_idx;
    s_strongest_index = max_idx;

    uint16_t imbalance_mv = (uint16_t)(max_v - min_v);
    uint8_t  avg_soc = (uint8_t)(soc_sum / valid_count);

    s_adaptive_threshold_mv = compute_adaptive_threshold(avg_soc, discharge_rate_c_x10);

    /* ---- Trend classification ----
     * Compares this cycle's imbalance to the previous cycle's, with a
     * small deadband so sensor noise doesn't flip the trend every tick.
     */
    if (!s_have_prev_imbalance) {
        s_trend = BMS_TREND_UNKNOWN;
        s_last_delta_mv = 0;
    } else {
        int32_t delta = (int32_t)imbalance_mv - (int32_t)s_prev_imbalance_mv;
        s_last_delta_mv = delta;
        if (delta > (int32_t)BMS_TREND_DEADBAND_MV) {
            s_trend = BMS_TREND_INCREASING;
        } else if (delta < -(int32_t)BMS_TREND_DEADBAND_MV) {
            s_trend = BMS_TREND_DECREASING;
        } else {
            s_trend = BMS_TREND_STABLE;
        }
    }

    /* ---- Overall status ---- */
    if (imbalance_mv <= s_adaptive_threshold_mv) {
        s_status = BMS_STATUS_OK;
    } else if (s_trend == BMS_TREND_INCREASING) {
        s_status = BMS_STATUS_CRITICAL;
    } else {
        s_status = BMS_STATUS_WARNING;
    }

    s_prev_imbalance_mv   = imbalance_mv;
    s_have_prev_imbalance = true;
    s_cycle_count++;
}

/* ------------------------------------------------------------------- */
BMS_Snapshot bms_get_snapshot(void)
{
    BMS_Snapshot snap;
    memset(&snap, 0, sizeof(snap));

    snap.num_cells = NUM_CELLS;
    memcpy(snap.cells, s_cells, sizeof(s_cells));

    snap.weakest_index         = s_weakest_index;
    snap.strongest_index       = s_strongest_index;
    snap.weakest_voltage_mv    = s_cells[s_weakest_index].voltage_mv;
    snap.strongest_voltage_mv  = s_cells[s_strongest_index].voltage_mv;

    snap.imbalance_mv = (uint16_t)(snap.strongest_voltage_mv - snap.weakest_voltage_mv);
    snap.imbalance_delta_mv = s_last_delta_mv;

    snap.trend                 = s_trend;
    snap.adaptive_threshold_mv = s_adaptive_threshold_mv;
    snap.status                = s_status;
    snap.cycle_count           = s_cycle_count;

    return snap;
}

uint16_t   bms_get_weakest_index(void)          { return s_weakest_index; }
uint16_t   bms_get_strongest_index(void)        { return s_strongest_index; }
uint16_t   bms_get_imbalance_mv(void)           { return (uint16_t)(s_cells[s_strongest_index].voltage_mv - s_cells[s_weakest_index].voltage_mv); }
BMS_Trend  bms_get_trend(void)                  { return s_trend; }
uint16_t   bms_get_adaptive_threshold_mv(void)  { return s_adaptive_threshold_mv; }
BMS_Status bms_get_status(void)                 { return s_status; }
