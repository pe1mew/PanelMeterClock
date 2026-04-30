# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [Unreleased] — 2026-04-30

### Added
- `Design/hardwareTechnicalDesign.md` §6.0 — Quectel L76-M33 module specification table (constellations, supply, baud rates, 1PPS, dimensions, TTFF, sensitivity)
- `Design/hardwareTechnicalDesign.md` §6.6 — VBAT backup power subsection: MS621FE coin cell or supercapacitor options for hot-start capability
- `Documentation/` — Quectel L76-M33 hardware design guide and product specification (three documents)

### Changed
- `Design/hardwareTechnicalDesign.md` §6.1 — signal overview table updated with L76-M33 pin names and VBAT row
- `Design/hardwareTechnicalDesign.md` §6.2 — level shifting confirmed not required for L76-M33 (3.3 V native)
- `Design/hardwareTechnicalDesign.md` §6.3 — UART section updated with PMTK baud-rate change command note
- `Design/hardwareTechnicalDesign.md` §6.4 — 1PPS pulse width referenced to L76-M33 spec (100 ms default)
- `Design/hardwareTechnicalDesign.md` §6.5 — antenna section updated: U.FL on-module connector, U.FL-to-SMA pigtail (W1) to chassis connector J1
- `Design/hardwareTechnicalDesign.md` §8 — power table updated with L76-M33 acquisition (18 mA) and tracking (15 mA) current; total system estimate added
- `Design/hardwareTechnicalDesign.md` §9 — U2 updated to Quectel L76-M33; BT1 (MS621FE backup cell) and W1 (U.FL-to-SMA pigtail) added
- `Design/hardwareTechnicalDesign.md` §10 — HW-001 closed (Quectel L76-M33 selected)
- `Design/functionalRequirementsSpecification.md` §4.1 — Quectel L76-M33 datasheet added to hardware references
- `Design/softwareTechnicalDesign.md` §5.6 — GNSS module identified as Quectel L76-M33
- `README.md` — hardware table updated with Quectel L76-M33 and antenna pigtail

### Changed
- `Design/hardwareTechnicalDesign.md` §4.3 — series resistor formula corrected to include R_filter (1 kΩ) in the DC series chain; measured values added (I_FSD = 1 mA, V_FSD = 82.2 mV, R_coil = 82.2 Ω); calculated R_series = 1917.8 Ω → specified as 2.0 kΩ E24
- `Design/hardwareTechnicalDesign.md` §9 — R4–R6 value updated to 2.0 kΩ ±1 % metal film
- `Design/hardwareTechnicalDesign.md` §10 — HW-002 closed (series resistor resolved by measurement); HW-003 dropped; HW-004 resolved as SMA female chassis connector
- `Design/hardwareTechnicalDesign.md` §6.5 — antenna subsection added: SMA female chassis-mount connector (J1)
- Repository root files rewritten to reflect PanelMeterClock project (previously contained Greenhouse Ventilation Controller content)

---

## [Unreleased] — 2026-04-29

### Added
- `Design/functionalRequirementsSpecification.md` — PMC-FRS-001 v0.1: full functional requirements specification covering boot phases, display subsystem, timekeeping, NTP, GNSS, DST, network/WiFi, web GUI, FOTA security, non-functional requirements, interface requirements, and design constraints
- `Design/softwareTechnicalDesign.md` — PMC-STD-001 v0.1: software technical design covering target platform, build system, FreeRTOS task architecture, module design (boot state machine, PWM driver, timekeeping, NTP client, WiFi/mDNS manager, GNSS, DST engine, HTTP server), NVS key inventory, named constants, and open technical issues
- `Design/hardwareTechnicalDesign.md` — PMC-HTD-001 v0.1: hardware technical design covering microcontroller module, GPIO pin assignment, panel meter drive circuit (series resistor modification, RC filter analysis, PWM-to-voltage mapping), LEDC configuration, GNSS interface, serial debug interface, power supply, component list, and open hardware issues
- `Design/signatureConciderations.md` — analysis of detached vs embedded FOTA signature schemes, verification process, four single-file alternatives, and comparison table

### Changed
- `Design/functionalRequirementsSpecification.md` — hardware detail and design appendices migrated to PMC-HTD-001 and PMC-STD-001:
  - §5.1 product description simplified; GPIO/LEDC table columns moved to HTD
  - §5.6 hardware constraints summary replaced with reference to HTD
  - IC-HW-001..004 simplified; component values and pin assignments reference HTD
  - DC-003 simplified; series resistor calculation detail moved to HTD §4.3
  - Appendices B, C, D, E replaced with references to STD sections
  - FR-DSP-006 duty formula replaced with behavioural description referencing STD §5.2
  - §4.3 references: HTD and STD added

### Added (network requirements)
- `Design/functionalRequirementsSpecification.md` §11.3 — mDNS requirements FR-NW-010..013: fixed hostname `panelclock` (resolves as `panelclock.local`), STA mode only, DNS-SD HTTP service advertisement (`_http._tcp`, port 80), DHCP client hostname (Option 12)
- `Design/functionalRequirementsSpecification.md` Appendix E — NVS key `clock/mdns_hostname` (default `"panelclock"`)
- `Design/functionalRequirementsSpecification.md` Appendix A — traceability row FR-NW-010..013 / TC-NW-002

### Resolved (open technical issues)
- TBD-001 — GNSS 1PPS GPIO: **GPIO 10**
- TBD-002 — GNSS UART: **UART1, GPIO 18 RX / GPIO 21 TX, 9600 baud (configurable via NVS)**
- TBD-005 — IP geolocation service: **ip-api.com (primary), worldtimeapi.org (fallback)**
- TBD-006 — Timezone library: **custom POSIX TZ string parser; rule stored in NVS key `clock/posix_tz`**
- TBD-007 — HTTP server library: **ESPAsyncWebServer + AsyncTCP**

---

## [0.1.0] — 2026-04-28

*Project initialised.*

### Added
- `Research/PWMDriver/` — PlatformIO project: production-quality PWM driver for three LEDC channels; FreeRTOS-aware (per-instance mutex, glitch-free duty updates via direct register writes)
- `Research/PWMTest/` — PlatformIO project: standalone 80 kHz PWM verification with potentiometer-controlled frequency and duty cycle
- `Research/PWMDriver.md` — PWM driver design rationale and API documentation
- `Research/PWMTest.md` — RC filter and PWM frequency verification plan
- `Hardware/Scale/` — panel meter scale face design files
- `Documentation/` — ESP32-S3-WROOM-1 datasheet, ESP32-S3 technical reference manual, LOLIN S3 schematic and dimensions
- `code_of_conduct.md`, `contributing.md` — community standards
- `LICENSE`, `license.md` — dual licence: source-available non-commercial (software) and CC BY-NC-ND 4.0 (hardware, documentation, images)
