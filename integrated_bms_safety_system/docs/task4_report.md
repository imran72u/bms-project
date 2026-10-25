# Fault State Machine with Structured Recovery — Design Report

## 1. States

| State | Meaning |
|---|---|
| `NORMAL` | No active faults. Full functionality. |
| `DEGRADED` | At least one isolated, non-critical fault is active. System keeps operating, at reduced confidence/capability, while the fault is tracked. |
| `FAILSAFE` | A major (or an unresolved/escalated) fault makes normal operation unsafe. Outputs are driven to a safe configuration; the system waits on a bounded, sampled recovery-verification process rather than resuming immediately. |
| `SHUTDOWN` | Terminal state. Reached from a critical/unrecoverable fault, an exhausted recovery budget, or an explicit manual command. Only left by an external reset (`begin()` again — modeling an MCU reboot/power cycle), never by internal logic. |

All four states and the events that move between them are enums
(`SystemState`, `Event`), and `currentState` is only ever assigned inside a
single function, `handleEvent()`, which looks the next state up in a static
transition table indexed `[currentState][event]`. This is what makes the
machine **deterministic**: for any given (state, event) pair there is
exactly one defined outcome, defined once, in one place — not scattered
`if` statements that could disagree with each other.

## 2. The transition table

```
                FAULT_   FAULT_   FAULT_    FAULT_    RECOVERY_  RECOVERY_  RECOVERY_   MANUAL_
                MINOR    MAJOR    CRITICAL  CLEARED   VERIFIED   FAILED     EXHAUSTED   SHUTDOWN
NORMAL          DEGRADED FAILSAFE SHUTDOWN  --        --         --         --          SHUTDOWN
DEGRADED        DEGRADED FAILSAFE SHUTDOWN  NORMAL    --         --         --          SHUTDOWN
FAILSAFE        --       --       SHUTDOWN  --        DEGRADED   FAILSAFE   SHUTDOWN    SHUTDOWN
SHUTDOWN        --       --       --        --        --         --         --          --
```
(`--` = sentinel: the event does not cause a transition from that state; it
is ignored, though any fault information carried with it is still recorded.)

Key properties visible directly in the table:

- **`FAILSAFE` + `FAULT_CLEARED` is deliberately `--`.** Clearing the fault
  condition alone is *not* sufficient to leave `FAILSAFE` — see Section 4.
- **`FAILSAFE` + `RECOVERY_VERIFIED` → `DEGRADED`, not `NORMAL`.** Recovery
  is a two-step process: FAILSAFE steps down to DEGRADED first, and only
  DEGRADED's ordinary `FAULT_CLEARED → NORMAL` rule (which requires *zero*
  active faults) completes the return to full operation.
- **`SHUTDOWN`'s entire row is `--`.** It is intentionally terminal from
  inside the state machine.

## 3. Fault isolation and identification

Faults are not represented as a single global "something is wrong" flag.
Each fault is a `FaultRecord { source, code, severity, timestamps,
description }`, and up to `MAX_ACTIVE_FAULTS` (8) can be active
simultaneously, each identified independently by `(FaultSource, code)`:

```cpp
enum class FaultSource : uint8_t { NONE, BATTERY_CELL, RELAY, COMMUNICATION, ADC };
```

`code` disambiguates within a source (e.g. which battery cell index, which
ADC channel). `raiseFault()`/`clearFault()` operate on a specific
`(source, code)` pair, so:

- A frozen ADC channel 1 and a relay mismatch can be active *at the same
  time*, each independently tracked, and each independently cleared.
- Re-raising an already-active fault (the common case — a detector calls
  `raiseFault()` on every poll while the condition persists) only refreshes
  its `lastSeenMs` timestamp; it does **not** re-trigger a state transition
  or spam the log. A transition/log entry is only produced the first time a
  given fault becomes active, or if its severity escalates.
- `getActiveFault(i)` / `activeFaultCount()` let the LCD engine (from the
  companion display task) or any other subsystem query *what* is wrong and
  *where* at any time — the information persists for as long as the fault
  is active, not just at the instant it was raised.

## 4. Frozen ADC and relay mismatch detection

Both detectors follow the same shape: sample → compare against an expected
pattern → require the deviation to *persist* past a settle/debounce window
before calling it a fault, so that normal transients don't false-trigger.

**`AdcFreezeMonitor`** — an ADC reading is flagged frozen if it hasn't moved
by more than `epsilon` in longer than `freezeTimeoutMs`. A perfectly flat
reading over an extended window (rather than the small noise any live
sensor exhibits) is the classic signature of a disconnected sensor, a stuck
sample-and-hold capacitor, or a wedged ADC peripheral.

**`RelayMismatchMonitor`** — compares the commanded relay state against a
feedback/readback pin. A relay takes a few milliseconds to physically
switch and can bounce, so a *sustained* disagreement (`mismatchTimeoutMs`,
default 500 ms) is required — not an instantaneous one — before it's
treated as a stuck/welded contact or a broken driver stage.

