# Functional Requirements Specification
## PanelMeterClock Firmware

---

## 1. Document Control

### 1.1 Identification

| Field | Value |
|-------|-------|
| Document title | Functional Requirements Specification — PanelMeterClock Firmware |
| Document ID | PMC-FRS-001 |
| Version | 0.1 (draft) |
| Date | 2026-04-29 |
| Author | Remko Welling |
| Status | Draft — under review |

### 1.2 Revision History

| Version | Date | Author | Change Summary |
|---------|------|--------|----------------|
| 0.1 | 2026-04-29 | Remko Welling | Initial draft |

### 1.3 Approvals

| Role | Name | Signature | Date |
|------|------|-----------|------|
| Author | Remko Welling | | |
| Reviewer | | | |

### 1.4 Distribution

| Recipient | Role |
|-----------|------|
| Remko Welling | Author / firmware developer |

---

## 2. Introduction

### 2.1 Purpose

This document defines all functional and non-functional requirements for the PanelMeterClock firmware running on the WEMOS LOLIN S3 (ESP32-S3-WROOM-1). It provides the authoritative specification against which firmware shall be designed, implemented, and tested.

### 2.2 Scope

**In scope:**
- Firmware running on the ESP32-S3 target
- Three-meter display subsystem (hours, minutes, seconds)
- Boot phase state machine and time-source management
- NTP synchronisation and WiFi connectivity
- GNSS receiver integration and 1PPS tick discipline
- Automatic Daylight Saving Time (DST) detection and transition
- Embedded web GUI (calibration, configuration, status, FOTA)
- Firmware Over The Air (FOTA) with asymmetric-key authentication

**Out of scope:**
- PCB schematic and layout
- Mechanical enclosure and panel meter face design
- NTP server infrastructure
- GNSS hardware procurement and wiring
- HTTPS implementation (deferred; see Appendix F)

### 2.3 Intended Audience

- Firmware developer(s)
- Hardware designer
- Test engineer
- Project owner

### 2.4 How to Read This Document

Sections 6 through 13 contain numbered requirements in the form `<Type>-<Domain>-<NNN>` (see Section 3 for the numbering scheme). Each requirement is atomic and independently testable.

Section 5 provides a system-level overview with priority hierarchies. Appendix B contains a complete boot-phase state-transition table. Appendix F tracks all open issues and TBD items.

---

## 3. Definitions, Acronyms, and Abbreviations

| Term | Definition |
|------|------------|
| 1PPS | One Pulse Per Second — a precise timing signal, typically from a GNSS receiver, used as a hardware tick source |
| AP mode | Access Point mode — the ESP32-S3 hosts its own WiFi network for client devices to connect to |
| DST | Daylight Saving Time — the seasonal clock adjustment applied in many jurisdictions |
| Duty cycle | The fraction of one PWM period during which the output signal is high, expressed as an 8-bit integer (0 = 0 %, 255 = 100 %) |
| FOTA | Firmware Over The Air — uploading and applying new firmware via the embedded web GUI without physical access |
| FreeRTOS | Real-time operating system layer provided by ESP-IDF / the Arduino core for the ESP32 |
| FSD | Full-Scale Deflection — the maximum needle position on a panel meter, corresponding to the maximum rated input |
| GNSS | Global Navigation Satellite System (encompasses GPS, GLONASS, Galileo, BeiDou, etc.) |
| IP geolocation | Inferring geographic location (latitude / longitude / timezone) from a device's public IP address by querying an external web service |
| LEDC | LED Control peripheral on the ESP32-S3; used in this project as a general-purpose high-frequency PWM generator |
| NMEA 0183 | A standard ASCII sentence protocol for GNSS receivers (e.g., $GPRMC, $GNRMC) |
| NTP | Network Time Protocol (NTPv4, RFC 5905) — used to synchronise the device clock over the internet |
| NVS | Non-Volatile Storage — the ESP-IDF key-value store in flash memory that persists across power cycles |
| OTA | Over The Air — generic term for wireless firmware updates; used here specifically for FOTA |
| Phase 1 / 2 / 3 | Named phases of the boot sequence as defined in Section 6 |
| PWM | Pulse Width Modulation — a technique for generating an analogue voltage from a digital output by varying duty cycle |
| RC filter | Resistor-capacitor low-pass filter; converts a PWM signal to a quasi-DC voltage proportional to duty cycle |
| STA mode | Station mode — the ESP32-S3 connects to an existing WiFi access point |
| Stratum | NTP hierarchy level; stratum 1 = directly connected to a reference clock; higher stratum = further removed |
| UTC | Coordinated Universal Time — the global time standard to which local time offsets and DST are applied |

---

## 4. References

### 4.1 Hardware Datasheets

