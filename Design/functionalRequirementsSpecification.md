# Functional Requirements Specification
## PanelMeterClock Firmware

---

## 1. Document Control

### 1.1 Identification

| Field | Value |
|-------|-------|
| Document title | Functional Requirements Specification — PanelMeterClock Firmware |
| Document ID | PMC-FRS-001 |
| Version | 0.2 (draft) |
| Date | 2026-04-29 |
| Author | Remko Welling |
| Status | Draft — under review |

### 1.2 Revision History

| Version | Date | Author | Change Summary |
|---------|------|--------|----------------|
| 0.1 | 2026-04-29 | Remko Welling | Initial draft |
| 0.2 | 2026-06-08 | Remko Welling | Restructured as a pure functional specification: each requirement paired with an acceptance / verification criterion; technical implementation detail and the interface/design-constraint sections moved to the Technical Design (PMC-STD-001 / PMC-HTD-001). |

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

This document defines the functional and non-functional requirements for the PanelMeterClock, together with the acceptance criteria by which each is verified in testing. It is the authoritative statement of *what* the clock shall do; *how* it is implemented is specified in the Technical Design (PMC-STD-001 Software Technical Design, PMC-HTD-001 Hardware Technical Design).

### 2.2 Scope

**In scope:**
- Firmware running on the ESP32-S3 target
- Three-meter display subsystem (hours, minutes, seconds)
- Boot phase state machine and time-source management
- NTP synchronisation and WiFi connectivity
- GNSS receiver integration and 1PPS tick discipline
- DCF77 longwave radio time-code receiver integration (offline time source)
- Battery-backed real-time clock (RTC) for time retention across power loss and local setting
- Automatic Daylight Saving Time (DST) detection and transition
- Embedded web GUI (calibration, configuration, status, FOTA)
- Firmware Over The Air (FOTA) with asymmetric-key authentication

**Out of scope:**
- PCB schematic and layout
- Mechanical enclosure and panel meter face design
- NTP server infrastructure
- GNSS hardware procurement and wiring
- HTTPS (out of scope; the web GUI is HTTP/1.1 only)

### 2.3 Intended Audience

- Firmware developer(s)
- Hardware designer
- Test engineer
- Project owner

### 2.4 How to Read This Document

Sections 6 through 15 contain numbered requirements in the form `<Type>-<Domain>-<NNN>` (see Section 3 for the numbering scheme). Each requirement is atomic and is paired with an **Acceptance / Verification Criteria** entry that defines, in observable terms, how the requirement is confirmed in testing. The corresponding test methods and pass conditions are held in the Technical Design (PMC-STD-001 §8 and PMC-HTD-001). Technical implementation decisions — pin assignments, components, protocols, algorithms, libraries — are specified in the Technical Design, not here.

Section 5 provides a system-level overview with priority hierarchies. Appendix B contains a complete boot-phase state-transition table. Appendix F tracks all open issues and TBD items.

---

## 3. Definitions, Acronyms, and Abbreviations

| Term | Definition |
|------|------------|
| 1PPS | One Pulse Per Second — a precise timing signal, typically from a GNSS receiver, used as a hardware tick source |
| AP mode | Access Point mode — the ESP32-S3 hosts its own WiFi network for client devices to connect to |
| CET / CEST | Central European Time (UTC+1) / Central European Summer Time (UTC+2) — the civil time broadcast by the DCF77 transmitter |
| DCF77 | The German longwave (77.5 kHz) time-signal service transmitted from Mainflingen and derived from PTB atomic clocks; carries the full date and time once per minute. Used here as an offline radio time source |
| DST | Daylight Saving Time — the seasonal clock adjustment applied in many jurisdictions |
| Duty cycle | The fraction of one PWM period during which the output signal is high, expressed as an 8-bit integer (0 = 0 %, 255 = 100 %) |
| FOTA | Firmware Over The Air — uploading and applying new firmware via the embedded web GUI without physical access |
| FreeRTOS | Real-time operating system layer provided by ESP-IDF for the ESP32 |
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
| RTC | Real-Time Clock — the battery-backed DS1307Z chip that retains the time while main power is off; it provides the time at boot and as the lowest-priority source, and can be set manually (PMC-GUI-001). See §5.4 (Priority 4). |
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
| Siemens 1604P Panel Meter (DC 10.00 GF024T005) | `Documentation/` — Siemens product documentation |
| Quectel L76-M33 GNSS Module | `Documentation/` — Quectel L76-M33 Hardware Design Guide and Product Specification |
| DCF77 Receiver Module | `Documentation/` — DCF77 longwave (77.5 kHz) receiver module; ferrite antenna, demodulated time-code output |

### 4.2 Software and Protocol Standards

