/*
 * anomaly.h
 *
 * Sensor health monitor. Detects three classes of sensor-level faults:
 *
 *   1. FROZEN   - reading has not moved by more than an epsilon for
 *                 longer than a configured timeout (stuck ADC, dead
 *                 sensor, broken wire holding last value, etc).
 *
 *   2. JUMP     - an unrealistic, non-physical step in the reading.
 *                 Distinguished from a genuine fast load change by a
 *                 confirmation window: a real change settles at the new
 *                 level for several consecutive samples; a bad sample or
 *                 EMI glitch reverts back or never settles.
 *
 *   3. RANGE    - reading outside the sensor's physically valid range
 *                 (e.g. a thermocouple reporting -900 C). Reported
 *                 immediately since it can never be "real".
 *
 * All decisions are made from a small rolling window of samples using
 * a running mean/standard deviation, so the "how big a jump is too big"
 * threshold adapts to how noisy the signal normally is, rather than
 * using one fixed constant that would be wrong at every operating point.
 *
 * This module performs no I/O and no waiting; anomaly_update() must be
 * called once per new sample and returns immediately.
 */
#ifndef ANOMALY_H
#define ANOMALY_H

#include <stdint.h>
#include <stdbool.h>

#define ANOMALY_WINDOW_SIZE 16

typedef enum {
    ANOMALY_NONE = 0,
    ANOMALY_FROZEN,
    ANOMALY_JUMP,
    ANOMALY_OUT_OF_RANGE,
} anomaly_type_t;

typedef struct {
    float    min_valid;            /* hard lower bound of a physically valid reading   */
    float    max_valid;            /* hard upper bound of a physically valid reading   */
    float    freeze_epsilon;       /* smaller-than-this change counts as "no change"   */
    uint32_t freeze_timeout_ms;    /* how long a frozen reading is tolerated           */
    float    jump_sigma_mult;      /* how many std-devs away counts as "suspicious"    */
    float    jump_min_delta;       /* absolute floor so a near-zero sigma doesn't make
                                       every tiny wiggle "suspicious"                   */
    uint8_t  jump_confirm_samples; /* consecutive samples needed to confirm a jump as a
                                       genuine new operating point                      */
    uint32_t jump_max_pending_ms;  /* if a candidate jump neither confirms nor reverts
                                       within this time, declare it a sensor fault      */
} anomaly_config_t;

typedef struct {
    /* rolling statistics window */
    float   buf[ANOMALY_WINDOW_SIZE];
    uint8_t buf_head;
    uint8_t buf_count;
    float   mean;
    float   std_dev;

    /* frozen-reading tracking */
    bool     has_last;
    float    last_value;
    uint32_t last_change_tick;

    /* pending-jump confirmation state */
    bool     jump_pending;
    float    jump_baseline;      /* mean value before the candidate jump   */
    float    jump_candidate;     /* the new value being evaluated          */
    uint8_t  jump_confirm_count;
    uint32_t jump_pending_since;

    bool     initialized;
} anomaly_window_t;

/* Fills cfg with reasonable defaults; caller should override to match the
 * physical sensor being monitored. */
void anomaly_config_default(anomaly_config_t *cfg);

void anomaly_init(anomaly_window_t *w);

/* Feed one new raw sample. Must be called once per sample, in order,
 * with a non-decreasing `now`. Returns the anomaly classification for
 * this sample (ANOMALY_NONE if the sample looks healthy or is a noise
 * spike that has not been confirmed as a real fault). */
anomaly_type_t anomaly_update(anomaly_window_t *w, const anomaly_config_t *cfg,
                               float new_value, uint32_t now);

const char *anomaly_type_name(anomaly_type_t t);

#endif /* ANOMALY_H */
