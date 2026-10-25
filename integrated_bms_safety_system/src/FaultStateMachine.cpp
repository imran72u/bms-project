#include "FaultStateMachine.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// THE TRANSITION TABLE
// ---------------------------------------------------------------------------
// Rows   = current state  (NORMAL, DEGRADED, FAILSAFE, SHUTDOWN)
// Cols   = event          (FAULT_MINOR, FAULT_MAJOR, FAULT_CRITICAL,
//                           FAULT_CLEARED, RECOVERY_VERIFIED,
//                           RECOVERY_FAILED, RECOVERY_EXHAUSTED,
//                           MANUAL_SHUTDOWN)
// Value  = next state, or SystemState::STATE_COUNT as a sentinel meaning
//          "this event is not valid/does not cause a transition from this
//          state" (it is simply ignored -- the fault itself is still
//          recorded by raiseFault(), only the *transition* is skipped).
//
// Design notes embedded directly in the table:
//  - FAILSAFE + FAULT_CLEARED does NOT go to STATE_COUNT->NORMAL. Clearing
//    the fault alone is deliberately insufficient to leave FAILSAFE -- only
//    RECOVERY_VERIFIED (produced by the sampled verification sequence in
//    tickRecoveryVerification()) can do that, and even then it steps down
//    to DEGRADED, not directly to NORMAL.
//  - FAILSAFE + RECOVERY_VERIFIED -> DEGRADED. DEGRADED's own
//    FAULT_CLEARED -> NORMAL rule then applies once no faults remain,
//    giving a two-step, structured recovery path rather than a single
//    jump back to full operation.
//  - RECOVERY_EXHAUSTED (retry budget used up) always escalates to
//    SHUTDOWN, which guarantees the system can never oscillate inside
//    FAILSAFE forever -- this is the deadlock-avoidance guarantee.
//  - SHUTDOWN's row is all sentinels: it is a true terminal state within
//    this state machine and is only left by calling begin() again, which
//    models an intentional external reset (e.g. MCU reboot / operator
//    power cycle), not a software-internal recovery.
// ---------------------------------------------------------------------------
SystemState FaultStateMachine::transitionTable[(size_t)SystemState::STATE_COUNT][(size_t)Event::EVENT_COUNT] = {
  // ----------------------------- NORMAL -----------------------------------
  {
    /* FAULT_MINOR        */ SystemState::DEGRADED,
    /* FAULT_MAJOR        */ SystemState::FAILSAFE,
    /* FAULT_CRITICAL     */ SystemState::SHUTDOWN,
    /* FAULT_CLEARED      */ SystemState::STATE_COUNT,
    /* RECOVERY_VERIFIED  */ SystemState::STATE_COUNT,
    /* RECOVERY_FAILED    */ SystemState::STATE_COUNT,
    /* RECOVERY_EXHAUSTED */ SystemState::STATE_COUNT,
    /* MANUAL_SHUTDOWN    */ SystemState::SHUTDOWN,
  },
  // ---------------------------- DEGRADED -----------------------------------
  {
    /* FAULT_MINOR        */ SystemState::DEGRADED,   // stays; new fault logged
    /* FAULT_MAJOR        */ SystemState::FAILSAFE,
    /* FAULT_CRITICAL     */ SystemState::SHUTDOWN,
    /* FAULT_CLEARED      */ SystemState::NORMAL,
    /* RECOVERY_VERIFIED  */ SystemState::STATE_COUNT,
    /* RECOVERY_FAILED    */ SystemState::STATE_COUNT,
    /* RECOVERY_EXHAUSTED */ SystemState::STATE_COUNT,
    /* MANUAL_SHUTDOWN    */ SystemState::SHUTDOWN,
  },
  // ---------------------------- FAILSAFE -----------------------------------
  {
    /* FAULT_MINOR        */ SystemState::STATE_COUNT, // already failsafe
    /* FAULT_MAJOR        */ SystemState::STATE_COUNT, // already failsafe
    /* FAULT_CRITICAL     */ SystemState::SHUTDOWN,
    /* FAULT_CLEARED      */ SystemState::STATE_COUNT, // NOT enough alone
    /* RECOVERY_VERIFIED  */ SystemState::DEGRADED,    // structured step-down
    /* RECOVERY_FAILED    */ SystemState::FAILSAFE,    // stay, retry
    /* RECOVERY_EXHAUSTED */ SystemState::SHUTDOWN,    // bounded -> escalate
    /* MANUAL_SHUTDOWN    */ SystemState::SHUTDOWN,
  },
  // ---------------------------- SHUTDOWN -----------------------------------
  {
    /* FAULT_MINOR        */ SystemState::STATE_COUNT,
    /* FAULT_MAJOR        */ SystemState::STATE_COUNT,
    /* FAULT_CRITICAL     */ SystemState::STATE_COUNT,
    /* FAULT_CLEARED      */ SystemState::STATE_COUNT,
    /* RECOVERY_VERIFIED  */ SystemState::STATE_COUNT,
    /* RECOVERY_FAILED    */ SystemState::STATE_COUNT,
    /* RECOVERY_EXHAUSTED */ SystemState::STATE_COUNT,
    /* MANUAL_SHUTDOWN    */ SystemState::STATE_COUNT, // already terminal
  },
};