| Document | Reference |
|----------|-----------|
| Network Time Protocol v4 | IETF RFC 5905 |
| NMEA 0183 GNSS Sentence Standard | NMEA 0183 v4.11 |
| IANA Time Zone Database | https://www.iana.org/time-zones |
| DCF77 Time-Code Format | PTB (Physikalisch-Technische Bundesanstalt) DCF77 specification |
| ESP-IDF LEDC Peripheral Guide | Espressif ESP-IDF Programming Guide, LEDC chapter |
| ESP-IDF OTA Update Guide | Espressif ESP-IDF Programming Guide, OTA chapter |

### 4.3 Internal Project Documents

| Document | Path |
|----------|------|
| Hardware Technical Design | `Design/hardwareTechnicalDesign.md` |
| Software Technical Design | `Design/softwareTechnicalDesign.md` |
| GUI / User Interface Specification | `Design/guiSpecification.md` |
| FOTA Signature Considerations | `Design/signatureConciderations.md` |
| PWM Driver Design and API | `Research/PWMDriver.md` |
| PWM + RC Filter Verification Plan | `Research/PWMTest.md` |
| Design Trade-off Notes | `Design/notes.md` |

---

## 5. System Overview

### 5.1 Product Description

PanelMeterClock is a wall clock that displays the current **local time** on three moving-coil panel meters — one each for hours, minutes and seconds. It acquires accurate time from GNSS, the internet (NTP) and the DCF77 radio signal, applies the local timezone and daylight saving automatically, and is configured through an embedded web GUI. A battery-backed real-time clock (RTC) keeps time across power loss and can be set locally with a rotary encoder (PMC-GUI-001). Hardware and firmware implementation are specified in the Technical Design (PMC-HTD-001 / PMC-STD-001).

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

   ┌───────────────┐  ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
   │ GNSS Receiver │  │ NTP Server    │  │ DCF77 Receiver│  │ Web Browser   │
   │ (optional)    │  │ (internet)    │  │ (optional)    │  │ (user)        │
   │ UART + 1PPS   │  │ UDP / 123     │  │ time-code in  │  │ HTTP / 80     │
   └───────┬───────┘  └───────┬───────┘  └───────┬───────┘  └───────┬───────┘
           │                  │                  │                  │
           └──────────────────┴  WiFi/UART/GPIO  ┴──────────────────┘
                                   ESP32-S3
```

> The **DCF77 receiver** (77.5 kHz longwave, optional) connects via a single demodulated time-code GPIO plus an enable line, and supplies UTC time only — no location data for the DST engine.

### 5.3 Operating Modes Summary

| Mode | Entry condition | Active time sources | Web GUI |
|------|----------------|---------------------|---------|
| Phase 1 — RTC | Power-on | Battery-backed RTC (time retained across power-off) | Available (no WiFi, AP on) |
| Phase 2 — NTP | Phase 1 active; WiFi credentials stored | Internal timer + NTP discipline | Available when WiFi connected after timeout AP is made active |
| Phase 3 — GNSS | Phase 2 active; GNSS enabled | GNSS 1PPS (preferred) or NTP synthesized | Available |
| DCF77 (parallel) | DCF77 enabled | DCF77 radio time when GNSS and NTP are unavailable | Available |
| Steady-state | Any phase completes initial sync | Best available source | Always available |
| AP mode | STA connection unavailable | RTC (last synced or set) time | Available on AP |

### 5.4 Time Source Priority Hierarchy

The firmware arbitrates between four time sources. Higher priority sources override lower priority sources when they become available.

```
Priority 1 (highest): GNSS 1PPS + GNSS time
    └─ Hardware 1PPS pulse drives the 1-second tick
    └─ GNSS-derived UTC sets the clock

Priority 2: NTP-synchronized time with synthesized 1PPS
    └─ Software timer disciplined by NTP
    └─ 1PPS synthesized from the corrected internal clock

Priority 3: DCF77 longwave radio time
    └─ Decoded DCF77 time (CET/CEST) converted to UTC sets the clock
    └─ 1-second tick taken from the DCF77 per-second mark; synthesized when reception drops
    └─ Used only when no GNSS fix and no NTP sync are available

Priority 4 (lowest, always available): RTC (DS1307, battery-backed)
    └─ Battery-backed time-of-day, retained across power loss
    └─ Seeds the clock at boot and provides time when no higher source is available
    └─ Settable manually (PMC-GUI-001); written by higher sources to stay accurate
```

### 5.5 DST Source Priority Hierarchy

```
Priority 1 (highest): GNSS-derived latitude / longitude
    └─ Used to resolve timezone and DST rules

Priority 2: IP geolocation (internet service)
    └─ Used when GNSS is unavailable or disabled

Priority 3: DCF77 CET/CEST flag (Z1/Z2)
    └─ Directly indicates Central European DST state — no location or internet needed
    └─ Applies only when the configured display timezone is Central European (CET/CEST)