| Document | Location |
|----------|----------|
| ESP32-S3-WROOM-1 Datasheet | `Documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf` |
| ESP32-S3 Technical Reference Manual | `Documentation/esp32-s3_technical_reference_manual_en.pdf` |
| WEMOS LOLIN S3 Schematic | `Documentation/sch_s3_v1.0.0.pdf` |
| WEMOS LOLIN S3 Dimensions | `Documentation/dim_s3_v1.0.0.pdf` |
| Siemens 1604P Panel Meter (DC 10.00 GF024T005) | `Documentation/` — Siemens product documentation |
| Quectel L76-M33 GNSS Module | `Documentation/` — Quectel L76-M33 Hardware Design Guide and Product Specification |

### 4.2 Software and Protocol Standards

| Document | Reference |
|----------|-----------|
| Network Time Protocol v4 | IETF RFC 5905 |
| NMEA 0183 GNSS Sentence Standard | NMEA 0183 v4.11 |
| IANA Time Zone Database | https://www.iana.org/time-zones |
| ESP-IDF LEDC Peripheral Guide | Espressif ESP-IDF Programming Guide, LEDC chapter |
| ESP-IDF OTA Update Guide | Espressif ESP-IDF Programming Guide, OTA chapter |

### 4.3 Internal Project Documents

| Document | Path |
|----------|------|
| Hardware Technical Design | `Design/hardwareTechnicalDesign.md` |
| Software Technical Design | `Design/softwareTechnicalDesign.md` |
| PWM Driver Design and API | `Research/PWMDriver.md` |
| PWM + RC Filter Verification Plan | `Research/PWMTest.md` |
| Design Trade-off Notes | `Design/notes.md` |

---

## 5. System Overview

### 5.1 Product Description

PanelMeterClock is a wall clock that displays the current **local time** on three Siemens 1604P moving-coil panel meters driven by PWM signals from an ESP32-S3-WROOM-1. Each PWM signal passes through a dedicated RC low-pass filter to produce a quasi-DC voltage proportional to the local time value. The firmware runs on FreeRTOS, provided by the ESP-IDF Arduino core. Circuit design details are in PMC-HTD-001.

| Meter | Displayed value | Scale |
|-------|-----------------|-------|
| Hours | Hour of day | 0 – 24 |
| Minutes | Minute of hour | 0 – 60 |
| Seconds | Second of minute | 0 – 60 |

### 5.2 System Context

```
┌───────────────────────────────────────────────────────────┐
│                      ESP32-S3 Firmware                    │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                 │
│  │ Hours    │  │ Minutes  │  │ Seconds  │  Display task   │
│  │ PWM ch0  │  │ PWM ch1  │  │ PWM ch2  │                 │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘                 │
│       │             │             │                       │
│  RC filter     RC filter      RC filter                   │
│       │             │             │                       │
└───────┼─────────────┼─────────────┼───────────────────────┘
        │             │             │
   ┌────▼──┐     ┌────▼──┐      ┌───▼───┐
   │ Meter │     │ Meter │      │ Meter │
   │ Hours │     │  Min  │      │  Sec  │
   └───────┘     └───────┘      └───────┘

   ┌──────────────┐     ┌────────────┐     ┌─────────────┐
   │ GNSS Receiver│     │ NTP Server │     │ Web Browser │
   │ (optional)   │     │ (internet) │     │ (user)      │
   │  UART + 1PPS │     │ UDP / 123  │     │ HTTP / 80   │
   └──────┬───────┘     └─────┬──────┘     └──────┬──────┘
          │                   │                   │
          └──────── WiFi / UART ──────────────────┘
                         ESP32-S3
```

### 5.3 Operating Modes Summary

| Mode | Entry condition | Active time sources | Web GUI |
|------|----------------|---------------------|---------|
| Phase 1 — Free-running | Power-on | Internal timer (1 Hz software tick) | Available (no WiFi, AP on) |
| Phase 2 — NTP | Phase 1 active; WiFi credentials stored | Internal timer + NTP discipline | Available when WiFi connected after timeout AP is made active |
| Phase 3 — GNSS | Phase 2 active; GNSS enabled | GNSS 1PPS (preferred) or NTP synthesized | Available |
| Steady-state | Any phase completes initial sync | Best available source | Always available |
| AP mode | STA connection unavailable | Free-running or last synced time | Available on AP |

### 5.4 Time Source Priority Hierarchy

The firmware arbitrates between three time sources. Higher priority sources override lower priority sources when they become available.

```
Priority 1 (highest): GNSS 1PPS + GNSS time
    └─ Hardware 1PPS pulse drives the 1-second tick
    └─ GNSS-derived UTC sets the epoch

Priority 2: NTP-synchronized time with synthesized 1PPS
    └─ Software timer disciplined by NTP
    └─ 1PPS synthesized from the corrected internal clock

Priority 3 (lowest, always available): Free-running internal clock
    └─ ESP32-S3 internal timer at nominally 1 Hz
    └─ No external correction; accumulates drift
```

### 5.5 DST Source Priority Hierarchy

```
Priority 1 (highest): GNSS-derived latitude / longitude
    └─ Used to resolve timezone and DST rules

Priority 2: IP geolocation (internet service)
    └─ Used when GNSS is unavailable or disabled

Priority 3 (fallback): No DST correction
    └─ UTC offset remains at the NTP-provided base offset
    └─ Active when neither GNSS nor internet geolocation is available
```

