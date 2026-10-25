/*
 * relay.h
 *
 * Non-blocking protection relay state machine.
 *
 *   NORMAL --(trip condition persists >= debounce_trip_ms)--> FAULT
 *   FAULT  --(clear condition persists >= debounce_clear_ms)--> RECOVERY
 *   RECOVERY --(stays clear for recovery_ms)--> NORMAL
 *   RECOVERY --(trip condition reappears >= debounce_trip_ms)--> FAULT
 *   * -> LOCKOUT if trip_count within lockout_window_ms reaches
 *        max_trips_in_window (guards against a flapping/continuously
 *        faulted input cycling the relay forever)
 *   LOCKOUT --(lockout_hold_ms elapses, if non-zero)--> RECOVERY
 *   LOCKOUT --(relay_manual_reset())--> RECOVERY
 *
 * Anti-chatter is provided by two independent mechanisms that both have
 * to be defeated before the relay changes state:
 *   - Hysteresis: the value must fall below reset_high (a lower
 *     threshold than trip_high) to be considered "clear", not just dip
 *     back under trip_high. Same idea mirrored for trip_low/reset_low.
 *   - Debounce: a trip or clear *condition* must be continuously true
 *     for a minimum dwell time before it is acted on, so a single noisy
 *     sample right at the boundary can't flip the state.
 *
 * relay_update() must be called once per control-loop tick (no fixed
 * rate required) and never blocks or waits.
 */
#ifndef RELAY_H
#define RELAY_H

#include <stdint.h>
#include <stdbool.h>
#include "anomaly.h"

typedef enum {
    RELAY_STATE_NORMAL = 0,
    RELAY_STATE_FAULT,
    RELAY_STATE_RECOVERY,
    RELAY_STATE_LOCKOUT,
} relay_state_t;

typedef enum {
    TRIP_NONE = 0,
    TRIP_OVER_THRESHOLD,
    TRIP_UNDER_THRESHOLD,
    TRIP_SENSOR_FROZEN,
    TRIP_SENSOR_JUMP,
    TRIP_SENSOR_RANGE,
    TRIP_LOCKOUT_LIMIT,
} trip_cause_t;

typedef struct {
    /* over-threshold protection (e.g. overcurrent/overvoltage/overtemp) */
    bool     high_enabled;
    float    trip_high;
    float    reset_high;      /* must be < trip_high */

    /* under-threshold protection (e.g. undervoltage), optional */
    bool     low_enabled;
    float    trip_low;
    float    reset_low;       /* must be > trip_low */

    /* anti-chatter timing */
    uint32_t debounce_trip_ms;   /* condition must persist this long to trip   */
    uint32_t debounce_clear_ms;  /* condition must persist this long to clear  */
    uint32_t recovery_ms;        /* timed hold in RECOVERY before back to NORMAL */

    /* flap/lockout protection under sustained fault injection */
    uint8_t  max_trips_in_window;  /* 0 disables lockout                       */
    uint32_t lockout_window_ms;
    uint32_t lockout_hold_ms;      /* 0 = latched, requires relay_manual_reset */
} relay_config_t;

typedef void (*relay_log_fn)(uint32_t now, relay_state_t from, relay_state_t to,
                              trip_cause_t cause, float value);

typedef struct {
    relay_state_t state;
    trip_cause_t  active_cause;

    /* debounce bookkeeping */
    bool     trip_cond_pending;
    uint32_t trip_cond_since;
    trip_cause_t trip_cond_cause;

    bool     clear_cond_pending;
    uint32_t clear_cond_since;

    uint32_t state_enter_tick;

    /* lockout bookkeeping */
    uint8_t  trip_count_in_window;
    uint32_t trip_window_start;
    uint32_t lockout_enter_tick;

    relay_log_fn log_fn; /* optional; NULL to disable logging callback */
} relay_t;

void relay_config_default(relay_config_t *cfg);
void relay_init(relay_t *r, relay_log_fn log_fn);

/* Call once per sample/tick. `anomaly` should come from anomaly_update()
 * for the same sample. Never blocks. */
void relay_update(relay_t *r, const relay_config_t *cfg,
                   float sensor_value, anomaly_type_t anomaly, uint32_t now);

/* Clears a latched LOCKOUT (only has effect while in LOCKOUT). Returns
 * true if a reset was actually performed. */
bool relay_manual_reset(relay_t *r, uint32_t now);

const char *relay_state_name(relay_state_t s);
const char *relay_cause_name(trip_cause_t c);

#endif /* RELAY_H */