Priority 4 (fallback): No DST correction
    └─ UTC offset remains at the NTP-provided base offset
    └─ Active when none of the above is available
```

### 5.6 Hardware Constraints Summary

Circuit analysis, component values, GPIO pin assignment, and the drive mapping are specified in **PMC-HTD-001 Hardware Technical Design**. Design constraints and external interfaces (target platform, pin map, protocols, partitioning) are specified in the Technical Design (PMC-HTD-001 and PMC-STD-001).

---

## 6. Functional Requirements — Boot Phase State Machine

### 6.1 State Machine Overview

The firmware progresses through three sequential phases after power-on, with DCF77 monitoring running alongside. Phase 1 loads the retained time from the battery-backed RTC; DCF77 monitoring (when enabled) then begins, followed by Phase 2 (NTP) and Phase 3 (GNSS). Reaching "steady-state" does not exit any phase; it simply means at least one external time source is active. Boot order reflects when each source is started, not its priority — time-source arbitration follows Section 5.4 (GNSS > NTP > DCF77 > RTC).

```
          ┌──────────┐
POWER-ON  │          │
─────────►│ PHASE 1  │
          │   RTC    │
          └────┬─────┘
               │ Always (immediately)
               ▼
          ┌──────────┐
          │  DCF77   │◄──── continuous (monitors DCF77)
          │ monitor  │
          └────┬─────┘
               │ Always (concurrently)
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

Phase 1, DCF77 monitoring, and Phases 2 and 3 all run concurrently once started. The display runs continuously throughout.

### 6.2 Phase 1 — RTC / Free-Running Clock

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-BOOT-001 | On power-on the clock shall start running immediately, before any network activity. | Display is live shortly after power-on, before WiFi connects. |
| FR-BOOT-002 | At power-on the clock shall initialise its time from the battery-backed RTC; if the RTC time is invalid it shall start at 00:00:00. | After a power cycle the meters resume the retained time (or 00:00:00 if the RTC is unset). |
| FR-BOOT-003 | The clock shall advance at one second per second from the RTC-seeded internal clock until an external source is acquired. | Seconds advance at ~1 Hz with no external source. |
| FR-BOOT-004 | The clock shall keep running while network and external-source acquisition proceed. | Time keeps advancing throughout acquisition. |

### 6.3 Phase 2 — NTP Synchronisation

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-BOOT-005 | The clock shall attempt to join the configured WiFi network on start-up. | With valid credentials stored, the clock joins WiFi. |
| FR-BOOT-006 | Once on the network the clock shall obtain time from the configured NTP server. | After connecting, the clock synchronises to NTP time. |
| FR-BOOT-007 | On a successful NTP sync the clock shall adopt that time and run from it. | Displayed time corrects to NTP time after sync. |
| FR-BOOT-008 | If WiFi or NTP acquisition fails, the clock shall keep retrying automatically. | After a failure the clock retries periodically without intervention. |
| FR-BOOT-009 | The NTP retry count and last-attempt time shall be visible on the status page (FR-WEB-033). | Status page shows retry count and last-attempt time. |
| FR-BOOT-010 | Time acquisition is complete once an NTP sync succeeds or a valid GNSS time is available. | Clock reaches steady-state when NTP or GNSS provides time. |

### 6.4 Phase 3 — GNSS Monitoring

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-BOOT-011 | When GNSS is enabled, the clock shall monitor the GNSS receiver alongside NTP. | With GNSS enabled, the receiver is monitored from start-up. |
| FR-BOOT-012 | On a valid GNSS time fix the clock shall adopt the GNSS time in preference to NTP. | With a GNSS fix, displayed time follows GNSS, overriding NTP. |
| FR-BOOT-013 | The clock shall use the GNSS one-pulse-per-second signal as its tick once a fix is valid. | With a valid fix, the seconds tick is driven by GNSS. |
| FR-BOOT-014 | Without a GNSS fix the clock shall keep time from the next-best available source. | With GNSS disabled or no fix, the clock runs on NTP or the internal clock. |
| FR-BOOT-015 | Loss of the GNSS fix shall not disturb the displayed time by more than ±1 second. | On fix loss the clock falls back smoothly; no visible jump > 1 s. |

### 6.5 Tick Source Arbitration

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-BOOT-016 | Switching between tick sources (GNSS, DCF77 second-mark, or internal) shall not jump the displayed seconds by more than one count. | On any tick-source change the seconds reading moves by at most 1. |
| FR-BOOT-017 | The active tick source shall be reported on the diagnostic log when it changes. | Each tick-source change produces a log entry (NFR-MNT-002). |

