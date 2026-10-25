/**
 * bms_config.h
 * -----------------------------------------------------------------------
 * Single point of compile-time configuration for the BMS engine.
 * Changing NUM_CELLS is the ONLY edit required to scale the engine from
 * a 4-cell pack to a 16-cell pack (or any other size). No other source
 * file references a hard-coded cell count.
 * -----------------------------------------------------------------------
 */

#ifndef BMS_CONFIG_H
#define BMS_CONFIG_H

/* ---- Scalability knob -------------------------------------------------
 * This is the single compile-time constant referenced in the task brief.
 * Every array in the engine is sized off this value, so recompiling with
 * a different NUM_CELLS is the entire "porting" effort.
 * ---------------------------------------------------------------------- */
#ifndef NUM_CELLS
#define NUM_CELLS 4u          /* default pack size; override with -DNUM_CELLS=16 */
#endif

/* ---- Physical / electrical constants ---------------------------------- */
#define BMS_CELL_VMIN_MV        2500u   /* absolute minimum safe cell voltage (mV) */
#define BMS_CELL_VMAX_MV        4200u   /* absolute maximum safe cell voltage (mV) */

/* ---- Adaptive imbalance threshold tuning ------------------------------
 * The engine does NOT use one fixed imbalance limit. Instead the limit is
 * derived at run time from State of Charge (SoC) and discharge C-rate.
 * These constants parameterize that curve; see bms_engine.c for the
 * formula and rationale.
 * ---------------------------------------------------------------------- */
#define BMS_BASE_THRESHOLD_MV      30u   /* nominal imbalance limit at mid-SoC, low load */
#define BMS_MIN_THRESHOLD_MV       15u   /* threshold never tightens below this */
#define BMS_MAX_THRESHOLD_MV      120u   /* threshold never relaxes above this */

#define BMS_SOC_KNEE_LOW           20u   /* % SoC below which cell curve is steep */
#define BMS_SOC_KNEE_HIGH          80u   /* % SoC above which cell curve is steep */
#define BMS_SOC_KNEE_MULTIPLIER_X10 20   /* x10 fixed-point: 2.0x threshold in knee zones */

#define BMS_CRATE_MULTIPLIER_X10PC  8    /* x10 fixed-point: +0.8x threshold per 1C of load */
#define BMS_CRATE_MAX_C            5     /* discharge rate is clamped to this many C for scaling */

/* ---- History / trend detection ---------------------------------------- */
#define BMS_TREND_DEADBAND_MV       1u   /* imbalance deltas smaller than this count as STABLE */

#endif /* BMS_CONFIG_H */
