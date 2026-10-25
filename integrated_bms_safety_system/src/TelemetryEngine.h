/*
 * ============================================================================
 *  Event-Driven Telemetry Engine
 * ============================================================================
 *  Architecture decision (read this before the rest of the file):
 *
 *  There is exactly ONE code path for outgoing telemetry, not two
 *  ("send directly when connected" vs "queue when offline"). Every event,
 *  connected or not, is pushed into a fixed-size FIFO ring buffer with a
 *  monotonically increasing sequence number. A separate, throttled drain
 *  step continuously pops from the front of that same queue and forwards
 *  to the dashboard whenever connectivity allows.
 *
 *  This single-path design is what makes correct ordering trivial and
 *  connectivity-transition-proof: there is no seam where a "live" event
 *  could race ahead of a "queued" one, because they are the same queue.
 *  It also naturally rate-limits outbound traffic (useful for Blynk's
 *  per-account operation limits) and gives a simple, always-accurate
 *  "queue depth" number for the dashboard: it's just how many events are
 *  currently waiting to be drained, which is ~0 when keeping up live and
 *  grows during an outage.
 *
 *  This file is intentionally free of any WiFi/Blynk headers so it can be
 *  compiled and unit-tested on a host. The .ino wires it to real hardware
 *  by providing a `SendFn` callback that performs the actual
 *  Blynk.virtualWrite() calls.
 * ============================================================================
 */

#ifndef TELEMETRY_ENGINE_H
#define TELEMETRY_ENGINE_H

#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
enum class TelemetryEventType : uint8_t {
  CELL_VOLTAGE = 0,   // index = cell #, floatValue = volts
  WEAKEST_CELL,       // index = cell #, floatValue = volts
  STRONGEST_CELL,     // index = cell #, floatValue = volts
  RELAY_STATE,        // intValue = 0/1
  FAULT_STATE,        // text = state name, intValue = active fault count
  WIFI_HEALTH,        // floatValue = RSSI dBm, text = category (GOOD/FAIR/POOR)
  HEARTBEAT,          // no payload, just proves liveness
  QUEUE_OVERFLOW,     // intValue = number of events dropped
  PACK_METRICS,       // floatValue=imbalance V, intValue=SoC %, text=status
  TEMPERATURE,        // floatValue=pack temperature C
  CURRENT,            // floatValue=pack current A
  BMS_TREND,          // text=STABLE/INCREASING/DECREASING
  BMS_STATUS,         // text=OK/WARNING/CRITICAL
  ANOMALY_STATUS,     // text=NONE/FROZEN/JUMP/OUT_OF_RANGE
  EVENT_TYPE_COUNT
};

struct TelemetryEvent {
  uint32_t sequence;
  unsigned long timestampMs;   // when the event actually occurred
  TelemetryEventType type;
  uint8_t index;
  float floatValue;
  int32_t intValue;
  char text[16];
};

#define OFFLINE_QUEUE_CAPACITY 64

// ---------------------------------------------------------------------------
// Fixed-size FIFO ring buffer. Overflow policy: drop the OLDEST entry to
// make room for the newest. Rationale: for live telemetry, the most
// recent state is generally more actionable than a stale one, and a
// synthetic QUEUE_OVERFLOW event is pushed so operators are explicitly
// told data was lost rather than silently missing it.
// ---------------------------------------------------------------------------
class OfflineEventQueue {
public:
  void begin() {
    head = 0; tail = 0; count = 0; overflowCounter = 0;
  }

  void push(const TelemetryEvent &ev) {
    if (count == OFFLINE_QUEUE_CAPACITY) {
      // Drop oldest: advance head, don't grow count.
      head = (head + 1) % OFFLINE_QUEUE_CAPACITY;
      count--;
      overflowCounter++;
    }
    buffer[tail] = ev;
    tail = (tail + 1) % OFFLINE_QUEUE_CAPACITY;
    count++;
  }

  bool pop(TelemetryEvent &out) {
    if (count == 0) return false;
    out = buffer[head];
    head = (head + 1) % OFFLINE_QUEUE_CAPACITY;
    count--;
    return true;
  }

  bool peek(TelemetryEvent &out) const {
    if (count == 0) return false;
    out = buffer[head];
    return true;
  }