### 6.6 DCF77 Monitoring

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-BOOT-018 | When DCF77 is enabled, the clock shall monitor the DCF77 receiver alongside NTP and GNSS once the baseline clock is running. | With DCF77 enabled, the receiver is monitored from start-up. |
| FR-BOOT-019 | When valid DCF77 time is available and neither GNSS nor NTP is, the clock shall adopt the DCF77 time (as UTC). | With only DCF77 available, displayed time follows DCF77. |
| FR-BOOT-020 | A GNSS fix or NTP sync becoming available shall override DCF77 without disturbing the displayed time by more than ±1 second. | When NTP/GNSS arrives, the clock switches to it with no visible jump > 1 s. |
| FR-BOOT-021 | The active time source (GNSS / NTP / DCF77 / RTC) shall be reported on the diagnostic log when it changes. | Each time-source change produces a log entry (NFR-MNT-002). |

---

## 7. Functional Requirements — Display Subsystem

### 7.1 Meter Assignment

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DSP-001 | The clock shall display the local hour of day on the hours meter over a 0–24 scale. | Hours meter tracks the local hour across the 0–24 range. |
| FR-DSP-002 | The clock shall display the local minute of the current hour on the minutes meter over a 0–60 scale. | Minutes meter tracks the local minute across 0–60. |
| FR-DSP-003 | The clock shall display the local second of the current minute on the seconds meter over a 0–60 scale. | Seconds meter advances through the minute and resets at the boundary. |
| FR-DSP-003a | The meters shall always show local time, never UTC. | With a non-zero UTC offset set, the meters read local time, not UTC. |

### 7.2 Display Behaviour

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DSP-004 | A value of zero shall drive the corresponding meter to its zero position. | Each meter rests at zero for value 0. |
| FR-DSP-005 | The maximum value of each meter shall drive it to full scale. | Each meter reads full scale at its maximum value. |
| FR-DSP-006 | Intermediate values shall be shown proportionally between zero and full scale. | Needle position tracks value linearly across the range. |
| FR-DSP-007 | The display shall update once per one-second tick. | Meters update once per second, in step with the clock. |
| FR-DSP-008 | The meter output shall be steady, without visible ripple or flicker. | No perceptible needle jitter at a held value. |
| FR-DSP-009 | Needle movement on each update shall be smooth, without visible glitch. | Needles move cleanly between values. |

> Meter-drive implementation (PWM channels, frequency, RC filtering, duty mapping and the driver API) is specified in PMC-STD-001 §5.2 and PMC-HTD-001 §3–§5.

### 7.3 Calibration

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DSP-010 | Each meter shall have an independently adjustable zero position. | Adjusting one meter's zero affects only that meter. |
| FR-DSP-011 | Each meter shall have an independently adjustable full-scale position. | Adjusting one meter's full scale affects only that meter. |
| FR-DSP-012 | Calibration settings shall persist across power cycles. | Calibration survives a power cycle. |
| FR-DSP-013 | Calibration shall be settable via the web GUI calibration page (FR-WEB-010..015). | Values set on the page are applied and read back. |
| FR-DSP-014 | Factory defaults shall place full scale near the top of travel without hitting the end-stop. | After a defaults reset, maximum value reads near full scale, clear of the end-stop. |

---

## 8. Functional Requirements — Timekeeping Engine

### 8.1 Internal Time Representation

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-TIM-001 | The clock shall keep time internally in UTC. | Internal time is UTC; local time is derived from it. |
| FR-TIM-002 | The clock shall track time to sub-second resolution for tick discipline. | No whole-second-only stepping artefacts; tick stays phase-aligned. |
| FR-TIM-003 | Local display time shall be derived from UTC by applying the current offset, including any active DST. | Displayed local time equals UTC plus the current offset/DST at all times. |

### 8.2 Tick Discipline

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-TIM-004 | When a GNSS one-pulse-per-second signal is the tick source, the clock shall advance one second per pulse. | Seconds advance on the GNSS pulse when it is the active tick. |
| FR-TIM-005 | When no hardware pulse is available, the clock shall advance from an internally generated one-second tick. | Seconds advance from the internal tick when no GNSS/DCF77 pulse is present. |
| FR-TIM-006 | Time error found at re-synchronisation shall be corrected — stepped if large, gently adjusted if small. | After re-sync, large errors correct at once; small errors correct without a visible jump. |
| FR-TIM-007 | When the DCF77 second-mark is the tick source, the clock shall advance on each mark and bridge reception gaps with the internal tick. | With DCF77 driving the tick, seconds follow the marks; gaps are covered without a visible jump (FR-DCF-012). |

### 8.3 NTP Synchronisation

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-NTP-001 | The NTP server shall be configurable. | A user-set NTP server is used for synchronisation. |
| FR-NTP-002 | A sensible default NTP server shall be provided out of the box. | With no user setting, the clock still synchronises via the default server. |
| FR-NTP-003 | After the first sync the clock shall re-synchronise periodically at a configurable interval. | The clock re-syncs automatically on the configured interval. |
| FR-NTP-004 | NTP status shall be visible on the status page (FR-WEB-032). | Status page shows NTP server, last sync time, quality and next sync. |

