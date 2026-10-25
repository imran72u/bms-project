# Event-Driven Telemetry and Live Blynk Dashboard — Design Report

## 1. Why event-driven instead of periodic polling

A fixed-rate telemetry stream (e.g. "send everything every 500 ms") wastes
bandwidth and Blynk API operations on values that haven't meaningfully
changed, and — on a free/limited-tier Blynk account in particular — risks
hitting per-second operation limits precisely when there's a burst of
activity worth seeing. This design instead sends a value only when:

- a cell voltage moves by more than `CELL_VOLTAGE_DELTA_THRESHOLD` (0.01 V,
  chosen to sit clearly above normal ADC/reference noise but well below a
  change that matters for pack balancing),
- the **identity** of the weakest or strongest cell changes (not just its
  value) — this is the number that actually matters operationally: which
  cell needs attention,
- the relay's commanded state changes,
- the fault state machine (reused unchanged from the companion task)
  transitions,
- WiFi RSSI moves by more than `RSSI_SIGNIFICANT_DELTA_DBM` (5 dBm) or
  crosses a quality-band boundary (Section 4).

**Deliberate exception — the heartbeat.** A single `HEARTBEAT` event is
still emitted every `HEARTBEAT_INTERVAL_MS` (60 s) regardless of whether
anything changed. Pure change-driven telemetry has one blind spot: if
*nothing* changes for an extended period, a dashboard operator cannot
distinguish "everything is fine and stable" from "the device died and
stopped reporting." The heartbeat exists purely to bound that staleness
window without reintroducing periodic polling for the actual data values —
it carries no payload.

## 2. The offline queue and why ordering is guaranteed "for free"

Rather than special-casing "send directly when connected, buffer when
not," every event — live or offline — goes through exactly one path:
`TelemetryEngine::emit()` always pushes into a single fixed-size FIFO ring
buffer (`OfflineEventQueue`, capacity 64), tagged with a monotonically
increasing sequence number. A separate `drain()` step, called every loop
tick, pops from the front of that same queue and forwards to Blynk
whenever `blynkConnectedFn()` is true.

Because there is only one queue and one drain path, there is no seam where
a live event could be sent ahead of an older queued one — draining is
strictly FIFO by construction, so **correct order is a structural
guarantee, not a policy that has to be separately enforced.** This also
means the "offline queue depth" the dashboard shows (`V17`) is always
simply accurate: it's the same counter whether the device has been offline
for an hour or came back up a second ago.

**Fixed size and overflow policy.** The queue capacity (64) is fixed at
compile time — an embedded system cannot afford unbounded RAM growth
during an extended outage. When full, `push()` drops the **oldest**
pending event to make room for the newest, on the reasoning that for live
telemetry the most current state is generally more actionable than a very
stale one, and unbounded growth is not an option. Every drop increments a
counter; the counter (not a synthetic queue event) is what's reported —
see the note below on why.

**Overflow reporting was deliberately kept out of the queue itself.** An
earlier version of this design pushed a synthetic `QUEUE_OVERFLOW` event
into the same queue whenever it overflowed. Testing surfaced the flaw
immediately: if the queue is already full, that synthetic event *also*
overflows and evicts more real data, so each overflowing emit ends up
costing two dropped slots instead of one — a cascade that inflates the
reported overflow count and wastes queue capacity precisely when capacity
is scarcest. The fix was to track the overflow count as a plain counter on
the queue (`overflowCount()`) and have the main loop check it for changes
using the exact same "only report on a meaningful change" pattern as every
other data source (`reportOverflowIfChanged()`), emitting a single,
non-recursive event for the delta. This was caught and fixed by writing a
host-side unit test before wiring the code to hardware (Section 6).

## 3. Non-blocking WiFi reconnection

`WiFiReconnectSM` is a small explicit state machine — `DISCONNECTED →
CONNECTING → CONNECTED`, with `WAIT_RETRY` as the failure path back to
`DISCONNECTED` — driven entirely by `millis()` comparisons in `update()`,
called every loop tick. No `delay()` and no blocking calls like
`WiFi.waitForConnectResult()` appear anywhere in it.

- `CONNECTING` gives one attempt `connectTimeoutMs` (8 s default) to
  succeed before giving up on it.
- A failed attempt moves to `WAIT_RETRY` for an **exponentially increasing,
  capped** backoff: `baseRetryMs * 2^attempt`, clamped to `maxRetryMs` (2 s
  → 4 s → 8 s → … capped at 60 s by default). This avoids hammering a
  struggling AP or radio with reconnect attempts every few milliseconds
  during an extended outage, while still recovering quickly from a brief
  one.
- `CONNECTED` continuously checks `WiFi.status()` and drops straight back
  to `DISCONNECTED` — restarting the whole cycle, including a reset retry
  counter — the instant the link is lost.

Hardware access (`WiFi.begin()`, `WiFi.status()`, `WiFi.RSSI()`) is
injected as function pointers rather than called directly inside this
class, specifically so the state machine's *logic* — timeouts, backoff
math, transition correctness — could be compiled and exercised with mock
hardware on a host machine, independent of an actual ESP32 and access
point. That test (included in the development process, not shipped as
part of the sketch) confirmed the exact backoff sequence (1200 ms → 2400
ms wait, doubling as designed) and correct behavior across a simulated
5-second outage followed by a live link drop.

## 4. RSSI monitoring and quality thresholds

