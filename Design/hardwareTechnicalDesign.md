# Hardware Technical Design
## PanelMeterClock

---

## 1. Document Control

### 1.1 Identification

| Field | Value |
|-------|-------|
| Document title | Hardware Technical Design — PanelMeterClock |
| Document ID | PMC-HTD-001 |
| Version | 0.1 (draft) |
| Date | 2026-04-29 |
| Author | Remko Welling |
| Status | Draft — under review |

### 1.2 Revision History

| Version | Date | Author | Change Summary |
|---------|------|--------|----------------|
| 0.1 | 2026-04-29 | Remko Welling | Initial draft; hardware design content migrated from PMC-FRS-001 and supplemented with circuit analysis |

### 1.3 Relationship to Other Documents

| Document | Role |
|----------|------|
| PMC-FRS-001 Functional Requirements Specification | Defines *what* the system shall do |
| PMC-STD-001 Software Technical Design | Defines firmware architecture and module design |
| This document (PMC-HTD-001) | Defines *how* the hardware is designed and wired |

---

## 2. Target Hardware Platform

### 2.1 Microcontroller Module — WEMOS LOLIN S3

| Attribute | Value |
|-----------|-------|
| Module | WEMOS LOLIN S3 |
| SoC | ESP32-S3-WROOM-1 |
| CPU | Xtensa LX7 dual-core, up to 240 MHz |
| Flash | 16 MB QSPI |
| PSRAM | 8 MB (WROOM-1 variant) |
| SRAM | 512 KB |
| WiFi | 802.11b/g/n 2.4 GHz |
| GPIO voltage | 3.3 V (not 5 V-tolerant) |
| USB | Native USB via USB-CDC (GPIO 19/20) + CH340 chip on UART0 |
| Operating supply | 3.3 V or 5 V via USB |

### 2.2 Available GPIO on LOLIN S3

The table below lists all GPIO pins available for application use after reserving the fixed-function pins.

| GPIO | Reserved for | Direction | Notes |
|------|-------------|-----------|-------|
| 0 | Strapping / Boot | — | Do not use; affects boot mode |
| 3 | Strapping | — | Do not use |
| 19 | USB D− (UART0 / USB-CDC) | — | Debug serial |
| 20 | USB D+ (UART0 / USB-CDC) | — | Debug serial |
| 26–32 | SPI flash (internal) | — | Not accessible |
| 43 | UART0 TX (CH340) | Output | Debug serial TX |
| 44 | UART0 RX (CH340) | Input | Debug serial RX |
| 45 | Strapping | — | Avoid |
| 46 | Strapping / input-only | Input only | No internal pull |
| All others | Application use | I/O | 3.3 V max |

---

## 3. GPIO Pin Assignment

All application GPIO assignments are listed below. This table is the single authoritative source for pin allocation; firmware constants in `config.h` (PMC-STD-001 §7) shall match these values.

| GPIO | Signal | Direction | Peripheral | Connected to | Requirement |
|------|--------|-----------|------------|--------------|-------------|
| 10 | GNSS 1PPS | Input | GPIO interrupt | GNSS module 1PPS output | IC-HW-003, TBD-001 |
| 15 | PWM Hours | Output | LEDC_CHANNEL_0 / LEDC_TIMER_0 | RC filter → Hours meter | IC-HW-001 |
| 16 | PWM Minutes | Output | LEDC_CHANNEL_1 / LEDC_TIMER_1 | RC filter → Minutes meter | IC-HW-001 |
| 17 | PWM Seconds | Output | LEDC_CHANNEL_2 / LEDC_TIMER_2 | RC filter → Seconds meter | IC-HW-001 |
| 18 | GNSS UART RX | Input | UART1 (Serial1) | GNSS module TX | IC-HW-004, TBD-002 |
| 19 | USB D− | — | USB-CDC / UART0 | USB connector | Fixed |
| 20 | USB D+ | — | USB-CDC / UART0 | USB connector | Fixed |
| 21 | GNSS UART TX | Output | UART1 (Serial1) | GNSS module RX | IC-HW-004, TBD-002 |
| 43 | Debug TX | Output | UART0 / CH340 | USB-serial chip | Fixed |
| 44 | Debug RX | Input | UART0 / CH340 | USB-serial chip | Fixed |