### 8.4 Time Source Arbitration

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-TIM-008 | The clock shall select its time source by priority GNSS > NTP > DCF77 > RTC, a higher source overriding a lower one when available. | With multiple sources present, the highest-priority one governs the displayed time. |

### 8.5 Real-Time Clock (RTC)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-RTC-001 | The clock shall include a battery-backed real-time clock that retains the time while main power is off. | After main power is removed and restored, the clock resumes the correct time of day. |
| FR-RTC-002 | At power-on the clock shall initialise its time from the RTC. | The first displayed time after boot matches the RTC. |
| FR-RTC-003 | When a higher-priority source (GNSS / NTP / DCF77) provides accurate time, the clock shall update the RTC so the retained time stays accurate. | After a sync, the RTC holds the corrected time (confirmed on the next cold boot). |
| FR-RTC-004 | The user shall be able to set the RTC manually, and a manually-set time shall persist across power cycles. | A time set via the encoder (PMC-GUI-001) is retained after a power cycle. |
| FR-RTC-005 | The clock shall detect an invalid RTC time (e.g. a depleted backup battery or first use), start at 00:00:00, and indicate it. | With the backup battery removed, boot shows 00:00:00 and the RTC indicator shows invalid (PMC-GUI-001). |
| FR-RTC-006 | The RTC shall be the lowest-priority time source, below GNSS, NTP and DCF77. | A higher source governs whenever valid; the RTC governs only when none is. |

---

## 9. Functional Requirements — GNSS Subsystem

### 9.1 Enable / Disable

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-GPS-001 | GNSS use shall be enabled or disabled by the user, and the setting shall persist. | Toggling GNSS on/off persists across reboot (FR-WEB-025..027). |
| FR-GPS-002 | When GNSS is disabled, the receiver shall not be used and shall consume no resources. | With GNSS off, no GNSS activity occurs. |

### 9.2 Data Input

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-GPS-003 | The clock shall obtain UTC time, fix validity and position from the GNSS receiver. | Valid GNSS data yields time, fix status and latitude/longitude. |
| FR-GPS-004 | The clock shall use the GNSS one-pulse-per-second output as a precise tick reference. | The GNSS pulse is detected and used for the seconds tick. |

### 9.3 Time Override

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-GPS-005 | GNSS time shall be used only when the receiver reports a good, current fix. | Time is taken from GNSS only while the fix is valid and recent. |
| FR-GPS-006 | A valid GNSS fix shall override NTP time. | With a GNSS fix, displayed time follows GNSS over NTP. |
| FR-GPS-007 | On loss of the GNSS fix the clock shall fall back automatically to the next source. | After fix loss the clock reverts to NTP/internal without user action. |

### 9.4 Location Data

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-GPS-008 | A valid GNSS position shall be the primary location for DST resolution (Section 10). | DST timezone is derived from GNSS position when available. |
| FR-GPS-009 | The last known GNSS position shall be retained across power cycles as a location hint. | After a power-cycle, the last position is available before a new fix. |

---

## 10. Functional Requirements — Daylight Saving Time

### 10.1 DST Source Selection

The DST engine selects a source according to the priority hierarchy in Section 5.5. GNSS coordinates or IP geolocation resolve the timezone and DST rules for any location; where the configured display timezone is Central European, the DCF77 CET/CEST flag (Z1/Z2) may instead supply the DST state directly, without location or internet. If none of these is available, no DST offset is applied and the UTC base offset is used as-is. DCF77 conveys no geographic location, so it cannot resolve an arbitrary timezone.

### 10.2 DST Requirements

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DST-001 | The clock shall determine the timezone and DST rules from the highest-priority available source (Section 5.5). | DST/timezone is resolved from GNSS, else IP geolocation, else the DCF77 CET/CEST flag. |
| FR-DST-002 | DST transitions shall be applied automatically at the correct local time, without user intervention. | At a changeover the local display shifts by one hour at the right moment, unattended. |
| FR-DST-003 | A DST transition shall not cause a skipped or repeated second on the meters. | At the changeover the seconds meter runs smoothly with no visible glitch. |
| FR-DST-004 | When location comes from IP geolocation, it shall be refreshed on reconnection and periodically. | Geolocation is re-checked after a network reconnect and at least daily. |
| FR-DST-005 | The DST state, active offset and DST source shall be shown on the status page (FR-WEB-030). | Status page shows DST active/inactive, the offset and the source. |
| FR-DST-006 | DST shall work without a live internet connection at the moment of transition. | With the network down at the changeover, DST still applies correctly. |

---

## 11. Functional Requirements — Network and Connectivity