// ---------------------------------------------------------------------------
// Construction / init
// ---------------------------------------------------------------------------
FaultStateMachine::FaultStateMachine()
  : currentState(SystemState::NORMAL),
    logCount(0),
    recoveryCleanSampleCount(0),
    recoveryAttemptCount(0),
    lastRecoverySampleMs(0),
    logSink(nullptr) {
  memset(faults, 0, sizeof(faults));
  memset(log, 0, sizeof(log));
}

void FaultStateMachine::begin(unsigned long nowMs, const RecoveryConfig &recoveryConfig) {
  currentState = SystemState::NORMAL;
  memset(faults, 0, sizeof(faults));
  logCount = 0;
  recovery = recoveryConfig;
  recoveryCleanSampleCount = 0;
  recoveryAttemptCount = 0;
  lastRecoverySampleMs = nowMs;
}

// ---------------------------------------------------------------------------
// Per-tick update -- only does meaningful work while in FAILSAFE.
// ---------------------------------------------------------------------------
void FaultStateMachine::update(unsigned long nowMs) {
  if (currentState == SystemState::FAILSAFE) {
    tickRecoveryVerification(nowMs);
  }
}

// ---------------------------------------------------------------------------
// Fault tracking
// ---------------------------------------------------------------------------
int FaultStateMachine::findFault(FaultSource source, uint8_t code) const {
  for (uint8_t i = 0; i < MAX_ACTIVE_FAULTS; i++) {
    if (faults[i].active && faults[i].source == source && faults[i].code == code) {
      return i;
    }
  }
  return -1;
}

void FaultStateMachine::raiseFault(FaultSource source, uint8_t code, FaultSeverity severity,
                                    const char *description, unsigned long nowMs) {
  int idx = findFault(source, code);
  bool isNewOrEscalated = false;

  if (idx < 0) {
    // Look for a free slot; isolation requires every distinct fault to
    // keep its own record so source+code are never conflated.
    idx = -1;
    for (uint8_t i = 0; i < MAX_ACTIVE_FAULTS; i++) {
      if (!faults[i].active) { idx = i; break; }
    }
    if (idx < 0) {
      // Fault table exhausted -- extremely unlikely with 8 slots, but
      // fail safe rather than silently drop the fault: overwrite slot 0
      // and force a MAJOR event so the system reacts to the saturation
      // itself rather than staying falsely quiet.
      idx = 0;
      severity = (severity < FaultSeverity::MAJOR) ? FaultSeverity::MAJOR : severity;
    }
    faults[idx].active = true;
    faults[idx].source = source;
    faults[idx].code = code;
    faults[idx].severity = severity;
    faults[idx].raisedAtMs = nowMs;
    strncpy(faults[idx].description, description, sizeof(faults[idx].description) - 1);
    faults[idx].description[sizeof(faults[idx].description) - 1] = '\0';
    isNewOrEscalated = true;
  } else if (faults[idx].severity < severity) {
    // Same fault, but it got worse -- treat the escalation like a new event.
    faults[idx].severity = severity;
    isNewOrEscalated = true;
  }

  faults[idx].lastSeenMs = nowMs;

  if (!isNewOrEscalated) {
    return; // ongoing, already-known fault -- no re-transition, no log spam
  }

  Event ev;
  switch (severity) {
    case FaultSeverity::MINOR:    ev = Event::FAULT_MINOR;    break;
    case FaultSeverity::MAJOR:    ev = Event::FAULT_MAJOR;    break;
    case FaultSeverity::CRITICAL: ev = Event::FAULT_CRITICAL; break;
    default: return; // FaultSeverity::NONE should never be raised as a fault
  }
  handleEvent(ev, nowMs, source, code);
}

