/*
 * logger.h
 *
 * Every state transition is captured into a small ring buffer instead of
 * being printed/transmitted immediately. A separate, non-blocking
 * log_process() call (made once per main-loop iteration) drains one
 * entry at a time. This keeps the relay's control path free of any I/O
 * latency -- on a real target, replace the body of log_emit() in
 * logger.c with a non-blocking UART/CAN transmit and this interface
 * does not need to change.
 *
 * If the buffer fills up before it can be drained (e.g. an extreme
 * fault-injection burst), the oldest unread entry is overwritten and a
 * dropped-entry counter is incremented -- logging can never itself block
 * or stall the safety logic.
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include "relay.h"

#define LOG_CAPACITY 64

typedef struct {
    uint32_t      tick_ms;
    relay_state_t from;
    relay_state_t to;
    trip_cause_t  cause;
    float         value;
} log_entry_t;

void logger_init(void);

/* Matches relay_log_fn -- pass directly to relay_init(). Never blocks. */
void logger_on_transition(uint32_t now, relay_state_t from, relay_state_t to,
                           trip_cause_t cause, float value);

/* Drains and prints/transmits up to `max_entries` queued log entries.
 * Call once per main-loop iteration. Never blocks waiting for new data;
 * simply returns 0 if the queue is empty. Returns the number drained. */
uint32_t logger_process(uint32_t max_entries);

uint32_t logger_dropped_count(void);
uint32_t logger_total_logged(void);

#endif /* LOGGER_H */
