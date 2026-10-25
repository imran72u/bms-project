#include "logger.h"
#include <stdio.h>

static log_entry_t g_queue[LOG_CAPACITY];
static uint32_t g_head;   /* next slot to write */
static uint32_t g_tail;   /* next slot to read  */
static uint32_t g_count;  /* entries currently queued */
static uint32_t g_dropped;
static uint32_t g_total;

void logger_init(void)
{
    g_head = g_tail = g_count = g_dropped = g_total = 0;
}

void logger_on_transition(uint32_t now, relay_state_t from, relay_state_t to,
                           trip_cause_t cause, float value)
{
    if (g_count >= LOG_CAPACITY) {
        /* Buffer full: drop the oldest entry rather than block or lose
         * the newest (most operationally relevant) transition. */
        g_tail = (g_tail + 1) % LOG_CAPACITY;
        g_count--;
        g_dropped++;
    }

    log_entry_t *e = &g_queue[g_head];
    e->tick_ms = now;
    e->from = from;
    e->to = to;
    e->cause = cause;
    e->value = value;

    g_head = (g_head + 1) % LOG_CAPACITY;
    g_count++;
    g_total++;
}

/* Replace this with a non-blocking UART/CAN/flash write on target
 * hardware. It must never wait for the transport to be ready; if it
 * isn't ready, just return and try again next call. */
static void log_emit(const log_entry_t *e)
{
    printf("[t=%6u ms] %-8s -> %-8s  cause=%-15s value=%.2f\n",
           e->tick_ms,
           relay_state_name(e->from),
           relay_state_name(e->to),
           relay_cause_name(e->cause),
           e->value);
}

uint32_t logger_process(uint32_t max_entries)
{
    uint32_t drained = 0;
    while (drained < max_entries && g_count > 0) {
        log_emit(&g_queue[g_tail]);
        g_tail = (g_tail + 1) % LOG_CAPACITY;
        g_count--;
        drained++;
    }
    return drained;
}

uint32_t logger_dropped_count(void) { return g_dropped; }
uint32_t logger_total_logged(void)  { return g_total; }