GPIO 10 is configured as input-only with no internal pull resistor (IC-HW-003). All PWM outputs are push-pull 3.3 V. GPIO 21 (GNSS TX) may be left unconnected if the GNSS module requires no runtime configuration.

---

## 4. Panel Meter Drive Circuit

### 4.1 Panel Meter — Siemens 1604P

| Parameter | Value |
|-----------|-------|
| Type | Moving-coil (D'Arsonval) DC panel meter |
| Full-scale deflection (FSD) voltage | 1 V (across meter terminals, factory) |
| Original series resistor | Present in factory configuration; limits current to rated FSD |
| Modified full-scale voltage | 3 V (after series resistor substitution — see §4.3) |
| Scale range | 0 – FSD (linear, continuous) |

### 4.2 Circuit Overview

Each of the three meter channels follows the same topology:

```
ESP32-S3 GPIO ──┬── R_filter (1 kΩ) ──┬── R_series ──[ Meter coil ]── GND
                │                     │
               (PWM)               C_filter
                                   (10 µF)
                                      │
                                     GND
```

The RC low-pass filter converts the PWM waveform to a quasi-DC voltage. The series resistor limits the current through the meter coil so that the maximum filter output voltage (3 V) causes exactly full-scale deflection.

### 4.3 Series Resistor Modification

The factory series resistor is sized for a 1 V drive source. It must be replaced with a higher value to adapt the meter to the 3 V PWM output.

**Measured values (Siemens 1604P, this build):**

| Parameter | Symbol | Measured value |
|-----------|--------|---------------|
| Full-scale current | I_FSD | 1.000 mA |
| Full-scale voltage (coil terminals) | V_FSD | 82.2 mV |
| Coil resistance (derived) | R_coil | 82.2 Ω |

**Formula:**

The DC current path is GPIO → R_filter → R_series → coil → GND. R_filter (1 kΩ) is in series at DC and must be included:

```
R_series = V_drive / I_FSD − R_filter − R_coil
```

**Calculation:**

```
R_series = 3.00 V / 1.000 mA − 1 000 Ω − 82.2 Ω
         = 3 000 − 1 000 − 82.2
         = 1 917.8 Ω
```

Nearest E24 standard value: **2.0 kΩ** (next value up, safe — avoids over-deflection).  
With 2.0 kΩ fitted: I = 3.00 V / (1 000 + 2 000 + 82.2) Ω = **0.973 mA = 97.3 % FSD**.  
The 2.7 % shortfall is trimmed by the per-meter `full_scale` NVS calibration value (FR-DSP-011).

**Note:** R_coil varies between individual meters even of the same type. Measure each meter and calculate its series resistor individually. Use a close-tolerance resistor (±1 % metal film) to ensure calibration accuracy is not limited by component tolerance.

**Design point:** The firmware uses a full-scale duty of 232/255, corresponding to:

```
V_out_max = (232 / 255) × 3.3 V = 3.00 V
```

The 3.3 V GPIO maximum is deliberately not used as the full-scale point. This provides a ≈ 0.3 V safety margin and ensures the meter never reaches mechanical end-stop from a firmware glitch. Firmware constants: `PWM_FULL_SCALE_DUTY = 232`, `V_DRIVE_FULL_SCALE = 3.0 V`.

### 4.4 RC Low-Pass Filter Design

One RC filter is fitted per channel. Values and rationale:

| Component | Value | Tolerance | Notes |
|-----------|-------|-----------|-------|
| R_filter | 1 kΩ | ±5 % (±1 % preferred) | Carbon or metal film; rated ≥ 100 mW |
| C_filter | 10 µF | ±20 % electrolytic | Positive terminal toward GPIO output (higher potential side) |

**Cutoff frequency:**

```
f_c = 1 / (2π × R × C) = 1 / (2π × 1 000 × 10 × 10⁻⁶) ≈ 15.9 Hz
```

**Attenuation at PWM frequency (80 kHz):**

```
A = 20 × log₁₀(f_c / f_PWM) = 20 × log₁₀(15.9 / 80 000) = −74 dB
```

At −74 dB the PWM ripple reaching the meter is negligible (< 0.2 mV peak). Residual mechanical damping of the moving-coil meter suppresses any remaining ripple further.

**DC gain:** 0 dB (unity). The filter introduces no attenuation at DC; the output voltage at any steady duty cycle equals `(duty / 255) × 3.3 V` within the accuracy of the resistor and capacitor tolerances.

**Settling time:** The RC time constant τ = R × C = 10 ms. Full settling to < 1 % error requires ≈ 5τ = 50 ms. The panel meter's mechanical inertia (settling time > 500 ms) dominates entirely, so the RC settling time is not the limiting factor.

### 4.5 PWM-to-Voltage-to-Deflection Mapping

| Duty (decimal) | V_out | Meter deflection |
|----------------|-------|-----------------|
| 0 | 0.00 V | 0 % (zero) |
| 64 | 0.83 V | 25 % |
| 128 | 1.66 V | 50 % |
| 192 | 2.49 V | 75 % |
| 232 | 3.00 V | 100 % (FSD) |
| 255 | 3.30 V | > FSD — **do not use** |

The firmware clamps duty to [0, 232] at the display layer. Firmware factory defaults: `zero_offset = 0`, `full_scale = 232` (FR-DSP-014).

---

## 5. LEDC PWM Configuration

The ESP32-S3 LEDC peripheral generates all three PWM signals independently.

| Parameter | Value | Notes |
|-----------|-------|-------|
| Clock source | APB clock (80 MHz) | Stable; not affected by CPU frequency scaling |
| PWM frequency | 80 000 Hz | Ultrasonic; inaudible and above panel meter mechanical response |
| Resolution | 8 bits (256 steps) | Gives 256 duty positions; adequate for meter accuracy |
| Full-scale duty register value | 232 | Corresponds to 3.00 V output |

**Channel and timer assignment:**

| Meter | GPIO | LEDC channel | LEDC timer |
|-------|------|-------------|------------|
| Hours | 15 | LEDC_CHANNEL_0 | LEDC_TIMER_0 |
| Minutes | 16 | LEDC_CHANNEL_1 | LEDC_TIMER_1 |
| Seconds | 17 | LEDC_CHANNEL_2 | LEDC_TIMER_2 |

Each meter uses a dedicated timer so that the PWM frequency of one channel can be changed independently without affecting the others (used during development/calibration).

**Frequency validation:** The 80 kHz operating frequency was selected and verified in `Research/PWMTest.md`. Key criteria: ultrasonic (> 20 kHz), above the RC filter corner frequency by > 70 dB, and achievable with integer divider from the 80 MHz APB clock (80 MHz / 80 kHz = 1 000, exact).

---

## 6. GNSS Hardware Interface

### 6.0 Module — Quectel L76-M33

| Parameter | Value |
|-----------|-------|
| Part number | Quectel L76-M33 |
| Constellations | GPS (L1 C/A), GLONASS (L1OF), Galileo (E1B/C), BeiDou (B1I), QZSS (L1 C/A) |
| Supply voltage (VCC) | 3.3 V (2.8 V – 3.6 V) |
| Backup voltage (VBAT) | 3.0 V nominal (maintains RTC and almanac across power cycles) |
| UART default baud rate | 9 600 baud, 8-N-1 |
| UART configurable baud rates | 4 800 / 9 600 / 14 400 / 19 200 / 38 400 / 57 600 / 115 200 via PMTK command |
| NMEA sentences | $GPRMC, $GPGGA, $GPGSV, $GPGSA, $GNRMC, $GNGGA (and others) |
| 1PPS output voltage | 3.3 V logic (active HIGH, configurable pulse width, default 100 ms) |
| Antenna connector | U.FL (IPEX) on-module |
| Acquisition current | ≈ 18 mA (typical) |
| Tracking current | ≈ 15 mA (typical) |
| Standby current | ≈ 1 mA |
| Cold-start TTFF | ≈ 30 s (open sky, no almanac) |
| Hot-start TTFF | ≈ 1 s (valid almanac and ephemeris in VBAT-retained RAM) |
| Tracking sensitivity | −165 dBm |
| Package | LCC, 10.1 × 9.7 × 2.5 mm |
| Reference document | `Documentation/` — Quectel L76-M33 Hardware Design Guide and Product Specification |

### 6.1 Signal Overview

| Signal | L76-M33 pin | ESP32-S3 GPIO | Level | Notes |
|--------|------------|---------------|-------|-------|
| UART RX (ESP ← GNSS) | TXD | 18 | 3.3 V | NMEA sentence output |
| UART TX (ESP → GNSS) | RXD | 21 | 3.3 V | PMTK command input |
| 1PPS (ESP ← GNSS) | 1PPS | 10 | 3.3 V | Rising-edge interrupt; 100 ms pulse (default) |
| Module power | VCC | — | 3.3 V | Supplied from 3.3 V rail |
| Backup power | VBAT | — | 3.0 V | Coin cell or supercapacitor (see §6.6) |

### 6.2 Level Shifting

The L76-M33 operates natively at 3.3 V. All UART and 1PPS signals are directly compatible with ESP32-S3 GPIO logic levels. **No level shifter is required.**

### 6.3 UART Configuration

| Parameter | Value | Configurable |
|-----------|-------|-------------|
| Peripheral | UART1 (`Serial1`) | No |
| RX GPIO | 18 | No |
| TX GPIO | 21 | No |
| Baud rate | 9 600 (default) | Yes — NVS key `clock/gnss_baud`; change applied via PMTK set-baud command then firmware reconnect |
| Frame format | 8-N-1 | No |

At 9 600 baud a complete `$GPRMC` sentence (≈ 70 bytes) arrives in ≈ 73 ms, well within the 1-second tick budget. To change the baud rate at runtime the firmware sends `$PMTK251,<baud>*<checksum><CR><LF>` before reopening `Serial1` at the new rate.

### 6.4 1PPS Signal

The L76-M33 1PPS output is 3.3 V active-HIGH with a default pulse width of 100 ms. GPIO 10 is configured as a rising-edge interrupt with no internal pull resistor. The ISR has a latency budget of < 10 µs (hardware interrupt response on LX7 core). The interrupt is enabled only while `gnss_task` is running (GNSS enabled).

### 6.5 Antenna

The L76-M33 has an on-module **U.FL (IPEX)** antenna connector. A short U.FL-to-SMA pigtail cable (W1, ≈ 100 mm) connects the module to the **SMA female chassis-mount connector (J1)** on the enclosure panel. An external active or passive GNSS antenna with SMA male plug connects to J1.

Active antennas (with built-in LNA) are recommended for enclosures with limited sky view. The L76-M33 supports active antennas directly; no external LNA bias circuit is required.

### 6.6 Backup Power (VBAT)

The VBAT pin maintains the L76-M33 internal RTC and almanac/ephemeris RAM across main-power cycles, enabling hot-start acquisition (≈ 1 s TTFF vs. ≈ 30 s cold start).

| Option | Component | Notes |
|--------|-----------|-------|
| Coin cell (recommended) | MS621FE rechargeable LiMnO₂, 3.0 V | Self-contained; no charge circuit needed; ≈ 2–3 year lifetime at VBAT quiescent current |
| Supercapacitor | 0.1 F, 3.3 V | Maintains data for several hours after power-off; no battery management |
| No backup | — | Cold start only (≈ 30 s TTFF); simpler circuit |

Connect VBAT through a Schottky diode (e.g. BAT54, V_f ≈ 0.3 V) from the 3.3 V rail if the coin cell or supercapacitor is omitted, to prevent VBAT floating.

---

## 7. Serial Debug Interface

| Parameter | Value |
|-----------|-------|
| Interface | UART0, also exposed via CH340 USB-serial chip |
| USB connector | USB-C on LOLIN S3 board |
| Baud rate | 115 200 baud, 8-N-1 |
| GPIO (UART0 TX) | 43 |
| GPIO (UART0 RX) | 44 |
| Arduino object | `Serial0` (not `Serial` — see PMC-STD-001, serial UART mapping note) |

`Serial` in the ESP32-S3 Arduino core defaults to the native USB-CDC port (HWCDC), not UART0. All firmware debug output uses `Serial0` explicitly to target the CH340 chip, which is more reliably supported by OS serial drivers.

---

## 8. Power Supply

| Parameter | Value | Notes |
|-----------|-------|-------|
| Input supply | 5 V via USB-C | Provided by the LOLIN S3 board regulator |
| 3.3 V rail | On-board LDO regulator | Supplies ESP32-S3 and GPIO outputs |
| GPIO output current per pin | 40 mA maximum | ESP32-S3 absolute maximum |
| Total GPIO current budget | 1 200 mA maximum (sum of all outputs) | Practical limit lower due to LDO rating |
| Panel meter coil current per channel | ≤ I_FSD (≈ 1 mA typical) | Negligible relative to GPIO budget |
| RC filter current per channel | V_out / R_filter ≈ 3 V / 1 kΩ = 3 mA max | Dominates over meter coil current |
| Total current from GPIO pins (3 channels) | ≤ 10 mA | Well within limits |
| GNSS module (L76-M33), acquisition | ≈ 18 mA | Supplied from 3.3 V rail |
| GNSS module (L76-M33), tracking | ≈ 15 mA | Typical steady-state |
| Total estimated system current | ≤ 300 mA | ESP32-S3 WiFi active (≈ 240 mA peak) dominates |

The L76-M33 adds ≤ 18 mA to the 3.3 V rail. The LOLIN S3 on-board LDO regulator is rated for at least 500 mA output; total system current remains well within that limit.

---

## 9. Component List (Partial)

This list covers the signal conditioning and display subsystem. Connectors, PCB, and enclosure are out of scope.

| Qty | Reference | Description | Value / Part number |
|-----|-----------|-------------|-------------------|
| 1 | U1 | Microcontroller module | WEMOS LOLIN S3 (ESP32-S3-WROOM-1, 16 MB flash) |
| 3 | M1–M3 | Panel meter | Siemens 1604P, 1 V FSD, DC |
| 3 | R1–R3 | RC filter resistor | 1 kΩ ±1 %, metal film, 250 mW |
| 3 | C1–C3 | RC filter capacitor | 10 µF, 16 V, electrolytic, radial |
| 3 | R4–R6 | Panel meter series resistor | 2.0 kΩ ±1 %, metal film (per §4.3; measured I_FSD = 1 mA, R_coil = 82.2 Ω) |
| 1 | U2 | GNSS receiver module | Quectel L76-M33 (multi-constellation, 3.3 V, NMEA 0183, 1PPS, U.FL) |
| 1 | BT1 | GNSS backup battery | MS621FE rechargeable 3.0 V LiMnO₂ coin cell (or 0.1 F supercapacitor) |
| 1 | W1 | GNSS antenna pigtail | U.FL male to SMA male, ≈ 100 mm |
| 1 | J1 | GNSS antenna connector | SMA female, chassis mount |

---

## 10. Open Hardware Issues

| ID | Item | Impact | Status |
|----|------|--------|--------|
| HW-001 | GNSS module selection | ✅ Resolved — Quectel L76-M33; 3.3 V, no level shifter required, U.FL antenna connector, default 9 600 baud (see §6) |
| HW-002 | Series resistor values R4–R6 | ✅ Resolved — I_FSD = 1 mA, V_FSD = 82.2 mV, R_coil = 82.2 Ω → R_series = 2.0 kΩ E24 (see §4.3) |
| HW-004 | GNSS antenna connector | ✅ Resolved — SMA female chassis connector |
