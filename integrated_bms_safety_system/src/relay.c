#include "relay.h"
#include <string.h>

void relay_config_default(relay_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->high_enabled = true;
    cfg->trip_high    = 90.0f;
    cfg->reset_high   = 80.0f;   /* hysteresis gap: must fall 10 units below trip */

    cfg->low_enabled  = false;
    cfg->trip_low     = 0.0f;
    cfg->reset_low    = 0.0f;

    cfg->debounce_trip_ms  = 200;
    cfg->debounce_clear_ms = 300;
    cfg->recovery_ms       = 1000;

    cfg->max_trips_in_window = 3;
    cfg->lockout_window_ms   = 5000;
    cfg->lockout_hold_ms     = 10000;
}

void relay_init(relay_t *r, relay_log_fn log_fn)
{
    memset(r, 0, sizeof(*r));
    r->state = RELAY_STATE_NORMAL;
    r->active_cause = TRIP_NONE;
    r->log_fn = log_fn;
}

static trip_cause_t evaluate_trip_cause(const relay_config_t *cfg, float value, anomaly_type_t anomaly)
{
    /* Sensor-integrity faults take priority: an unhealthy sensor makes
     * the threshold comparison itself untrustworthy. */
    if (anomaly == ANOMALY_FROZEN)       return TRIP_SENSOR_FROZEN;
    if (anomaly == ANOMALY_JUMP)         return TRIP_SENSOR_JUMP;
    if (anomaly == ANOMALY_OUT_OF_RANGE) return TRIP_SENSOR_RANGE;

    if (cfg->high_enabled && value >= cfg->trip_high) return TRIP_OVER_THRESHOLD;
    if (cfg->low_enabled  && value <= cfg->trip_low)  return TRIP_UNDER_THRESHOLD;
    return TRIP_NONE;
}

static bool value_is_clear(const relay_config_t *cfg, float value, anomaly_type_t anomaly)
{
    if (anomaly != ANOMALY_NONE) return false;
    if (cfg->high_enabled && value > cfg->reset_high) return false;
    if (cfg->low_enabled  && value < cfg->reset_low)  return false;
    return true;
}

static bool debounce_trip(relay_t *r, trip_cause_t cause, uint32_t debounce_ms, uint32_t now)
{
    if (cause == TRIP_NONE) {
        r->trip_cond_pending = false;
        return false;
    }
    if (!r->trip_cond_pending || r->trip_cond_cause != cause) {
        r->trip_cond_pending = true;
        r->trip_cond_cause = cause;
        r->trip_cond_since = now;
    }
    return (uint32_t)(now - r->trip_cond_since) >= debounce_ms;
}

static bool debounce_clear(relay_t *r, bool is_clear, uint32_t debounce_ms, uint32_t now)
{
    if (!is_clear) {
        r->clear_cond_pending = false;
        return false;
    }
    if (!r->clear_cond_pending) {
        r->clear_cond_pending = true;
        r->clear_cond_since = now;
    }
    return (uint32_t)(now - r->clear_cond_since) >= debounce_ms;
}

static void do_transition(relay_t *r, relay_state_t new_state, trip_cause_t cause,
                           float value, uint32_t now)
{
    relay_state_t old = r->state;
    if (r->log_fn) {
        r->log_fn(now, old, new_state, cause, value);
    }
    r->state = new_state;
    r->active_cause = cause;
    r->state_enter_tick = now;
    r->trip_cond_pending = false;
    r->clear_cond_pending = false;
    if (new_state == RELAY_STATE_LOCKOUT) {
        r->lockout_enter_tick = now;
    }
}

/* Handles the flap/lockout bookkeeping any time we are about to trip,
 * from either NORMAL or RECOVERY. Sustained/continuous fault injection
 * gets funneled into a single stable LOCKOUT state instead of chattering
 * between FAULT and RECOVERY forever. */
