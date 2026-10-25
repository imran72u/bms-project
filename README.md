# Integrated BMS Safety & IoT Monitoring System

Internship final project. One ESP32 system that combines six tasks:
battery cell monitoring, protection relay, flicker-free LCD,
fault state machine, Wi-Fi reconnect with telemetry queue, and a Blynk dashboard.

## Live simulation (Wokwi)
https://wokwi.com/projects/478855251308721153

## Project files
All code, reports and diagrams are in the folder `integrated_bms_safety_system`.
- `src/` : firmware source code
- `docs/` : reports, architecture and workflow diagrams
- `dashboard/` : dashboard code

## How to run
Open the Wokwi link above and press the green Play button.
Use the knobs to change cell voltage, current and temperature.
Use the red FAULT button and green CLEAR button to test the fault logic.