### 5.6 Hardware Constraints Summary

Circuit analysis, component values, LEDC configuration, GPIO pin assignment, and PWM-to-voltage mapping are specified in **PMC-HTD-001 Hardware Technical Design**. The firmware-relevant constraints derived from the hardware design are captured as design constraints DC-002 through DC-004.

---

## 6. Functional Requirements — Boot Phase State Machine

### 6.1 State Machine Overview

The firmware progresses through three sequential phases after power-on. Phases 2 and 3 run concurrently after Phase 1 establishes a baseline clock. Reaching "steady-state" does not exit any phase; it simply means at least one external time source is active.

```
          ┌──────────┐
POWER-ON  │          │
─────────►│ PHASE 1  │
          │ Free-run │
          └────┬─────┘
               │ Always (immediately)
               ▼
          ┌──────────┐
          │ PHASE 2  │◄──── retry every 15 s on failure
          │ NTP sync │
          └────┬─────┘
               │ Always (concurrently with Phase 2)
               ▼
          ┌──────────┐
          │ PHASE 3  │◄──── continuous (monitors GNSS)
          │ GNSS mon │
          └──────────┘
```

Phases 1, 2, and 3 all run concurrently once started. The display runs continuously throughout.

### 6.2 Phase 1 — Free-Running Clock

| ID | Requirement |
|----|-------------|
| FR-BOOT-001 | On power-on, the system shall enter Phase 1 immediately before any network activity. |
| FR-BOOT-002 | In Phase 1, the displayed time shall be initialised to 00:00:00 (hours, minutes, seconds). |
| FR-BOOT-003 | In Phase 1, seconds shall be incremented at a nominal rate of 1 Hz using the ESP32-S3 internal timer. |
| FR-BOOT-004 | Phase 1 timing shall continue uninterrupted while Phase 2 and Phase 3 activities proceed in parallel. |

### 6.3 Phase 2 — NTP Synchronisation

| ID | Requirement |
|----|-------------|
| FR-BOOT-005 | The system shall attempt to connect to the WiFi SSID stored in NVS upon entering Phase 2. |
| FR-BOOT-006 | On successful WiFi connection, the system shall issue an NTPv4 query to the configured NTP server address. |
| FR-BOOT-007 | On a valid NTP response, the system shall set the internal UTC epoch to the NTP-provided value and begin synthesising a 1PPS tick from it. |
| FR-BOOT-008 | If WiFi association fails, or if the NTP query produces no valid response, the system shall retry the failed step every 15 seconds. |
| FR-BOOT-009 | The Phase 2 retry count and the timestamp of the last retry attempt shall be accessible on the clock status page (see FR-WEB-033). |
| FR-BOOT-010 | Phase 2 is considered complete when either an NTP sync succeeds or a valid GNSS time is available from Phase 3. |

### 6.4 Phase 3 — GNSS Monitoring

| ID | Requirement |
|----|-------------|
| FR-BOOT-011 | When GNSS is enabled (FR-GPS-001), the system shall monitor the GNSS receiver in parallel with Phase 2, beginning at the same time as Phase 2. |
| FR-BOOT-012 | When the GNSS receiver presents a valid time fix (FR-GPS-005), the system shall override the internal UTC epoch with the GNSS-provided value. |
| FR-BOOT-013 | When a valid GNSS 1PPS signal is detected, the system shall switch the 1-second tick source from the synthesised NTP tick to the hardware 1PPS pulse. |
| FR-BOOT-014 | If GNSS is disabled or no valid GNSS fix is available, the 1-second tick shall be synthesised from the NTP-corrected internal timer. |
| FR-BOOT-015 | Loss of a valid GNSS fix shall cause the system to fall back to NTP-synthesised 1PPS without any discontinuity in the displayed time beyond ±1 second. |

### 6.5 Tick Source Arbitration

| ID | Requirement |
|----|-------------|
| FR-BOOT-016 | Transition between hardware 1PPS (GNSS) and software 1PPS (NTP) in either direction shall not cause the displayed seconds value to jump by more than 1 count. |
| FR-BOOT-017 | The currently active tick source (GNSS hardware 1PPS or NTP synthesised) shall be logged to the serial debug output whenever it changes (see NFR-MNT-002). |

---

## 7. Functional Requirements — Display Subsystem

### 7.1 Meter Assignment

| ID | Requirement |
|----|-------------|
| FR-DSP-001 | Meter 0, connected to GPIO 15 via LEDC_CHANNEL_0 / LEDC_TIMER_0, shall display the **local** hour of day on a scale of 0 to 24. |
| FR-DSP-002 | Meter 1, connected to GPIO 16 via LEDC_CHANNEL_1 / LEDC_TIMER_1, shall display the **local** minute of the current hour on a scale of 0 to 60. |
| FR-DSP-003 | Meter 2, connected to GPIO 17 via LEDC_CHANNEL_2 / LEDC_TIMER_2, shall display the **local** second of the current minute on a scale of 0 to 60. |
| FR-DSP-003a | UTC time shall never be displayed directly on any panel meter; the UTC-to-local conversion (including DST) shall always be applied before driving the meters. |