### 11.1 Station Mode (STA)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-NW-001 | The clock shall connect to a WiFi network using configured credentials. | With valid SSID/password set, the clock connects. |
| FR-NW-002 | The clock shall obtain its network address automatically. | The clock gets an address via DHCP; no manual IP needed. |
| FR-NW-003 | If the connection is lost, the clock shall reconnect automatically, backing off between attempts. | After WiFi drops, the clock reconnects on its own when the network returns. |
| FR-NW-004 | WiFi credentials shall be updatable via the web GUI (FR-WEB-020..023). | New credentials entered in the GUI take effect. |

### 11.2 Access Point Fallback (AP Mode)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-NW-005 | If it cannot join a network within a configurable timeout, the clock shall start its own access point. | With no joinable network, the clock exposes its own AP after the timeout. |
| FR-NW-006 | The fallback access point shall be open (no password). | A client can join the fallback AP without a password. |
| FR-NW-007 | The fallback AP name shall be unique per device. | The AP name includes a device-unique suffix (e.g. PanelClock-3FA2). |
| FR-NW-008 | When a network connection is established, the fallback AP shall close automatically. | On joining a network, the fallback AP disappears. |
| FR-NW-009 | The full web GUI shall be available over the fallback AP. | All GUI pages work for a client on the fallback AP. |

### 11.3 Network Discovery

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-NW-010 | The clock shall be reachable by a fixed friendly name on the local network. | The clock resolves as `panelclock.local` on the LAN. |
| FR-NW-011 | The friendly name shall be advertised while on a network and withdrawn when disconnected. | Name resolves when connected; not during AP-only mode. |
| FR-NW-012 | The clock's web service shall be discoverable without knowing its IP address. | A discovery browser finds the clock's web service by name. |
| FR-NW-013 | The clock shall present its hostname to the network so routers can resolve it. | The clock appears under its hostname on the router/LAN. |

---

## 12. Functional Requirements — Web GUI

### 12.1 General Web Server

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-WEB-001 | The clock shall serve a web GUI over HTTP. | The GUI loads in a browser at the clock's address. |
| FR-WEB-002 | Every page shall be reachable from a persistent navigation menu. | The nav menu is present and works on every page. |
| FR-WEB-003 | The menu shall link to Calibration, WiFi, GNSS, DCF77, Status and Firmware Update. | All six pages are reachable from the menu. |
| FR-WEB-004 | The GUI shall be fully self-hosted, with no external/internet dependencies. | The GUI loads and works with no internet access. |
| FR-WEB-005 | The GUI shall work in any standard browser without plug-ins or installation. | Pages function in a current mainstream browser with no add-ons. |

### 12.2 Page: Meter Calibration (`/calibrate`)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-WEB-010 | The calibration page shall provide independent controls for each of the three meters. | Hours, minutes and seconds each have their own calibration controls. |
| FR-WEB-011 | The user shall be able to set each meter's zero position. | Setting a meter's zero moves its needle to the zero mark. |
| FR-WEB-012 | The user shall be able to set each meter's full-scale position. | Setting a meter's full scale moves its needle to full scale. |
| FR-WEB-013 | The page shall offer a live preview that moves a meter without saving. | The preview drives the meter; the stored calibration is unchanged until saved. |
| FR-WEB-014 | Saving shall store the calibration and apply it to the live display immediately. | After Save, the meter uses the new calibration and it persists. |
| FR-WEB-015 | A reset-to-defaults action shall restore factory calibration for all meters. | Reset returns all meters to factory calibration. |

### 12.3 Page: WiFi Configuration (`/wifi`)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-WEB-020 | The page shall show nearby WiFi networks. | A scan list of networks is shown on load. |
| FR-WEB-021 | The user shall be able to pick a network from the list or enter one manually. | Either method selects the target network. |
| FR-WEB-022 | The page shall provide a masked password field. | The password entry is masked by default. |
| FR-WEB-023 | Saving shall store the credentials and start a connection attempt. | After Save, the clock attempts to join the chosen network. |
| FR-WEB-024 | The page shall show the current network state and, when connected, the address. | Page reflects connected/connecting/AP-only and shows the IP when connected. |

### 12.4 Page: GNSS Configuration (`/gnss`)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-WEB-025 | The page shall show whether GNSS is enabled. | Page reflects the current GNSS on/off state. |
| FR-WEB-026 | The user shall be able to enable/disable GNSS, effective without a reboot. | Toggling GNSS takes effect immediately and persists. |
| FR-WEB-027 | When enabled, the page shall show fix status, satellites in use and position. | Page shows fix/no-fix, satellite count and latitude/longitude. |

### 12.5 Page: DCF77 Configuration (`/dcf77`)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-WEB-050 | The page shall show whether DCF77 is enabled. | Page reflects the current DCF77 on/off state. |
| FR-WEB-051 | The user shall be able to enable/disable DCF77, effective without a reboot. | Toggling DCF77 takes effect immediately and persists. |
| FR-WEB-052 | The user shall be able to set the DCF77 signal polarity. | The polarity setting is applied and persists. |
| FR-WEB-053 | When enabled, the page shall show reception and decode status. | Page shows signal-present, decode/lock state and last decoded time. |