  uint16_t depth() const { return count; }
  uint16_t capacity() const { return OFFLINE_QUEUE_CAPACITY; }
  uint32_t overflowCount() const { return overflowCounter; }

private:
  TelemetryEvent buffer[OFFLINE_QUEUE_CAPACITY];
  uint16_t head = 0, tail = 0, count = 0;
  uint32_t overflowCounter = 0;
};

// ---------------------------------------------------------------------------
// TelemetryEngine: owns the queue + sequence counter, exposes emit() for
// change-detection code to call, and drain() for the main loop to call
// every tick.
// ---------------------------------------------------------------------------
class TelemetryEngine {
public:
  // Called by the drain loop once per event that is actually forwarded.
  // Returns true if the send succeeded (engine advances past it either
  // way -- a failed send is treated the same as "connection dropped
  // mid-drain," which the caller should detect via connectedFn() on the
  // next tick rather than us silently retrying forever here).
  typedef void (*SendFn)(const TelemetryEvent &ev, bool isReplay);
  typedef bool (*ConnectedFn)(void);

  void begin(unsigned long nowMs) {
    queue.begin();
    nextSequence = 1;
    (void)nowMs;
  }

  // The ONLY way events enter the system. Called by change-detection
  // logic -- never on a fixed timer for regular data (HEARTBEAT is the
  // deliberate, documented exception; see report).
  void emit(TelemetryEventType type, uint8_t index, float floatValue,
            int32_t intValue, const char *text, unsigned long nowMs) {
    TelemetryEvent ev;
    ev.sequence = nextSequence++;
    ev.timestampMs = nowMs;
    ev.type = type;
    ev.index = index;
    ev.floatValue = floatValue;
    ev.intValue = intValue;
    ev.text[0] = '\0';
    if (text) {
      strncpy(ev.text, text, sizeof(ev.text) - 1);
      ev.text[sizeof(ev.text) - 1] = '\0';
    }

    queue.push(ev);
    // Deliberately NOT pushing a synthetic QUEUE_OVERFLOW event into this
    // same queue here: if the queue is already full, that synthetic event
    // would itself immediately overflow and evict more real data, cascading
    // (each overflowing emit would then cost two slots instead of one).
    // Overflow is instead tracked purely as a counter (queue.overflowCount(),
    // exposed below) that the caller polls; see reportOverflowIfChanged().
  }

  // Call periodically (e.g. once per main-loop tick) alongside emit().
  // Detects overflow-counter changes via ordinary change-detection and
  // emits a single QUEUE_OVERFLOW telemetry event for the delta -- using
  // the exact same "only send on meaningful change" pattern as every other
  // event source, and only ever one controlled emit() call per change,
  // never a recursive push from inside push() itself.
  void reportOverflowIfChanged(unsigned long nowMs) {
    uint32_t current = queue.overflowCount();
    if (current != lastReportedOverflowCount) {
      uint32_t delta = current - lastReportedOverflowCount;
      lastReportedOverflowCount = current;
      emit(TelemetryEventType::QUEUE_OVERFLOW, 0, 0.0f, (int32_t)delta, nullptr, nowMs);
    }
  }

  // Call every loop tick. Drains at most maxPerTick events IF connected,
  // preserving FIFO order. Throttling (maxPerTick, and the caller's own
  // tick rate) exists to respect dashboard-service rate limits and to
  // avoid a burst of hundreds of backlog events saturating the link the
  // instant it comes back.
  void drain(SendFn sendFn, ConnectedFn connectedFn, uint8_t maxPerTick,
             unsigned long nowMs, unsigned long liveStalenessThresholdMs) {
    if (!connectedFn()) return;
    for (uint8_t i = 0; i < maxPerTick; i++) {
      TelemetryEvent ev;
      if (!queue.pop(ev)) break;
      bool isReplay = (nowMs - ev.timestampMs) > liveStalenessThresholdMs;
      sendFn(ev, isReplay);
    }
  }

  uint16_t pendingDepth() const { return queue.depth(); }
  uint16_t queueCapacity() const { return queue.capacity(); }
  uint32_t overflowCount() const { return queue.overflowCount(); }

private:
  OfflineEventQueue queue;
  uint32_t nextSequence = 1;
  uint32_t lastReportedOverflowCount = 0;
};

#endif // TELEMETRY_ENGINE_H