### 7.2 Voltage-to-Deflection Mapping

| ID | Requirement |
|----|-------------|
| FR-DSP-004 | A time value of 0 shall produce a PWM duty of `zero_offset` (the per-meter calibrated zero-point duty). |
| FR-DSP-005 | The maximum time value for each meter (24 for hours, 60 for minutes, 60 for seconds) shall produce a PWM duty of `full_scale` (the per-meter calibrated full-scale duty). |
| FR-DSP-006 | The duty for any intermediate time value shall be computed by linear interpolation between `zero_offset` and `full_scale`, proportional to the value's position within the meter's full range (0–24 for hours, 0–60 for minutes/seconds). The exact formula is specified in PMC-STD-001 Section 5.2. |
| FR-DSP-007 | The display shall be updated exactly once per 1PPS tick event. |
| FR-DSP-008 | The PWM frequency for all three LEDC channels shall be set to 80 000 Hz (80 kHz). |
| FR-DSP-009 | All PWM duty updates shall use the PWM driver API defined in `Research/PWMDriver/src/pwm_driver.h` to ensure glitch-free, cycle-boundary-aligned updates (see also NFR-PORT-001). |

### 7.3 Calibration Parameters

| ID | Requirement |
|----|-------------|
| FR-DSP-010 | Each of the three meters shall have an independently configurable `zero_offset` duty value (8-bit, 0–255) stored in NVS. |
| FR-DSP-011 | Each of the three meters shall have an independently configurable `full_scale` duty value (8-bit, 0–255) stored in NVS. |
| FR-DSP-012 | Calibration values shall survive power cycles. |
| FR-DSP-013 | Calibration values shall be settable and retrievable via the web GUI calibration page (FR-WEB-010 through FR-WEB-015). |
| FR-DSP-014 | Factory-default calibration values shall be `zero_offset = 0` and `full_scale = 232` for all three meters. The value 232 corresponds to a PWM output of approximately 3.0 V (232 / 255 × 3.3 V), which equals full-scale deflection after series resistor modification. |

---

## 8. Functional Requirements — Timekeeping Engine

### 8.1 Internal Time Representation

| ID | Requirement |
|----|-------------|
| FR-TIM-001 | The firmware shall maintain internal time as a 64-bit unsigned UTC epoch counter (seconds since 1970-01-01 00:00:00 UTC). |
| FR-TIM-002 | A sub-second phase accumulator shall be maintained to track fractional seconds for disciplining the synthesised 1PPS. |
| FR-TIM-003 | Local time for display shall be derived from the UTC epoch by applying the current UTC offset, which incorporates any active DST adjustment. |

### 8.2 1PPS Tick Discipline

| ID | Requirement |
|----|-------------|
| FR-TIM-004 | When the active tick source is the hardware GNSS 1PPS, the firmware shall increment the UTC epoch on each rising edge of the 1PPS input GPIO. |
| FR-TIM-005 | When the active tick source is the synthesised NTP tick, the firmware shall increment the UTC epoch using a FreeRTOS timer calibrated against the last NTP response. |
| FR-TIM-006 | Accumulated clock error detected by NTP re-sync shall be corrected by stepping the internal epoch (not slewing) if the error exceeds 1 second, or by adjusting the FreeRTOS timer period if the error is ≤ 1 second. |

### 8.3 NTP Synchronisation

| ID | Requirement |
|----|-------------|
| FR-NTP-001 | The firmware shall support configuration of at least one NTP server address (hostname or dotted-decimal IP) stored in NVS. |
| FR-NTP-002 | The default NTP server address shall be `pool.ntp.org`. |
| FR-NTP-003 | After the initial NTP sync, the firmware shall re-query the NTP server at a configurable interval stored in NVS (default: 3600 seconds). |
| FR-NTP-004 | The NTP stratum of the responding server, the timestamp of the last successful sync, and the next scheduled sync time shall be stored in RAM and exposed on the status page (FR-WEB-032). |

---

## 9. Functional Requirements — GNSS Subsystem

### 9.1 Enable / Disable

| ID | Requirement |
|----|-------------|
| FR-GPS-001 | GNSS support shall be independently enabled or disabled via the web GUI GNSS page (FR-WEB-025 through FR-WEB-027). The setting shall be persisted in NVS. |
| FR-GPS-002 | When GNSS is disabled, all GNSS-related tasks, UART, and GPIO interrupt shall be inactive; no GNSS-related resources shall be allocated. |

### 9.2 Data Input

| ID | Requirement |
|----|-------------|
| FR-GPS-003 | The firmware shall parse GNSS NMEA 0183 sentences received over a UART interface. At minimum, `$GPRMC` and `$GNRMC` sentences shall be decoded for UTC time, fix validity, latitude, and longitude. |
| FR-GPS-004 | The firmware shall detect and latch the rising edge of the GNSS 1PPS signal on a dedicated interrupt-capable GPIO (pin TBD; see Appendix F). |