### 12.6 Page: Clock Status (`/status`)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-WEB-030 | The status page shall show UTC time, local time, the active offset, and the DST state and source. | Page shows UTC, local, offset, DST on/off and DST source. |
| FR-WEB-031 | The status page shall show the active time source and tick source. | Page shows time source (GNSS/NTP/DCF77/RTC) and tick source. |
| FR-WEB-032 | The status page shall show NTP status. | Page shows NTP server, last sync, quality and next sync. |
| FR-WEB-033 | The status page shall show the network-sync retry count and last attempt. | Page shows retry count and last-attempt time. |
| FR-WEB-034 | The status page shall refresh its live data automatically without a full reload. | Displayed values update periodically on their own. |
| FR-WEB-035 | The status page shall show DCF77 reception and last decoded time. | Page shows DCF77 signal/decode state and last decoded frame time. |

### 12.7 Page: Firmware Update (`/update`)

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-WEB-040 | The page shall let the user upload a firmware package. | A firmware file can be selected and uploaded. |
| FR-WEB-041 | Uploaded firmware shall be authenticated before any of it is installed. | Verification happens before any flash write (FR-SEC-001..005). |
| FR-WEB-042 | If authentication fails, the upload shall be rejected with a clear error and the running firmware left unchanged. | An altered package is refused, an error is shown, the clock keeps the old firmware. |
| FR-WEB-043 | A valid update shall be installed safely so a failed update cannot brick the clock. | A valid package installs and boots; an interrupted update leaves the clock bootable. |
| FR-WEB-044 | The page shall show upload/installation progress. | A progress indicator advances during the update. |
| FR-WEB-045 | On success the page shall confirm and tell the user the device will restart. | A success message and restart notice are shown. |
| FR-WEB-046 | The device shall restart automatically into the new firmware after a successful update. | The clock reboots into the updated firmware. |

---

## 13. Functional Requirements — Security and FOTA Authentication

### 13.1 Authenticity

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-SEC-001 | The clock shall hold a trusted key for verifying firmware authenticity that cannot be overwritten by an update. | The verification key is present and survives firmware updates. |
| FR-SEC-002 | A firmware update package shall carry a digital signature for verification. | Update packages include a signature the clock can check. |
| FR-SEC-003 | The secret signing key shall never reside on the device. | Only the public verification key is on the device. |

### 13.2 Verification

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-SEC-004 | Firmware shall be fully verified before any of it is written. | No part of an update is installed until its signature passes. |
| FR-SEC-005 | Only firmware with a valid signature shall be accepted. | A correctly signed package is accepted; an unsigned or altered one is rejected. |
| FR-SEC-006 | A failed verification shall be logged with a human-readable reason. | A rejected update shows a clear reason on the log and status page. |

### 13.3 Key Rotation

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-SEC-007 | It shall be possible to replace the verification key via a signed update. | A signed update can install a new verification key; subsequent updates use it. |

---

## 14. Functional Requirements — DCF77 Subsystem

The DCF77 subsystem provides an offline radio time reference derived from the German DCF77 longwave time signal. In the time-source priority hierarchy (Section 5.4) it sits at **Priority 3** — below GNSS (Priority 1) and NTP (Priority 2), and above the RTC (Priority 4). It supplies UTC time; it provides no geographic location, though its CET/CEST flag can serve as a DST source for the Central European zone (Section 5.5).

### 14.1 Enable / Disable

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DCF-001 | DCF77 use shall be enabled or disabled by the user, and the setting shall persist. | Toggling DCF77 on/off persists across reboot (FR-WEB-050..053). |
| FR-DCF-002 | DCF77 shall be off by default, and when off the receiver shall not be used and shall consume no resources. | Out of the box DCF77 is disabled; when off, no DCF77 activity occurs. |

### 14.2 Signal Decoding

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DCF-003 | The clock shall decode the DCF77 time signal to recover the broadcast date and time. | With a usable signal, the clock recovers the current date and time. |
| FR-DCF-004 | The clock shall accept either signal polarity of the receiver output. | Decoding works regardless of the receiver's output polarity (configurable). |
| FR-DCF-005 | Decoding shall recover the full date and time plus the CET/CEST (summer-time) indication. | Decoded data includes date, time and whether CET or CEST is in force. |
| FR-DCF-006 | Corrupted or incomplete frames shall be detected and rejected. | Frames failing the signal's built-in checks are discarded, not used. |