static void enter_fault(relay_t *r, const relay_config_t *cfg, trip_cause_t cause,
                         float value, uint32_t now)
{
    if (cfg->max_trips_in_window > 0) {
        if (r->trip_count_in_window == 0 ||
            (uint32_t)(now - r->trip_window_start) > cfg->lockout_window_ms) {
            r->trip_window_start = now;
            r->trip_count_in_window = 0;
        }
        r->trip_count_in_window++;
        if (r->trip_count_in_window >= cfg->max_trips_in_window) {
            do_transition(r, RELAY_STATE_LOCKOUT, TRIP_LOCKOUT_LIMIT, value, now);
            return;
        }
    }
    do_transition(r, RELAY_STATE_FAULT, cause, value, now);
}

void relay_update(relay_t *r, const relay_config_t *cfg,
                   float sensor_value, anomaly_type_t anomaly, uint32_t now)
{
    trip_cause_t cause = evaluate_trip_cause(cfg, sensor_value, anomaly);
    bool is_clear = value_is_clear(cfg, sensor_value, anomaly);

    switch (r->state) {

    case RELAY_STATE_NORMAL:
        if (debounce_trip(r, cause, cfg->debounce_trip_ms, now)) {
            enter_fault(r, cfg, cause, sensor_value, now);
        }
        break;

    case RELAY_STATE_FAULT:
        if (cause != TRIP_NONE) {
            /* Still (or newly) faulted -- keep the logged cause current
             * and make sure a stale clear-timer can't sneak us out. */
            r->active_cause = cause;
            r->clear_cond_pending = false;
        }
        if (debounce_clear(r, is_clear, cfg->debounce_clear_ms, now)) {
            do_transition(r, RELAY_STATE_RECOVERY, TRIP_NONE, sensor_value, now);
        }
        break;

    case RELAY_STATE_RECOVERY:
        if (debounce_trip(r, cause, cfg->debounce_trip_ms, now)) {
            enter_fault(r, cfg, cause, sensor_value, now);
            break;
        }
        if (cause == TRIP_NONE &&
            (uint32_t)(now - r->state_enter_tick) >= cfg->recovery_ms) {
            do_transition(r, RELAY_STATE_NORMAL, TRIP_NONE, sensor_value, now);
        }
        break;

    case RELAY_STATE_LOCKOUT:
        if (cfg->lockout_hold_ms != 0 &&
            (uint32_t)(now - r->lockout_enter_tick) >= cfg->lockout_hold_ms) {
            do_transition(r, RELAY_STATE_RECOVERY, TRIP_NONE, sensor_value, now);
        }
        break;
    }
}

bool relay_manual_reset(relay_t *r, uint32_t now)
{
    if (r->state != RELAY_STATE_LOCKOUT) {
        return false;
    }
    do_transition(r, RELAY_STATE_RECOVERY, TRIP_NONE, 0.0f, now);
    r->trip_count_in_window = 0;
    return true;
}

const char *relay_state_name(relay_state_t s)
{
    switch (s) {
        case RELAY_STATE_NORMAL:   return "NORMAL";
        case RELAY_STATE_FAULT:    return "FAULT";
        case RELAY_STATE_RECOVERY: return "RECOVERY";
        case RELAY_STATE_LOCKOUT:  return "LOCKOUT";
        default:                   return "UNKNOWN";
    }
}

const char *relay_cause_name(trip_cause_t c)
{
    switch (c) {
        case TRIP_NONE:            return "-";
        case TRIP_OVER_THRESHOLD:  return "OVER_THRESHOLD";
        case TRIP_UNDER_THRESHOLD: return "UNDER_THRESHOLD";
        case TRIP_SENSOR_FROZEN:   return "SENSOR_FROZEN";
        case TRIP_SENSOR_JUMP:     return "SENSOR_JUMP";
        case TRIP_SENSOR_RANGE:    return "SENSOR_RANGE";
        case TRIP_LOCKOUT_LIMIT:   return "LOCKOUT_LIMIT";
        default:                   return "UNKNOWN";
    }
}