### 9.3 Time Override

| ID | Requirement |
|----|-------------|
| FR-GPS-005 | A GNSS time fix shall be considered valid when the NMEA data-valid flag is 'A' and at least one consecutive valid sentence has been received within the preceding 2 seconds. |
| FR-GPS-006 | When a valid GNSS fix is available, the GNSS-provided UTC time shall override the NTP-derived time unconditionally. |
| FR-GPS-007 | When a valid GNSS fix is lost (no valid sentence received for more than 5 seconds), the firmware shall fall back to the NTP-synthesised tick without user intervention. |

### 9.4 Location Data

| ID | Requirement |
|----|-------------|
| FR-GPS-008 | When a valid GNSS fix is available, the decoded latitude and longitude shall be provided to the DST engine (Section 10) as the Priority 1 location source. |
| FR-GPS-009 | The most recently valid GNSS-derived latitude and longitude shall be cached in NVS so that they survive a power cycle and can serve as a location hint if GNSS signal is lost. |

---

## 10. Functional Requirements — Daylight Saving Time

### 10.1 DST Source Selection

The DST engine selects a location source according to the priority hierarchy in Section 5.5. If neither GNSS nor IP geolocation can provide a location, no DST offset is applied and the UTC base offset is used as-is.

### 10.2 DST Requirements

| ID | Requirement |
|----|-------------|
| FR-DST-001 | The firmware shall determine the applicable timezone and DST rules from the location data provided by the highest-priority currently available source (GNSS coordinates or IP geolocation). |
| FR-DST-002 | DST transitions shall be applied automatically at the correct local wall-clock transition time without any user intervention. |
| FR-DST-003 | At a DST transition, the UTC offset shall be updated atomically so that no visible anomaly (skipped or repeated second) occurs on the panel meters. |
| FR-DST-004 | When IP geolocation is the active DST source, the firmware shall query the geolocation service once per successful network reconnection and once per 24 hours thereafter. |
| FR-DST-005 | The current DST state (active / inactive), the active UTC offset in hours and minutes, and the DST source in use shall be displayed on the clock status page (FR-WEB-030). |
| FR-DST-006 | The timezone rules database shall be embedded in firmware flash. A live internet connection shall not be required at the moment of a DST transition. |

---

## 11. Functional Requirements — Network and Connectivity

### 11.1 Station Mode (STA)

| ID | Requirement |
|----|-------------|
| FR-NW-001 | The firmware shall connect to a WiFi access point using the SSID and password stored in NVS. |
| FR-NW-002 | The firmware shall use DHCP exclusively to obtain an IP address; static IP configuration is out of scope. |
| FR-NW-003 | If the STA connection is lost, the firmware shall attempt reconnection automatically using exponential back-off, starting at 5 seconds and capped at 300 seconds per attempt. |
| FR-NW-004 | The configured SSID and password shall be updatable via the web GUI WiFi configuration page (FR-WEB-020 through FR-WEB-023). |

### 11.2 Access Point Fallback (AP Mode)

| ID | Requirement |
|----|-------------|
| FR-NW-005 | If STA mode fails to associate within a configurable timeout stored in NVS (default: 60 seconds), the firmware shall activate AP mode. |
| FR-NW-006 | The fallback AP shall be open (no password required). |
| FR-NW-007 | The fallback AP SSID shall be `PanelClock-AABB`, where `AABB` are the last two bytes of the device's WiFi MAC address expressed as uppercase hexadecimal (e.g., MAC `...:3F:A2` → SSID `PanelClock-3FA2`). |
| FR-NW-008 | When a STA connection is successfully established, the fallback AP shall be deactivated automatically. |
| FR-NW-009 | While in AP mode, the full web GUI shall be available to any client connected to the fallback AP. |

### 11.3 mDNS Registration

| ID | Requirement |
|----|-------------|
| FR-NW-010 | The device shall register itself in mDNS (RFC 6762 / Zeroconf / Bonjour) with the fixed hostname `panelclock`, resolvable by any mDNS-capable client on the same network as `panelclock.local`. |
| FR-NW-011 | mDNS registration shall be initiated immediately after a STA WiFi connection is established and shall be cancelled when the STA connection is lost. mDNS shall not be active during AP-only fallback mode. |
| FR-NW-012 | The device shall advertise its HTTP service via DNS-SD with service type `_http._tcp`, port 80, and instance name `panelclock`, so that network browsers (e.g., Bonjour Browser) can discover it without knowing the IP address. |
| FR-NW-013 | The hostname `panelclock` shall also be used as the DHCP client hostname (Option 12) sent during STA association, so that routers with hostname-based DNS forwarding can resolve `panelclock` on the LAN. |

---

## 12. Functional Requirements — Web GUI

### 12.1 General Web Server

