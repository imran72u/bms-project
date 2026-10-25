# Integrated BMS Safety & IoT Monitoring System

A single ESP32/Wokwi project integrating all six internship tasks into one production-oriented embedded system.

## Integrated tasks
1. Modular scalable BMS engine: weakest/strongest cells, imbalance, trend and adaptive threshold.
2. Non-blocking protection relay: hysteresis, debounce, frozen/jump/range anomaly detection and lockout.
3. Flicker-free LCD: differential line updates and non-blocking page rotation.
4. Fault state machine: NORMAL/DEGRADED/FAILSAFE/SHUTDOWN, isolated faults and bounded recovery.
5. Wi-Fi + telemetry: non-blocking reconnect, RSSI health, FIFO offline queue, event-driven Blynk routing.
6. Dashboard/diagnostics: live cell data, system state, relay, imbalance, SoC, temperature, current, trend, queue depth and alerts.

## Non-blocking guarantee
The integrated application uses `millis()` and state machines. No `delay()` is used. Blynk connection attempts use `Blynk.connect(0)` and the telemetry path is queued and throttled.

## Wokwi controls
- Cell 1-4 potentiometers: simulated cell voltages.
- Current potentiometer: 0-12 A; above the protection trip point causes relay protection.
- Temperature potentiometer: simulated pack temperature.
- FAULT button: inject/clear a major relay fault.
- CLEAR button: clear the demo relay fault.

## Blynk virtual pins
V0-V3 cells; V8/V9 weakest; V10/V11 strongest; V12 relay; V13/V14 fault state/count; V15/V16 Wi-Fi; V17 queue; V18 LIVE/REPLAY; V19 overflow; V20/V21/V22 imbalance/SoC/BMS status; V23 temperature; V24 current; V25 trend; V26 BMS status; V27 anomaly.

## Build
PlatformIO: `pio run`.
For Arduino IDE, copy `src/*` into one sketch folder and install Blynk + LiquidCrystal_I2C.

## Live dashboard
Set a real Blynk auth token before the demo. Never commit secrets. Use a local `secrets.h` or build environment variable for credentials.