void FaultStateMachine::clearFault(FaultSource source, uint8_t code, unsigned long nowMs) {
  int idx = findFault(source, code);
  if (idx >= 0) {
    faults[idx].active = false;
  }
  if (!hasActiveFaults()) {
    handleEvent(Event::FAULT_CLEARED, nowMs, source, code);
  }
}

bool FaultStateMachine::hasActiveFaults() const {
  for (uint8_t i = 0; i < MAX_ACTIVE_FAULTS; i++) {
    if (faults[i].active) return true;
  }
  return false;
}

uint8_t FaultStateMachine::activeFaultCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_ACTIVE_FAULTS; i++) {
    if (faults[i].active) n++;
  }
  return n;
}

const FaultRecord *FaultStateMachine::getActiveFault(uint8_t index) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_ACTIVE_FAULTS; i++) {
    if (faults[i].active) {
      if (n == index) return &faults[i];
      n++;
    }
  }
  return nullptr;
}

FaultSeverity FaultStateMachine::highestActiveSeverity() const {
  FaultSeverity worst = FaultSeverity::NONE;
  for (uint8_t i = 0; i < MAX_ACTIVE_FAULTS; i++) {
    if (faults[i].active && faults[i].severity > worst) {
      worst = faults[i].severity;
    }
  }
  return worst;
}

void FaultStateMachine::requestManualShutdown(unsigned long nowMs) {
  handleEvent(Event::MANUAL_SHUTDOWN, nowMs, FaultSource::NONE, 0);
}

// ---------------------------------------------------------------------------
// Structured recovery verification (FAILSAFE -> DEGRADED)
// ---------------------------------------------------------------------------
// This is deliberately a *sampled, sustained* check rather than a single
// instantaneous test, and it is bounded by recovery.maxAttempts so the
// system can never sit in an infinite FAILSAFE<->FAILSAFE retry loop
// without eventually making a decision (either full recovery to DEGRADED,
// or escalation to SHUTDOWN). This is the deadlock-avoidance guarantee for
// the recovery path specifically.
bool FaultStateMachine::sampleRecoveryHealthy(unsigned long /*nowMs*/) const {
  // Minimum bar: no fault record currently active. In a fuller system this
  // would also re-run source-specific self tests (relay feedback readback,
  // ADC sanity read, comms ping) via injected callbacks; hasActiveFaults()
  // already reflects those once the corresponding detector modules call
  // clearFault() for each resolved condition.
  return !hasActiveFaults();
}

void FaultStateMachine::tickRecoveryVerification(unsigned long nowMs) {
  if (nowMs - lastRecoverySampleMs < recovery.sampleIntervalMs) {
    return;
  }
  lastRecoverySampleMs = nowMs;

  if (sampleRecoveryHealthy(nowMs)) {
    recoveryCleanSampleCount++;
    if (recoveryCleanSampleCount >= recovery.requiredCleanSamples) {
      handleEvent(Event::RECOVERY_VERIFIED, nowMs, FaultSource::NONE, 0);
      recoveryCleanSampleCount = 0;
      recoveryAttemptCount = 0;
    }
  } else {
    // A fault reappeared mid-sequence: this verification attempt failed.
    recoveryCleanSampleCount = 0;
    recoveryAttemptCount++;
    if (recoveryAttemptCount >= recovery.maxAttempts) {
      handleEvent(Event::RECOVERY_EXHAUSTED, nowMs, FaultSource::NONE, 0);
    } else {
      handleEvent(Event::RECOVERY_FAILED, nowMs, FaultSource::NONE, 0);
    }
  }
}