| ID | Requirement |
|----|-------------|
| FR-WEB-001 | The embedded HTTP server shall listen on TCP port 80. |
| FR-WEB-002 | All pages shall be reachable from a persistent navigation menu present on every page. |
| FR-WEB-003 | The navigation menu shall include links to: Calibration (`/calibrate`), WiFi (`/wifi`), GNSS (`/gnss`), Status (`/status`), and Firmware Update (`/update`). |
| FR-WEB-004 | All static assets (HTML, CSS, JavaScript, images) shall be served from device flash; no external CDN dependencies are permitted. |
| FR-WEB-005 | The GUI shall be functional in any standards-compliant browser with JavaScript enabled; no browser extension or application installation is required. |

### 12.2 Page: Meter Calibration (`/calibrate`)

| ID | Requirement |
|----|-------------|
| FR-WEB-010 | The calibration page shall present three independent sections, one for each meter (Hours, Minutes, Seconds). |
| FR-WEB-011 | Each section shall provide a numeric input field for the `zero_offset` duty value (integer, 0–255). |
| FR-WEB-012 | Each section shall provide a numeric input field for the `full_scale` duty value (integer, 0–255). |
| FR-WEB-013 | Each section shall provide a live-preview control (e.g., a slider or test-value input) that drives the corresponding meter to a specific duty position without saving the value to NVS. |
| FR-WEB-014 | A "Save" action per section shall write the entered `zero_offset` and `full_scale` values to NVS and apply them to the running display immediately. |
| FR-WEB-015 | A "Reset to Defaults" action shall restore `zero_offset = 0` and `full_scale = 255` for all three meters, write the defaults to NVS, and apply them immediately. |

### 12.3 Page: WiFi Configuration (`/wifi`)

| ID | Requirement |
|----|-------------|
| FR-WEB-020 | The WiFi configuration page shall display a list of SSIDs detected by a WiFi scan, updated on page load. |
| FR-WEB-021 | The page shall allow the user to select an SSID from the scan list or type one manually. |
| FR-WEB-022 | The page shall provide a password input field (masked by default). |
| FR-WEB-023 | Saving the WiFi configuration shall store the SSID and password in NVS and trigger a new STA connection attempt. |
| FR-WEB-024 | The page shall display the current network state (connected / connecting / AP-only) and, when connected, the assigned IP address. |

### 12.4 Page: GNSS Configuration (`/gnss`)

| ID | Requirement |
|----|-------------|
| FR-WEB-025 | The GNSS configuration page shall display the current GNSS enabled / disabled state. |
| FR-WEB-026 | The page shall provide a toggle control to enable or disable GNSS. The change shall be persisted to NVS and applied without requiring a device reboot. |
| FR-WEB-027 | When GNSS is enabled, the page shall display: fix status (valid / no fix), number of satellites in use, current latitude, and current longitude. |

### 12.5 Page: Clock Status (`/status`)

| ID | Requirement |
|----|-------------|
| FR-WEB-030 | The status page shall display: current UTC time, current local time, active UTC offset (hours and minutes), DST state (active / inactive), and DST source (GNSS / IP geolocation / none). |
| FR-WEB-031 | The status page shall display the active tick source: GNSS hardware 1PPS or NTP synthesised 1PPS. |
| FR-WEB-032 | The status page shall display: configured NTP server address, time of last successful NTP sync, NTP stratum of the last response, and time of next scheduled NTP sync. |
| FR-WEB-033 | The status page shall display Phase 2 NTP retry count and timestamp of the last retry attempt. |
| FR-WEB-034 | The status page shall auto-refresh all dynamic data at a 5-second interval without a full page reload (e.g., via JavaScript fetch or WebSocket). |

### 12.6 Page: Firmware Update (`/update`)

| ID | Requirement |
|----|-------------|
| FR-WEB-040 | The firmware update page shall provide a file upload form that accepts a firmware package file. |
| FR-WEB-041 | Before writing any data to flash, the firmware shall verify the cryptographic signature of the uploaded package using the public key embedded in flash (FR-SEC-001 through FR-SEC-005). |
| FR-WEB-042 | If signature verification fails, the firmware shall reject the upload entirely, display a clear error message including the reason code, and leave the running firmware unchanged. |
| FR-WEB-043 | If signature verification passes, the firmware shall perform an OTA update using the ESP-IDF dual-OTA-partition scheme, writing to the inactive OTA slot. |
| FR-WEB-044 | During the flash write operation, the page shall display a progress indicator (percentage of bytes written). |
| FR-WEB-045 | On successful completion, the page shall display a success message and inform the user that the device will reboot in 10 seconds. |
| FR-WEB-046 | The device shall reboot automatically 10 seconds after a successful OTA flash to boot into the new firmware. |

---

## 13. Functional Requirements — Security and FOTA Authentication

### 13.1 Key Scheme

