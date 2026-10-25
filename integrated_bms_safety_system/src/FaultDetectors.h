/*
 * ============================================================================
 *  Fault Detectors: frozen ADC, relay mismatch, communication timeout
 * ============================================================================
 *  These are small, self-contained monitors that just produce a boolean
 *  "is this specific thing currently faulted?" result each time they are
 *  sampled. They do not know about SystemState at all -- the demo sketch
 *  is the glue that calls raiseFault()/clearFault() on the state machine
 *  based on what these return. That separation keeps detection logic
 *  reusable and independently testable.
 * ============================================================================
 */

#ifndef FAULT_DETECTORS_H
#define FAULT_DETECTORS_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Frozen-ADC detector
// ---------------------------------------------------------------------------
// An ADC channel is considered "frozen" (stuck-at fault) if its reading has
// not moved by more than `epsilon` for longer than `freezeTimeoutMs`, given
// that in a real, live system some amount of sensor noise/drift is always
// expected. A perfectly flat-lined reading over an extended window is a
// classic symptom of a disconnected sensor, a stuck sample-and-hold, or a
// wedged ADC peripheral -- not a genuinely stable signal.
// ---------------------------------------------------------------------------
class AdcFreezeMonitor {
public:
  void begin(float epsilon, unsigned long freezeTimeoutMs, unsigned long nowMs) {
    this->epsilon = epsilon;
    this->freezeTimeoutMs = freezeTimeoutMs;
    this->lastValue = 0.0f;
    this->lastChangeMs = nowMs;
    this->initialized = false;
  }

  // Call once per sample. Returns true if the channel is currently
  // considered frozen.
  bool sample(float value, unsigned long nowMs) {
    if (!initialized) {
      lastValue = value;
      lastChangeMs = nowMs;
      initialized = true;
      return false;
    }
    float delta = value - lastValue;
    if (delta < 0) delta = -delta;

    if (delta > epsilon) {
      // Genuine movement -- reset the freeze timer.
      lastValue = value;
      lastChangeMs = nowMs;
      return false;
    }
    return (nowMs - lastChangeMs) >= freezeTimeoutMs;
  }

private:
  float epsilon;
  unsigned long freezeTimeoutMs;
  float lastValue;
  unsigned long lastChangeMs;
  bool initialized;
};

// ---------------------------------------------------------------------------
// Relay mismatch detector
// ---------------------------------------------------------------------------
// Compares the commanded relay state (what the firmware told the relay
// driver to do) against the sensed feedback state (a readback pin wired to
// the actual contact position). A relay that is welded shut, mechanically
// stuck open, or has a broken driver stage will disagree with the command
// for a sustained period -- transient disagreement during the relay's own
// switching time (a few ms of contact bounce) should NOT trigger a fault,
// which is why a debounce/settle window is required before declaring one.
// ---------------------------------------------------------------------------
class RelayMismatchMonitor {
public:
  void begin(unsigned long mismatchTimeoutMs) {
    this->mismatchTimeoutMs = mismatchTimeoutMs;
    this->mismatchSinceMs = 0;
    this->inMismatch = false;
  }

  // Call once per sample with the last commanded state and the current
  // feedback reading. Returns true once the mismatch has persisted longer
  // than mismatchTimeoutMs (i.e. it survived the switching/settle window).
  bool sample(bool commandedState, bool feedbackState, unsigned long nowMs) {
    if (commandedState == feedbackState) {
      inMismatch = false;
      return false;
    }
    if (!inMismatch) {
      inMismatch = true;
      mismatchSinceMs = nowMs;
      return false; // just started -- still inside the settle window
    }
    return (nowMs - mismatchSinceMs) >= mismatchTimeoutMs;
  }

private:
  unsigned long mismatchTimeoutMs;
  unsigned long mismatchSinceMs;
  bool inMismatch;
};

// ---------------------------------------------------------------------------
// Communication timeout detector
// ---------------------------------------------------------------------------
// Simple heartbeat/watchdog pattern: caller reports every time a valid
// message is received; isTimedOut() reports whether too long has passed
// since the last one.
// ---------------------------------------------------------------------------
class CommTimeoutMonitor {
public:
  void begin(unsigned long timeoutMs, unsigned long nowMs) {
    this->timeoutMs = timeoutMs;
    this->lastRxMs = nowMs;
  }

  void onMessageReceived(unsigned long nowMs) {
    lastRxMs = nowMs;
  }

  bool isTimedOut(unsigned long nowMs) const {
    return (nowMs - lastRxMs) >= timeoutMs;
  }

private:
  unsigned long timeoutMs;
  unsigned long lastRxMs;
};

#endif // FAULT_DETECTORS_H
