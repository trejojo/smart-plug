# Firmware Development Evolution Summary

## Scope

This summary is based on commit history from:

- `remotes/origin/feature/integrate-ayce`
- `remotes/origin/test/ade7953_calibration`

Reference date: 2026-05-26.

## Shared Foundation (Before Branch Divergence)

Both branches share the same baseline up to commit `9a6043f` (2026-05-15), after building these common capabilities:

- Project bootstrap, initial build/debug setup, and module separation.
- BLE + NVS integration and provisioning improvements.
- Wi-Fi + MQTT initial implementation and setup documentation.
- Early board bring-up (RGB, TMP, SD card, relay) and ADE initialization.
- Safety evaluation refinement in ADE flow (`d6cdfe0`).

In short, the firmware moved from initial architecture to a connected and measurable SmartPlug baseline before specialization started.

## Divergence Point

- Merge-base between both branches: `9a6043f` (2026-05-15).
- From there, each branch specialized in a different direction.

## Evolution in `feature/integrate-ayce`

Primary objective: integrate ADE telemetry, protections, and MQTT-host interaction into the production-oriented firmware path.

### Key milestones

1. `be6597c` (2026-05-18)
   - Integrated button handling for NVS credential erase into main flow.

2. `c7053ad` (2026-05-25)
   - Integrated ADE configuration with test flow while preserving waveform streaming.
   - Shift away from PZEM driver usage toward ADE-centered implementation.

3. `e2d8616` (2026-05-26)
   - Expanded MQTT payload to include richer electrical and status telemetry:
     - `vrms`, `irms`, `active_power`, `reactive_power`, `frequency`, `energy_wh`, `no_load`, `relay`, `temp`.

4. `7f2575b` (2026-05-26)
   - Fixed LED channel logic (GRB) and adjusted no-load read path.

5. `9dc57ce` (2026-05-26)
   - Moved ADE safety limits and IRQ wiring initialization into ADE driver.
   - Added writes to `OVLVL` and `OILVL` registers at driver level.

6. `1326f88` (2026-05-26)
   - Added MQTT listener + acknowledgement flow.
   - Enabled host-driven writing of ADE protection thresholds (`OVLVL`/`OILVL`) through MQTT-triggered path.

### Technical impact (vs `test/ade7953_calibration`)

- 9 files changed, 767 insertions, 613 deletions.
- Main concentration in:
  - `main/main.c`
  - `main/module_ade7953.c`
  - `main/module_ade7953.h`
  - `main/module_mqtt.c`
  - `main/module_mqtt.h`
- Removal/de-emphasis of `components/pzem-driver` code in this line of evolution.

## Evolution in `test/ade7953_calibration`

Primary objective: improve ADE calibration methodology, reproducibility, and defaults for measurement correctness.

### Key milestones

1. `f0006a4` (2026-05-15)
   - Simplified `main.c` to support focused ADE testing.

2. `cb8b333` (2026-05-15)
   - Introduced `Calibration/ade_calibration.py` and related workspace/readme support.

3. `40b4b92` (2026-05-18)
   - Fixed ADE latch and timeout behaviors.

4. `71f27b7` (2026-05-18)
   - Added 7 kHz waveform reconstruction option (interrupt-based).

5. `3d9ad34` (2026-05-25)
   - Reworked calibration Python flow and added waveform reconstruction path.
   - Added multiple calibration result CSV artifacts.

6. `a4159a8` (2026-05-25)
   - Added hardcoded calibration defaults and automatic application path.
   - Updated read-measurement behavior.
   - Added NVS calibration handling (`module_nvs.c/.h` changes).

### Technical impact (vs `feature/integrate-ayce`)

- 13 files changed, 1512 insertions, 281 deletions.
- Strong focus on:
  - `Calibration/ade_calibration.py`
  - Calibration CSV datasets
  - `main/main.c`
  - `main/module_nvs.c`
  - `main/module_nvs.h`

## Firmware Evolution Narrative (High-Level)

The development evolved in three clear phases:

1. Platform foundation and connectivity
   - System architecture, provisioning, BLE/Wi-Fi/MQTT baseline, and board bring-up.

2. ADE metering enablement
   - ADE integration and safety logic maturation.

3. Branch specialization
   - `feature/integrate-ayce`: production integration focus (runtime telemetry + host-driven control via MQTT + protection register management).
   - `test/ade7953_calibration`: measurement-quality focus (calibration tooling, waveform reconstruction, and calibration defaults persistence path).

## Current Practical Interpretation

- `feature/integrate-ayce` represents the stronger path for integrated runtime firmware behavior and remote operation workflow.
- `test/ade7953_calibration` represents the stronger path for metrology quality and calibration process maturity.

Together, they indicate a firmware trajectory from "functional connectivity" to "electrical accuracy + operational control".