| ID | Requirement |
|----|-------------|
| FR-SEC-001 | The device shall embed one asymmetric public key (Ed25519 recommended; RSA-2048 acceptable) in a dedicated flash region that is excluded from OTA write operations. |
| FR-SEC-002 | The firmware package uploaded via the web GUI shall consist of two files: the firmware binary image and a detached digital signature file. |
| FR-SEC-003 | The private key corresponding to the embedded public key shall never be stored on the device. |

### 13.2 Verification Process

| ID | Requirement |
|----|-------------|
| FR-SEC-004 | Signature verification shall be completed entirely in RAM before any flash write to the OTA partition begins. |
| FR-SEC-005 | The digest algorithm used for signing shall be SHA-256 at minimum. |
| FR-SEC-006 | A failed signature verification shall produce a log entry on the serial debug output and on the status page, including a human-readable reason code. |

### 13.3 Key Rotation

| ID | Requirement |
|----|-------------|
| FR-SEC-007 | Re-keying (replacing the embedded public key) shall require delivery of a signed firmware update that carries the new public key in a known structure. The detailed key-rotation procedure shall be defined in a separate Key Management Procedure document. |

---

## 14. Non-Functional Requirements

### 14.1 Performance

| ID | Requirement |
|----|-------------|
| NFR-PERF-001 | The latency from a 1PPS tick event to the completion of all three `pwm_driver_set_duty()` calls shall be less than 10 milliseconds. |
| NFR-PERF-002 | All web GUI pages shall complete initial load within 3 seconds over a typical WiFi connection. |
| NFR-PERF-003 | NTP query processing shall execute in a dedicated FreeRTOS task and shall not block the display update task. |

### 14.2 Reliability

| ID | Requirement |
|----|-------------|
| NFR-REL-001 | The firmware shall enable the ESP32-S3 hardware watchdog with a timeout of no more than 30 seconds. |
| NFR-REL-002 | Following a watchdog reset, the system shall boot normally and re-enter Phase 1 as if starting from power-on. |
| NFR-REL-003 | NVS corruption shall be detected at startup. If detected, the firmware shall erase and reinitialise the NVS namespace to factory defaults and log the event to the serial debug output. |

### 14.3 Power and Electrical

| ID | Requirement |
|----|-------------|
| NFR-PWR-001 | On power-down or reset, PWM outputs shall be driven to duty 0 (0 V) within one PWM cycle to prevent sustained non-zero voltage on the meter coils. |

### 14.4 Maintainability

| ID | Requirement |
|----|-------------|
| NFR-MNT-001 | All user-configurable parameters (NTP server, retry interval, AP timeout, etc.) and all NVS key names shall be defined as named constants; no magic numbers or literal strings shall appear in application code. |
| NFR-MNT-002 | The firmware shall emit a structured serial debug log at 115200 baud via UART0 (USB-CDC / CH340). Log entries shall include, at minimum: boot phase transitions, tick source changes, NTP sync events (success and failure), GNSS fix state changes, DST transitions, and OTA events. |

### 14.5 Portability

| ID | Requirement |
|----|-------------|
| NFR-PORT-001 | The `pwm_driver_t` API defined in `Research/PWMDriver/src/pwm_driver.h` shall be the sole abstraction boundary between the timekeeping/display logic and the LEDC hardware. Direct LEDC register access shall not appear outside `pwm_driver.cpp`. |

---

## 15. Interface Requirements

### 15.1 Hardware Interfaces

| ID | Requirement |
|----|-------------|
| IC-HW-001 | PWM meter outputs shall use the GPIO pins, LEDC channels, and LEDC timers assigned in PMC-HTD-001 §3 and §5. Firmware shall not assume alternative pin or channel assignments. |
| IC-HW-002 | Each PWM output channel has a dedicated RC low-pass filter. The firmware shall not assume a filter cutoff frequency other than ≈ 16 Hz. Component values and circuit design are specified in PMC-HTD-001 §4.4. |
| IC-HW-003 | The GNSS 1PPS input is connected to GPIO 10, configured as input with no internal pull resistor. The firmware shall register a rising-edge interrupt on this pin. Pin assignment details are in PMC-HTD-001 §3. |
| IC-HW-004 | The GNSS UART interface uses UART1 (GPIO 18 RX, GPIO 21 TX). The default baud rate is 9 600; it is configurable via NVS. Interface details are in PMC-HTD-001 §6. |

### 15.2 Software and Protocol Interfaces

| ID | Requirement |
|----|-------------|
| IC-SW-001 | NTP communication shall comply with NTPv4 as defined in IETF RFC 5905, using UDP port 123. |
| IC-SW-002 | GNSS data shall be received as NMEA 0183 sentences. The firmware shall at minimum parse `$GPRMC` and `$GNRMC` sentence types. |
| IC-SW-003 | The web GUI shall be served over HTTP/1.1 on TCP port 80. HTTPS is deferred to a future version (see Appendix F). |
| IC-SW-004 | The OTA firmware update mechanism shall use the ESP-IDF dual-slot OTA partition layout (one active OTA slot, one inactive OTA slot, plus factory partition). |

### 15.3 Human Interfaces

