# Flicker-Free LCD Display Engine — Design Report

## 1. Problem summary

The task requires an LCD driver that:

1. Redraws only the characters that actually changed (no full-screen clear).
2. Rotates automatically among ≥3 informational pages using a non-blocking timer.
3. Immediately overrides the normal display with a dedicated fault screen during
   a critical fault, and holds that screen until the fault is explicitly cleared.
4. Uses a display refresh interval that is justified against LCD hardware
   characteristics and the importance/urgency of the data being shown.

The reference implementation (`FlickerFreeLCD.ino`) targets a 16×2 HD44780
character LCD driven over I2C through a PCF8574 backpack — the most common
setup for this kind of internship assignment — but the architecture (buffer
diffing, dual non-blocking timers, latched fault override) is display-agnostic
and ports directly to a 20×4 LCD, a TFT with a character grid, or a
segment/OLED panel with only the low-level `commitBufferToLCD()` routine
needing to change.

## 2. Why `lcd.clear()` causes visible flicker

Two independent effects combine to make naive redraw loops flicker:

- **Electrically**, `lcd.clear()` writes every DDRAM cell to the blank
  character and resets the cursor, so the whole visible area goes blank for
  one refresh cycle before the new content is written back in. On a human
  time scale this blank-then-redraw is directly visible as a flash, even
  though the panel itself has no true "vsync" concept the way a CRT/LCD
  monitor does.
- **Electronically**, the HD44780 command set specifies that `Clear Display`
  and `Return Home` are the two slowest instructions the controller
  supports — on the order of **1.6–1.64 ms** of internal busy time (vs.
  roughly **37–43 µs** for a normal character write or cursor move). Calling
  `clear()` on every refresh tick therefore also wastes almost two orders of
  magnitude more bus time than necessary.

The fix implemented here is **differential rendering**: every page is first
composed into an in-RAM buffer (`backBuffer`), which is compared cell-by-cell
against a shadow copy of whatever is currently on the physical screen
(`frontBuffer`). Only the cells that differ get a `setCursor()` + `write()`
call, and adjacent changed cells are coalesced into a single run to minimize
I2C transactions. The LCD's `clear()` command is never called during normal
operation — the very first draw after power-up is the only place a full
paint happens, and even that goes through the same per-cell path rather than
the hardware clear command.

## 3. Non-blocking page rotation

`delay()` is avoided entirely. Two independent `millis()`-based timers drive
the system inside `loop()`:

| Timer | Interval | Purpose |
|---|---|---|
| Refresh timer | 250 ms | Re-samples the active page's data source and redraws only the changed cells |
| Rotation timer | 3000 ms | Switches to the next page (Battery → System → Telemetry → …) |

Because both timers are just `if (now - last >= interval)` checks, the loop
never stalls, which is what allows the fault check described below to run on
literally every pass of `loop()` regardless of where the rotation/refresh
cycle currently is.

## 4. Refresh interval justification

Two different "refresh" concepts are deliberately kept separate, because
they are driven by different constraints:

### 4.1 Data/redraw refresh — 250 ms

This governs how often a page's *contents* are re-sampled and (if changed)
redrawn.

- **Hardware ceiling:** a single character write takes on the order of
  40 µs; even redrawing all 32 cells of a 16×2 display back-to-back is well
  under 2 ms of controller time. The panel could physically be updated far
  faster than 250 ms — the limiting factor is not the LCD.
- **I2C bus overhead:** the PCF8574 backpack multiplexes each 8-bit LCD
  transaction into 4-bit nibbles sent as separate I2C byte writes, and each
  I2C transaction carries its own start/stop/ack overhead. At typical
  100 kHz I2C, redrawing a full 32-character screen this way can take a few
  milliseconds — trivial compared to 250 ms, leaving comfortable headroom
  for other loop work (sensor sampling, fault checks, button polling).
  Because only *changed* cells are ever written (Section 2), most refresh
  ticks touch only a handful of cells and cost microseconds, not
  milliseconds.
- **Human visual perception:** the human eye cannot usefully resolve
  character-level numeric updates faster than roughly 5–10 updates per
  second — beyond that, faster updates just look like jitter rather than
  conveying more information. 250 ms (4 Hz) sits comfortably below the
  point of perceptible flicker/jitter for numeric values while still
  feeling "live" for telemetry such as speed or temperature.
- **Data importance:** telemetry and system state are not so
  latency-critical that they require millisecond-scale updates on a 2-line
  character display — a 250 ms lag between a real-world change and its
  on-screen reflection is imperceptible in normal operation.

250 ms was chosen as a balance point: fast enough that the display never
feels "stuck," slow enough that it doesn't waste bus cycles or produce
visually noisy digit-flicker on fast-changing values.

### 4.2 Page rotation interval — 3000 ms

This governs how long each *page* stays on screen before switching to the
next one.

- **Reading speed:** a 16×2 display holds at most ~32 characters — roughly
  a short phrase and a number. Human reading/comprehension of a short
  two-line readout comfortably fits within 2–3 seconds; much shorter and a
  user glancing at the panel may miss a page entirely, much longer and the
  display feels unresponsive when the user is actively trying to cycle
  through information.
- **Priority ordering:** because rotation is slow (3 s) relative to
  refresh (250 ms), values *within* the currently displayed page still stay
  live between rotations — the user isn't stuck looking at stale telemetry
  just because the page itself hasn't switched.
- **Fault interaction:** rotation must be slow enough that it never
  competes with fault visibility. Since the fault check runs every loop
  pass independent of both timers, a fault is displayed well within one
  refresh tick (≤250 ms, in practice next-loop-iteration speed), regardless
  of where the 3-second rotation cycle currently is.

3000 ms was chosen as the rotation interval as a standard, easily-justified
"glanceable" value common in embedded HMI design (many commercial dashboard
and instrument-cluster displays use 2–5 s page-hold times for non-critical
status pages).

## 5. Fault override behavior

`checkForCriticalFault()` is called unconditionally at the top of every
`loop()` iteration, *before* the refresh/rotation timers are even
evaluated. If it detects a critical condition:

1. `raiseFault()` latches `faultActive = true`, stores a short fault
   message, and **immediately** renders and commits the fault page — it does
   not wait for the next 250 ms refresh tick.
2. While `faultActive` is true, the normal rotation/refresh scheduling
   branch is skipped entirely (`return` after the fault-specific redraw
   block), so the fault screen fully owns the display — no interleaving
   with the battery/system/telemetry pages is possible.
3. The fault screen is still redrawn on the same 250 ms cadence in case the
   underlying fault message needs live data (e.g., an evolving voltage
   reading), but it is drawn through the exact same diffing path, so it
   does not flicker either.
4. The fault is only cleared by an explicit action — `clearFault()`,
   wired here to a debounced physical acknowledge button — never by a
   timeout. This matches real fault-handling practice: a transient
   condition clearing itself should not silently erase the operator's
   evidence that a fault occurred.

## 6. Summary of chosen constants

| Constant | Value | Rationale |
|---|---|---|
| `REFRESH_INTERVAL_MS` | 250 ms | Fast enough to feel live, slow enough to avoid digit jitter and unnecessary bus traffic |
| `ROTATION_INTERVAL_MS` | 3000 ms | Matches typical human short-text reading time on embedded HMI panels |
| `DEBOUNCE_MS` | 50 ms | Standard mechanical-switch debounce window |
| LCD clear command usage | Never (except implicit first paint) | Avoids the ~1.6 ms controller blank cycle that causes visible flicker |