### 14.3 Time Validity and Override

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DCF-007 | DCF77 time shall be accepted only after it has been confirmed by consecutive consistent frames. | A single noisy frame is not trusted; time is used only after confirmation. |
| FR-DCF-008 | DCF77 time shall set the clock only when neither GNSS nor NTP is available, and shall never override them. | With GNSS/NTP present, DCF77 does not change the time; with neither, DCF77 sets it. |
| FR-DCF-009 | The clock shall convert the broadcast CET/CEST time to UTC before use. | Internal time is correct UTC, accounting for the CET or CEST offset. |

### 14.4 Loss of Reception

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DCF-010 | Loss of DCF77 reception shall not disturb the displayed time. | When the signal drops, the clock keeps running smoothly and falls back per Section 5.4. |

### 14.5 DST-Change Handling

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DCF-011 | The clock shall follow the DCF77 CET↔CEST change so that UTC stays continuous across it. | At a CET↔CEST changeover, internal UTC has no step or gap. (Display DST per Section 10.) |

### 14.6 Tick Reference

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| FR-DCF-012 | The DCF77 per-second mark shall be usable as the seconds tick when no GNSS pulse is available. | With GNSS absent and DCF77 received, the seconds tick follows DCF77; gaps fall back smoothly (FR-BOOT-016). |

---

## 15. Non-Functional Requirements

### 15.1 Performance

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| NFR-PERF-001 | The meters shall update promptly on each tick. | All three meters reach the new value within ~10 ms of the tick. |
| NFR-PERF-002 | Web GUI pages shall load quickly. | Each page loads within a few seconds over typical WiFi. |
| NFR-PERF-003 | Time synchronisation shall not disturb the display. | The display keeps ticking smoothly during NTP/GNSS/DCF77 activity. |

### 15.2 Reliability

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| NFR-REL-001 | The clock shall recover automatically from a software hang. | If firmware stops responding, the clock resets itself within ~30 s. |
| NFR-REL-002 | After an automatic reset the clock shall start up normally. | Following a watchdog reset the clock boots and resumes timekeeping. |
| NFR-REL-003 | Corrupted stored settings shall be detected and safely reset to defaults. | On corrupt settings the clock restores defaults, logs it, and still boots. |

### 15.3 Power and Electrical

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| NFR-PWR-001 | On power-down or reset the meters shall fall to zero, not stick mid-scale. | After power-off/reset the needles return to zero promptly. |

### 15.4 Maintainability

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| NFR-MNT-001 | Operational settings shall be changeable at runtime without rebuilding the firmware. | NTP server, intervals and timeouts can be changed via the GUI/config and take effect without reflashing. |
| NFR-MNT-002 | The clock shall provide a diagnostic log of key events. | A diagnostic log records boot, time-source changes, sync events, DST changes and updates. |

### 15.5 Portability

| ID | Functional Requirement | Acceptance / Verification Criteria |
|----|------------------------|------------------------------------|
| NFR-PORT-001 | Hardware-specific meter drive shall be isolated behind a defined interface so the timekeeping/display logic is portable. | Display/timekeeping logic has no direct hardware dependencies; the drive layer is replaceable. |

---

## 16. Appendices

### Appendix A — Requirements Traceability Matrix

| Requirement ID | Feature Area | Test Case | Notes |
|----------------|-------------|-----------|-------|
| FR-BOOT-001..004 | Boot / Phase 1 | TC-BOOT-001 | |
| FR-BOOT-005..010 | Boot / Phase 2 / NTP | TC-BOOT-002 | |
| FR-BOOT-011..017 | Boot / Phase 3 / GNSS | TC-BOOT-003 | |
| FR-BOOT-018..021 | Boot / DCF77 monitoring | TC-BOOT-004 | |
| FR-DSP-001..014 | Display / meters | TC-DSP-001 | |
| FR-TIM-001..008 | Timekeeping | TC-TIM-001 | |
| FR-RTC-001..006 | Real-time clock (RTC) | TC-RTC-001 | |
| FR-NTP-001..004 | NTP sync | TC-NTP-001 | |
| FR-GPS-001..009 | GNSS subsystem | TC-GPS-001 | |
| FR-DCF-001..012 | DCF77 subsystem | TC-DCF-001 | |
| FR-DST-001..006 | DST engine | TC-DST-001 | |
| FR-NW-001..009 | Network / WiFi | TC-NW-001 | |
| FR-NW-010..013 | Network / mDNS | TC-NW-002 | |
| FR-WEB-001..053 | Web GUI | TC-WEB-001..006 | One test case per page |
| FR-SEC-001..007 | FOTA security | TC-SEC-001 | |
| NFR-* | Non-functional | TC-NFR-001 | Performance, watchdog |

*Each requirement's acceptance criterion is stated inline in Sections 6–15. The corresponding test methods and pass conditions (test cases TC-*) are specified in PMC-STD-001 §8 (Verification and Test Criteria) and PMC-HTD-001.*

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

All technical issues are resolved or closed; the decisions are recorded in the Technical Design (PMC-STD-001, PMC-HTD-001).
