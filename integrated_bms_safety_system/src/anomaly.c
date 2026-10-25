#include "anomaly.h"
#include <math.h>
#include <string.h>

static uint32_t elapsed_ms_local(uint32_t since, uint32_t now);

void anomaly_config_default(anomaly_config_t *cfg)
{
    cfg->min_valid            = -20.0f;
    cfg->max_valid            = 150.0f;
    cfg->freeze_epsilon       = 0.02f;
    cfg->freeze_timeout_ms    = 3000;
    cfg->jump_sigma_mult      = 5.0f;
    cfg->jump_min_delta       = 3.0f;
    cfg->jump_confirm_samples = 4;
    cfg->jump_max_pending_ms  = 500;
}

void anomaly_init(anomaly_window_t *w)
{
    memset(w, 0, sizeof(*w));
}

/* Recompute mean/std-dev over the current window. O(window size); fine
 * for the small windows and sample rates typical of protection relays. */
static void recompute_stats(anomaly_window_t *w)
{
    if (w->buf_count == 0) {
        w->mean = 0.0f;
        w->std_dev = 0.0f;
        return;
    }
    float sum = 0.0f;
    for (uint8_t i = 0; i < w->buf_count; i++) {
        sum += w->buf[i];
    }
    float mean = sum / (float)w->buf_count;

    float var_sum = 0.0f;
    for (uint8_t i = 0; i < w->buf_count; i++) {
        float d = w->buf[i] - mean;
        var_sum += d * d;
    }
    float variance = var_sum / (float)w->buf_count;

    w->mean = mean;
    w->std_dev = sqrtf(variance);
}

static void push_sample(anomaly_window_t *w, float value)
{
    w->buf[w->buf_head] = value;
    w->buf_head = (uint8_t)((w->buf_head + 1) % ANOMALY_WINDOW_SIZE);
    if (w->buf_count < ANOMALY_WINDOW_SIZE) {
        w->buf_count++;
    }
    recompute_stats(w);
}

static anomaly_type_t check_frozen(anomaly_window_t *w, const anomaly_config_t *cfg,
                                    float new_value, uint32_t now)
{
    if (!w->has_last) {
        w->has_last = true;
        w->last_value = new_value;
        w->last_change_tick = now;
        return ANOMALY_NONE;
    }

    if (fabsf(new_value - w->last_value) >= cfg->freeze_epsilon) {
        w->last_value = new_value;
        w->last_change_tick = now;
        return ANOMALY_NONE;
    }

    if (elapsed_ms_local(w->last_change_tick, now) >= cfg->freeze_timeout_ms) {
        return ANOMALY_FROZEN;
    }
    return ANOMALY_NONE;
}

/* Local wrap-safe elapsed helper taking an explicit "now" (the anomaly
 * module is time-source agnostic and driven by whatever `now` its caller
 * supplies, so it does not depend on platform_get_tick_ms() directly). */
static uint32_t elapsed_ms_local(uint32_t since, uint32_t now)
{
    return (uint32_t)(now - since);
}

static anomaly_type_t check_jump(anomaly_window_t *w, const anomaly_config_t *cfg,
                                  float new_value, uint32_t now)
{
    /* Not enough history yet to judge what's "normal" -- just build baseline. */
    if (w->buf_count < 3) {
        push_sample(w, new_value);
        return ANOMALY_NONE;
    }

    if (!w->jump_pending) {
        float dev = fabsf(new_value - w->mean);
        float threshold = cfg->jump_sigma_mult * w->std_dev;
        if (threshold < cfg->jump_min_delta) {
            threshold = cfg->jump_min_delta;
        }

        if (dev > threshold) {
            /* Suspicious single-sample deviation: neither accept it into
             * the baseline nor declare a fault yet. Wait for corroborating
             * evidence from the next few samples. */
            w->jump_pending = true;
            w->jump_baseline = w->mean;
            w->jump_candidate = new_value;
            w->jump_confirm_count = 1;
            w->jump_pending_since = now;
            return ANOMALY_NONE;
        }

        push_sample(w, new_value);
        return ANOMALY_NONE;
    }

    /* A candidate jump is pending confirmation. */
    float dist_to_candidate = fabsf(new_value - w->jump_candidate);
    float dist_to_baseline  = fabsf(new_value - w->jump_baseline);
    /* Confirmation tolerance also adapts to normal noise level. */
    float tol = cfg->jump_sigma_mult * 0.5f * w->std_dev;
    if (tol < cfg->jump_min_delta * 0.5f) {
        tol = cfg->jump_min_delta * 0.5f;
    }

    if (dist_to_candidate <= tol) {
        w->jump_confirm_count++;
        if (w->jump_confirm_count >= cfg->jump_confirm_samples) {
            /* Settled at the new level for several samples in a row --
             * this is a genuine rapid load change, not a sensor fault.
             * Rebuild the baseline around the new operating point so
             * future samples are judged against reality, not history. */
            w->jump_pending = false;
            w->buf_count = 0;
            w->buf_head = 0;
            push_sample(w, new_value);
        }
        return ANOMALY_NONE;
    }

    if (dist_to_baseline <= tol) {
        /* Reverted back to where it was before -- that was a transient
         * noise spike/glitch, not a fault and not a real load change. */
        w->jump_pending = false;
        push_sample(w, new_value);
        return ANOMALY_NONE;
    }

    /* Neither confirmed nor reverted. If it's had long enough to do
     * either and still hasn't, the signal is erratic/unreliable. */
    if (elapsed_ms_local(w->jump_pending_since, now) >= cfg->jump_max_pending_ms) {
        w->jump_pending = false;
        push_sample(w, new_value);
        return ANOMALY_JUMP;
    }

    /* Still within the grace period -- keep waiting, don't feed this
     * unresolved sample into the baseline statistics yet. */
    return ANOMALY_NONE;
}

anomaly_type_t anomaly_update(anomaly_window_t *w, const anomaly_config_t *cfg,
                               float new_value, uint32_t now)
{
    if (!w->initialized) {
        anomaly_init(w);
        w->initialized = true;
    }

    /* 1. Hard physical range check -- always conclusive, checked first. */
    if (new_value < cfg->min_valid || new_value > cfg->max_valid) {
        return ANOMALY_OUT_OF_RANGE;
    }

    /* 2. Frozen-reading check (independent of the jump/statistics path,
     *    since a perfectly frozen signal has zero variance and would
     *    otherwise never trip a deviation-based check). */
    anomaly_type_t frozen = check_frozen(w, cfg, new_value, now);
    if (frozen != ANOMALY_NONE) {
        return frozen;
    }

    /* 3. Statistical jump / noise-vs-real-change discrimination. */
    return check_jump(w, cfg, new_value, now);
}

const char *anomaly_type_name(anomaly_type_t t)
{
    switch (t) {
        case ANOMALY_NONE:          return "NONE";
        case ANOMALY_FROZEN:        return "FROZEN";
        case ANOMALY_JUMP:          return "JUMP";
        case ANOMALY_OUT_OF_RANGE:  return "OUT_OF_RANGE";
        default:                    return "UNKNOWN";
    }
}
