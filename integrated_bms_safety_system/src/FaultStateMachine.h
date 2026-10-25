/*
 * ============================================================================
 *  Fault State Machine with Structured Recovery
 * ============================================================================
 *  A deterministic, table-driven state machine for embedded fault handling.
 *
 *  States:   NORMAL, DEGRADED, FAILSAFE, SHUTDOWN
 *  Faults:   isolated by source (battery cell, relay, communication, ADC)
 *            and tracked simultaneously (multiple active faults supported)
 *  Recovery: FAILSAFE never jumps straight back to NORMAL. It runs a
 *            bounded, sampled verification sequence and only then steps
 *            down to DEGRADED, from which normal fault-clear rules apply.
 *
 *  Portable C++ -- uses only millis()-style timing, no Arduino-only APIs
 *  in the core logic, so it can be unit-tested on a host as well as run
 *  on an MCU. Only the demo .ino wraps it with Serial/hardware I/O.
 * ============================================================================
 */

#ifndef FAULT_STATE_MACHINE_H
#define FAULT_STATE_MACHINE_H

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Core enumerations
// ---------------------------------------------------------------------------

enum class SystemState : uint8_t {
  NORMAL = 0,
  DEGRADED,
  FAILSAFE,
  SHUTDOWN,
  STATE_COUNT   // sentinel, keep last
};

// Events are the only thing allowed to drive a transition. Application code
// (fault detectors, recovery verifier) raises events; it never sets state
// directly. This keeps every transition traceable through one function.
enum class Event : uint8_t {
  FAULT_MINOR = 0,     // isolated, non-critical fault detected
  FAULT_MAJOR,         // fault serious enough to require safe-state
  FAULT_CRITICAL,      // unrecoverable / unsafe-to-continue fault
  FAULT_CLEARED,       // all active faults have cleared
  RECOVERY_VERIFIED,   // FAILSAFE recovery-verification sequence passed
  RECOVERY_FAILED,     // one verification attempt failed (may retry)
  RECOVERY_EXHAUSTED,  // retry budget used up -> must escalate
  MANUAL_SHUTDOWN,      // operator/host commanded shutdown
  EVENT_COUNT           // sentinel, keep last
};

enum class FaultSource : uint8_t {
  NONE = 0,
  BATTERY_CELL,
  RELAY,
  COMMUNICATION,
  ADC,
  SOURCE_COUNT
};

enum class FaultSeverity : uint8_t {
  NONE = 0,
  MINOR,
  MAJOR,
  CRITICAL
};

// ---------------------------------------------------------------------------
// Fault record -- one per (source, code) pair currently tracked.
// Faults persist (isolated + retained) until explicitly cleared, so the
// system always knows *what* is wrong and *where*, not just *that*
// something is wrong.
// ---------------------------------------------------------------------------
struct FaultRecord {
  bool active;
  FaultSource source;
  uint8_t code;              // source-specific fault code, e.g. channel index
  FaultSeverity severity;
  unsigned long raisedAtMs;
  unsigned long lastSeenMs;
  char description[32];
};

#define MAX_ACTIVE_FAULTS 8

// ---------------------------------------------------------------------------
// Structured transition log entry
// ---------------------------------------------------------------------------
struct TransitionLogEntry {
  unsigned long timestampMs;
  SystemState previousState;
  SystemState newState;
  Event triggerEvent;
  FaultSource faultSource;   // FaultSource::NONE if not fault-triggered
  uint8_t faultCode;
};

#define TRANSITION_LOG_SIZE 32

// ---------------------------------------------------------------------------
// Recovery verification configuration/state (FAILSAFE -> DEGRADED path)
// ---------------------------------------------------------------------------
struct RecoveryConfig {
  uint8_t requiredCleanSamples;   // consecutive good samples required
  unsigned long sampleIntervalMs; // spacing between verification samples
  uint8_t maxAttempts;            // full verification sequences allowed
};