// ---------------------------------------------------------------------------
// Central transition dispatcher -- the ONLY place currentState is assigned.
// ---------------------------------------------------------------------------
void FaultStateMachine::handleEvent(Event ev, unsigned long nowMs, FaultSource src, uint8_t code) {
  SystemState next = transitionTable[(size_t)currentState][(size_t)ev];
  if (next == SystemState::STATE_COUNT) {
    return; // sentinel: event not valid from this state, silently ignored
  }

  SystemState prev = currentState;
  currentState = next;

  // Fresh entry into FAILSAFE always starts recovery bookkeeping from zero,
  // whether we arrived from NORMAL or DEGRADED.
  if (next == SystemState::FAILSAFE && prev != SystemState::FAILSAFE) {
    recoveryCleanSampleCount = 0;
    recoveryAttemptCount = 0;
    lastRecoverySampleMs = nowMs;
  }

  emitLog(prev, next, ev, src, code, nowMs);
}

// ---------------------------------------------------------------------------
// Structured logging
// ---------------------------------------------------------------------------
void FaultStateMachine::emitLog(SystemState prev, SystemState next, Event ev,
                                 FaultSource src, uint8_t code, unsigned long nowMs) {
  TransitionLogEntry &entry = log[logCount % TRANSITION_LOG_SIZE];
  entry.timestampMs = nowMs;
  entry.previousState = prev;
  entry.newState = next;
  entry.triggerEvent = ev;
  entry.faultSource = src;
  entry.faultCode = code;
  logCount++;

  if (logSink) {
    char line[128];
    snprintf(line, sizeof(line),
             "ts=%lu prev_state=%s new_state=%s event=%s fault_source=%s fault_code=%u",
             nowMs, systemStateName(prev), systemStateName(next), eventName(ev),
             faultSourceName(src), (unsigned)code);
    logSink(line);
  }
}

const TransitionLogEntry *FaultStateMachine::getLogEntry(uint8_t index) const {
  uint8_t count = getLogCount();
  if (index >= count) return nullptr;
  // index 0 == most recent
  uint16_t physicalMostRecent = (logCount - 1) % TRANSITION_LOG_SIZE;
  uint16_t physicalIdx = (physicalMostRecent + TRANSITION_LOG_SIZE - index) % TRANSITION_LOG_SIZE;
  return &log[physicalIdx];
}

// ---------------------------------------------------------------------------
// Name lookup helpers
// ---------------------------------------------------------------------------
const char *systemStateName(SystemState s) {
  switch (s) {
    case SystemState::NORMAL:   return "NORMAL";
    case SystemState::DEGRADED: return "DEGRADED";
    case SystemState::FAILSAFE: return "FAILSAFE";
    case SystemState::SHUTDOWN: return "SHUTDOWN";
    default: return "UNKNOWN_STATE";
  }
}

const char *faultSourceName(FaultSource s) {
  switch (s) {
    case FaultSource::NONE:          return "NONE";
    case FaultSource::BATTERY_CELL:  return "BATTERY_CELL";
    case FaultSource::RELAY:         return "RELAY";
    case FaultSource::COMMUNICATION: return "COMMUNICATION";
    case FaultSource::ADC:           return "ADC";
    default: return "UNKNOWN_SOURCE";
  }
}

const char *eventName(Event e) {
  switch (e) {
    case Event::FAULT_MINOR:        return "FAULT_MINOR";
    case Event::FAULT_MAJOR:        return "FAULT_MAJOR";
    case Event::FAULT_CRITICAL:     return "FAULT_CRITICAL";
    case Event::FAULT_CLEARED:      return "FAULT_CLEARED";
    case Event::RECOVERY_VERIFIED:  return "RECOVERY_VERIFIED";
    case Event::RECOVERY_FAILED:    return "RECOVERY_FAILED";
    case Event::RECOVERY_EXHAUSTED: return "RECOVERY_EXHAUSTED";
    case Event::MANUAL_SHUTDOWN:    return "MANUAL_SHUTDOWN";
    default: return "UNKNOWN_EVENT";
  }
}