**`CommTimeoutMonitor`** — a standard heartbeat pattern: every valid
received message resets a timer; if too long elapses without one,
communication is considered lost.

These modules are intentionally decoupled from `FaultStateMachine` itself —
they just return booleans. The demo sketch is the glue that turns "ADC
channel 1 is frozen" into `raiseFault(FaultSource::ADC, 1,
FaultSeverity::MINOR, ...)`. This separation means each detector can be unit
tested independently of the state machine, and new detectors (e.g. current
sense, thermal) can be added without touching the state machine at all.

## 5. Structured transition logging

Every transition writes a `TransitionLogEntry` into a fixed-size ring
buffer (`TRANSITION_LOG_SIZE = 32`) containing:

```cpp
struct TransitionLogEntry {
  unsigned long timestampMs;
  SystemState previousState;
  SystemState newState;
  Event triggerEvent;
  FaultSource faultSource;  // NONE if not fault-triggered (e.g. a recovery event)
  uint8_t faultCode;
};
```

and simultaneously emits a structured, key=value formatted line through an
injected log sink (kept as a function pointer rather than a hard `Serial`
dependency, so the core logic stays host-testable), e.g.:

```
ts=9114 prev_state=DEGRADED new_state=FAILSAFE event=FAULT_MAJOR fault_source=RELAY fault_code=0
```

key=value format was chosen over free-text sentences because it is
trivially machine-parseable (log tooling, post-mortem scripts, or a
ground-station UI can `split` on `=` and space) while still being
human-readable directly off a serial terminal — satisfying "structured
format" without needing a JSON library on a memory-constrained MCU.

## 6. Recovery from FAILSAFE: why not immediate, and how it works

Jumping straight from "the fault condition cleared" back to `NORMAL` is
risky for two reasons: (1) a fault may clear and reappear within
milliseconds (e.g. a marginal connection), so a single clean reading is not
strong evidence of a real fix, and (2) resuming full-power operation
immediately after a major fault (e.g. a relay that was just stuck) removes
the safety margin exactly when it's most valuable to keep it a little
longer.

Instead, `tickRecoveryVerification()` runs only while the machine is in
`FAILSAFE`, sampling on a fixed cadence (`sampleIntervalMs`, default
400 ms) and requiring `requiredCleanSamples` (default 5) **consecutive**
clean samples — about a 2-second sustained-health window by default —
before declaring recovery verified:

- Any single unhealthy sample during the sequence resets the clean-sample
  counter to zero and counts as one failed **attempt**.
- After `maxAttempts` (default 3) failed attempts, the machine stops
  retrying and raises `RECOVERY_EXHAUSTED`, which the transition table
  routes to `SHUTDOWN`.
- Only a full, uninterrupted run of clean samples raises
  `RECOVERY_VERIFIED`, which steps the machine down to `DEGRADED` — from
  which the ordinary "all faults clear → `NORMAL`" rule takes over.

This gives three tunable knobs (`requiredCleanSamples`,
`sampleIntervalMs`, `maxAttempts`) that let the verification window and
retry budget be matched to how safety-critical a given deployment is,
without touching the state machine's core logic.

## 7. Why this is deadlock-free

"Deadlock-free" here means: the system can never reach a state where no
event can make progress toward either full recovery or a safe terminal
state.

- **Every non-terminal state has at least one outward path on some
  event.** `NORMAL` and `DEGRADED` both escalate on worsening faults and
  (for `DEGRADED`) recover on `FAULT_CLEARED`. `FAILSAFE` has exactly two
  ultimate outcomes reachable from it — `DEGRADED` (via verified recovery)
  or `SHUTDOWN` (via a new critical fault or an exhausted recovery
  budget) — so it can never sit inertly forever.
- **`FAILSAFE`'s self-loop (`RECOVERY_FAILED → FAILSAFE`) is bounded.**
  Without `maxAttempts`, a fault that flickers on/off right at the
  boundary of the verification window could in principle keep resetting
  the clean-sample counter forever, leaving the system stuck retrying
  indefinitely. The attempt counter guarantees termination: after a fixed,
  finite number of failed verification sequences, the system is *forced*
  to a decision (`SHUTDOWN`) rather than being allowed to loop forever.
- **`SHUTDOWN` is intentionally terminal, not an oversight.** A state
  machine that could talk itself out of `SHUTDOWN` internally would
  undermine the guarantee that shutdown means shutdown; requiring an
  external reset is a deliberate safety property, not a missing
  transition.
- **Repeated/ongoing faults cannot re-trigger transitions.** Because
  `raiseFault()` only emits an event on a fault's first occurrence or on
  severity escalation (Section 3), a single persistent fault condition
  cannot flood the machine with repeated `FAULT_MAJOR` events and cause
  transition churn — it settles into the appropriate state and stays
  there until something actually changes.

Together, these guarantee that from any state, the system either reaches
`NORMAL` (fully recovered), stays in a well-defined `DEGRADED` holding
state while a minor fault persists, or reaches `SHUTDOWN` in bounded time
when a fault cannot be verified as resolved — there is no state and no
event sequence that leaves the system stuck making no progress.