// ---------------------------------------------------------------------------
// The state machine itself
// ---------------------------------------------------------------------------
class FaultStateMachine {
public:
  FaultStateMachine();

  void begin(unsigned long nowMs,
             const RecoveryConfig &recoveryConfig = defaultRecoveryConfig());

  // Called once per control loop iteration ("tick"). Drives the recovery
  // verification sampling while in FAILSAFE; otherwise a light no-op.
  void update(unsigned long nowMs);

  // --- Fault reporting API -------------------------------------------------
  // Raise (or refresh) a fault. Automatically raises the matching event
  // (FAULT_MINOR/MAJOR/CRITICAL) the first time a given (source, code)
  // becomes active. Re-raising an already-active fault just refreshes
  // lastSeenMs -- it does not re-trigger a transition, which prevents
  // log spam and self-re-entrant transitions from a single ongoing fault.
  void raiseFault(FaultSource source, uint8_t code, FaultSeverity severity,
                   const char *description, unsigned long nowMs);

  // Clear one specific fault. If this was the last active fault, a
  // FAULT_CLEARED event is raised automatically.
  void clearFault(FaultSource source, uint8_t code, unsigned long nowMs);

  bool hasActiveFaults() const;
  uint8_t activeFaultCount() const;
  const FaultRecord *getActiveFault(uint8_t index) const; // 0..activeFaultCount()-1

  // Manual/operator-driven shutdown, bypasses normal fault severity path.
  void requestManualShutdown(unsigned long nowMs);

  // --- Accessors -------------------------------------------------------
  SystemState getState() const { return currentState; }
  const TransitionLogEntry *getLogEntry(uint8_t index) const; // most recent = index 0
  uint8_t getLogCount() const { return logCount < TRANSITION_LOG_SIZE ? logCount : TRANSITION_LOG_SIZE; }

  // Prints a structured (key=value) line for one log entry. Injected as a
  // callback so this file has no hard Serial.print dependency, keeping it
  // host-testable.
  typedef void (*LogSinkFn)(const char *line);
  void setLogSink(LogSinkFn sink) { logSink = sink; }

private:
  SystemState currentState;
  FaultRecord faults[MAX_ACTIVE_FAULTS];

  TransitionLogEntry log[TRANSITION_LOG_SIZE];
  uint16_t logCount; // total ever logged; ring buffer wraps physically

  RecoveryConfig recovery;
  uint8_t recoveryCleanSampleCount;
  uint8_t recoveryAttemptCount;
  unsigned long lastRecoverySampleMs;

  LogSinkFn logSink;

  // The transition table: for a given (state, event) pair, what state do
  // we move to? SystemState::STATE_COUNT is used as a sentinel meaning
  // "no transition defined -- event ignored in this state."
  static SystemState transitionTable[(size_t)SystemState::STATE_COUNT][(size_t)Event::EVENT_COUNT];

  void handleEvent(Event ev, unsigned long nowMs,
                    FaultSource src = FaultSource::NONE, uint8_t code = 0);
  void emitLog(SystemState prev, SystemState next, Event ev,
               FaultSource src, uint8_t code, unsigned long nowMs);
  FaultSeverity highestActiveSeverity() const;
  int findFault(FaultSource source, uint8_t code) const;
  void tickRecoveryVerification(unsigned long nowMs);
  bool sampleRecoveryHealthy(unsigned long nowMs) const;

  static RecoveryConfig defaultRecoveryConfig() {
    RecoveryConfig c;
    c.requiredCleanSamples = 5;     // 5 consecutive clean samples...
    c.sampleIntervalMs = 400;       // ...spaced 400 ms apart (~2 s total)
    c.maxAttempts = 3;              // give up and escalate after 3 tries
    return c;
  }
};

// Utility: human-readable names, useful for logging/UI (e.g. the LCD engine).
const char *systemStateName(SystemState s);
const char *faultSourceName(FaultSource s);
const char *eventName(Event e);

#endif // FAULT_STATE_MACHINE_H