RSSI is sampled every `rssiPollIntervalMs` (2 s default) while connected,
and classified into three bands:

| Band | Range | Interpretation |
|---|---|---|
| GOOD | ≥ −60 dBm | Strong, reliable link |
| FAIR | −60 to −75 dBm | Usable but marginal; retries become more likely |
| POOR | < −75 dBm | Weak; expect dropped packets and increased latency |

These are the widely used rule-of-thumb bands for 2.4 GHz WiFi signal
strength and are exposed as tunable constants (`classifyRssi()`) rather
than hard-coded inline, since the "right" thresholds can shift with
antenna design, enclosure shielding, or deployment environment. A new
`WIFI_HEALTH` telemetry event fires only when the category changes *or*
the raw value moves by more than 5 dBm since the last report — giving the
dashboard a live-feeling RSSI gauge without emitting an event on every 1
dBm flutter, which would defeat the purpose of event-driven telemetry.

## 5. Dashboard virtual pin mapping

| Pin | Content | Suggested widget |
|---|---|---|
| V0–V7 | Individual cell voltages (cells 0–7) | Multi-value display / 8x gauge |
| V8 / V9 | Weakest cell index / voltage | Value display |
| V10 / V11 | Strongest cell index / voltage | Value display |
| V12 | Relay status (0/1) | LED |
| V13 / V14 | Fault state name / active fault count | Value display |
| V15 / V16 | WiFi RSSI (dBm) / quality category | Gauge / Value display |
| V17 | Offline queue depth (0–64) | Gauge |
| V18 | Data mode: `"LIVE"` or `"REPLAY"` | Value display |
| V19 | Queue overflow count (should read 0 in normal operation) | Value display |

**Distinguishing live from queued data (V18).** Every event carries the
timestamp of when it *actually occurred* (`ev.timestampMs`), not when it
was sent. At drain time, if `now - ev.timestampMs` exceeds
`LIVE_STALENESS_THRESHOLD_MS` (1 s default), the event is tagged as a
replay and `V18` is set to `"REPLAY"` when it's forwarded; once the
backlog is exhausted and new events are draining within that 1-second
window of their own occurrence, `V18` naturally flips back to `"LIVE"`.
Combined with `V17` visibly counting down during backlog replay, an
operator can watch a reconnection event happen on the dashboard in real
time: queue depth high + "REPLAY" showing, then depth falling to 0 and
"LIVE" reappearing.

## 6. Demonstration: fault injection and network outage

The demo sketch (`TelemetryDashboardDemo.ino`) includes a scripted
timeline (`runDemoScript()`) so both required scenarios can be observed
without needing to physically fault a cell or unplug a router on the
bench:

| Time | Action | Expected observable result |
|---|---|---|
| t ≈ 20 s | Inject a MAJOR relay fault via `faultSM.raiseFault()` | `FaultStateMachine` transitions `NORMAL/DEGRADED → FAILSAFE`; `V13`/`V14` update immediately; transition is logged (`[FAULT-SM]` line) |
| t ≈ 30 s | Force `WiFi.disconnect(true)` | `WiFiReconnectSM` detects the drop, cycles `CONNECTED → DISCONNECTED → CONNECTING → WAIT_RETRY → …`; `blynkConnected` goes false; new telemetry events keep being `emit()`-ted (cell voltages, etc.) and simply accumulate in the offline queue; `V17` (queue depth) would climb if the dashboard were still reachable to show it, and resumes reporting once reconnected |
| t ≈ 45 s | Outage window ends | The reconnect state machine (already retrying on its own backoff schedule) succeeds on its next attempt; Blynk reconnects; `drain()` begins flushing the backlog in original order at `DRAIN_MAX_PER_TICK` (5) events per tick; `V18` shows `"REPLAY"` for the backlog, then `"LIVE"` once caught up |
| t ≈ 50 s | Clear the injected relay fault | `FailSafe`'s bounded recovery-verification sequence runs (5 consecutive clean samples over ~2 s, per the companion fault state machine's design); on success, `FAILSAFE → DEGRADED → NORMAL`, each step visible on `V13` |

This single scripted run exercises both required demonstrations (fault
injection and a network outage) plus their recovery paths, and every step
produces a distinct, observable Serial log line and dashboard pin change
so it can be captured for a report or a live walkthrough.

## 7. Testing performed before hardware integration

Because the sandbox used to develop this could not run real WiFi/Blynk
hardware, the two pieces of logic that don't need it —
`WiFiReconnectSM` and `TelemetryEngine` — were compiled and run standalone
against mock hardware callbacks. This caught and fixed one real bug (the
overflow cascade in Section 2) before it ever reached a board, and
confirmed:

- the exponential backoff sequence during a simulated outage matches the
  configured `baseRetryMs`/`maxRetryMs`,
- a link drop while `CONNECTED` is detected and restarts the cycle,
- events emitted while offline are held, not dropped, until reconnection,
- draining after reconnection delivers all pending events in strictly
  increasing sequence order,
- the fixed-size queue's overflow counter is exact (16 dropped for 16
  events pushed past a 64-capacity queue, not double-counted).

The `.ino` wiring layer (real `WiFi.h`/`BlynkSimpleEsp32.h` calls) requires
the Arduino/ESP32 toolchain and a real or emulated Blynk account to build
and run, and was written to match the tested logic exactly — the state
machine and engine classes it calls are unchanged from what was verified.