| ID | Requirement |
|----|-------------|
| IC-HMI-001 | The primary user interface shall be the embedded web GUI as specified in Section 12. |
| IC-HMI-002 | A secondary diagnostic interface shall be available as a serial debug stream at 115200 baud, 8-N-1, on UART0 / USB-CDC. |

---

## 16. Design Constraints

| ID | Constraint |
|----|------------|
| DC-001 | The firmware target is the WEMOS LOLIN S3 (ESP32-S3-WROOM-1). The build system shall be PlatformIO with the Arduino framework on ESP-IDF 5.x. The firmware shall run on FreeRTOS as provided by the ESP-IDF Arduino core; no bare-metal or alternative RTOS may be substituted. |
| DC-002 | The ESP32-S3 does not provide an analogue DAC peripheral. All analogue meter drive shall be produced exclusively by PWM + RC filtering through the LEDC peripheral. |
| DC-003 | Each panel meter is modified so that a 3 V drive voltage produces full-scale deflection. The firmware design point is 0 – 3 V (duty 0 – 232); the 3.3 V GPIO maximum is not used as the full-scale point. Series resistor calculation and modification procedure are in PMC-HTD-001 §4.3. |
| DC-004 | PWM resolution is fixed at 8 bits (0–255 duty steps) at 80 kHz with an 80 MHz LEDC clock. This gives 256 discrete meter positions; no fractional duty is available. |
| DC-005 | The flash partition table must include two OTA application partitions. The partition table is a build-time configuration and cannot be changed by FOTA. |
| DC-006 | The public key for FOTA signature verification is embedded in the firmware binary at build time. It cannot be modified at runtime and is excluded from OTA writes by the partition layout. |

---

## 17. Appendices

### Appendix A — Requirements Traceability Matrix

| Requirement ID | Feature Area | Test Case | Notes |
|----------------|-------------|-----------|-------|
| FR-BOOT-001..004 | Boot / Phase 1 | TC-BOOT-001 | |
| FR-BOOT-005..010 | Boot / Phase 2 / NTP | TC-BOOT-002 | |
| FR-BOOT-011..017 | Boot / Phase 3 / GNSS | TC-BOOT-003 | |
| FR-DSP-001..014 | Display / meters | TC-DSP-001 | |
| FR-TIM-001..006 | Timekeeping | TC-TIM-001 | |
| FR-NTP-001..004 | NTP sync | TC-NTP-001 | |
| FR-GPS-001..009 | GNSS subsystem | TC-GPS-001 | |
| FR-DST-001..006 | DST engine | TC-DST-001 | |
| FR-NW-001..009 | Network / WiFi | TC-NW-001 | |
| FR-NW-010..013 | Network / mDNS | TC-NW-002 | |
| FR-WEB-001..046 | Web GUI | TC-WEB-001..005 | One test case per page |
| FR-SEC-001..007 | FOTA security | TC-SEC-001 | |
| NFR-* | Non-functional | TC-NFR-001 | Performance, watchdog |

*Test case specifications are defined in a separate Test Specification document.*

---

### Appendix B — Boot Phase State-Transition Table

Moved to **PMC-STD-001 Software Technical Design, Section 5.1**.

---

### Appendix C — Time Source and DST Priority Flowcharts

Moved to **PMC-STD-001 Software Technical Design, Section 5.3 and Section 5.7**.

---

### Appendix D — Web GUI Site Map

Moved to **PMC-STD-001 Software Technical Design, Section 5.8**.

---

### Appendix E — NVS Key Inventory

Moved to **PMC-STD-001 Software Technical Design, Section 6**.

---

### Appendix F — Open Issues and TBDs

| ID | Item | Status | Resolution |
|----|------|--------|------------|
| TBD-001 | GNSS 1PPS input GPIO pin number | ✅ Resolved | GPIO 10 — see PMC-STD-001 §5.6, §7 |
| TBD-002 | GNSS UART peripheral number and baud rate | ✅ Resolved | UART1 (Serial1), RX GPIO 18, TX GPIO 21, default 9600 baud (configurable via NVS) — see PMC-STD-001 §5.6, §6, §7 |
| TBD-003 | HTTPS support (IC-SW-003 deferred) — evaluate flash space after initial build | Open | v1.1 milestone — see PMC-STD-001 §8 TBD-003 |
| TBD-004 | Ed25519 vs RSA-2048 for FOTA signing — confirm mbedTLS support and binary size impact on target | Open | Before FR-SEC implementation — see PMC-STD-001 §8 TBD-004 |
| TBD-005 | IP geolocation service URL and API format | ✅ Resolved | ip-api.com (primary), worldtimeapi.org (fallback) — see PMC-STD-001 §5.7, §7 |
| TBD-006 | Timezone rules library | ✅ Resolved | Custom POSIX TZ string parser; rule stored in NVS key `clock/posix_tz` — see PMC-STD-001 §5.7, §6, §7 |
| TBD-007 | HTTP server library | ✅ Resolved | ESPAsyncWebServer + AsyncTCP — see PMC-STD-001 §3.1, §5.8 |
