# PanelMeterClock

A wall clock that displays the current local time on three Siemens 1604P moving-coil panel meters, driven by an ESP32-S3. Hours, minutes, and seconds each have a dedicated meter whose needle deflects proportionally to the time value. Time is obtained via NTP over WiFi, with optional GNSS disciplining for higher accuracy. The device is configured entirely through an embedded web GUI.

## Features

- Three-meter display: hours (0–24), minutes (0–60), seconds (0–60)
- NTP synchronisation over WiFi (`pool.ntp.org` default, configurable)
- Optional GNSS receiver for hardware 1PPS disciplining and precise UTC
- Automatic Daylight Saving Time from GNSS coordinates or IP geolocation
- Embedded web GUI: meter calibration, WiFi config, GNSS config, status, firmware update
- Firmware Over The Air (FOTA) with asymmetric-key signature verification
- mDNS registration (`panelclock.local`) for zero-configuration browser access
- WiFi AP fallback (`PanelClock-XXYY`) when no network credentials are stored
- FreeRTOS firmware on ESP-IDF 5.x / Arduino framework

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | WEMOS LOLIN S3 (ESP32-S3-WROOM-1, dual-core 240 MHz, 16 MB flash, 8 MB PSRAM) |
| Panel meters | 3 × Siemens 1604P, 1 V FSD, modified to 3 V FSD by series resistor substitution |
| PWM filter | RC low-pass filter per channel: R = 1 kΩ, C = 10 µF, f_c ≈ 16 Hz |
| Series resistor | 2.0 kΩ ±1 % metal film per meter (measured: I_FSD = 1 mA, R_coil = 82.2 Ω) |
| GNSS receiver | Optional; Quectel L76-M33 (GPS/GLONASS/Galileo/BeiDou, 3.3 V, NMEA 0183, 1PPS, U.FL) |
| GNSS antenna | SMA female chassis connector (J1) via U.FL-to-SMA pigtail; external active or passive antenna |
| Power supply | 5 V via USB-C (LOLIN S3 on-board regulator) |

## Repository Structure

```
PanelMeterClock/
│
├── Design/                              ← Project design documents
│   ├── functionalRequirementsSpecification.md   ← PMC-FRS-001: what the system shall do
│   ├── softwareTechnicalDesign.md               ← PMC-STD-001: firmware architecture
│   ├── hardwareTechnicalDesign.md               ← PMC-HTD-001: circuit design and BOM
│   ├── signatureConciderations.md               ← FOTA signature scheme analysis
│   └── notes.md                                 ← Design trade-off notes
│
├── Documentation/                       ← Component datasheets and reference material
│   ├── esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf
│   ├── esp32-s3_technical_reference_manual_en.pdf
│   ├── dim_s3_v1.0.0.pdf
│   └── sch_s3_v1.0.0.pdf
│
├── Hardware/                            ← Hardware design files
│   └── Scale/                           ← Panel meter scale face designs
│
├── Research/                            ← Standalone PlatformIO validation projects
│   ├── PWMDriver/                       ← PWM driver library (production-ready)
│   │   ├── platformio.ini
│   │   └── src/
│   │       ├── main.cpp
│   │       ├── pwm_driver.h
│   │       └── pwm_driver.cpp
│   ├── PWMTest/                         ← PWM frequency and RC filter verification
│   │   ├── platformio.ini
│   │   └── src/
│   │       └── main.cpp
│   ├── PWMDriver.md                     ← PWM driver design and API documentation
│   └── PWMTest.md                       ← RC filter and frequency verification plan
│
├── README.md
├── LICENSE
├── license.md
├── changelog.md
├── contributing.md
└── code_of_conduct.md
```

## Getting Started

### Prerequisites

- [Visual Studio Code](https://code.visualstudio.com/) with the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- Git

### Build and Flash (Research Projects)

The main firmware application is not yet implemented. The `Research/PWMDriver` project validates the PWM driver and RC filter circuit.

1. Clone the repository:
   ```sh
   git clone https://github.com/pe1mew/PanelMeterClock.git
   cd PanelMeterClock
   ```
2. Open `Research/PWMDriver/` in VS Code (PlatformIO detects `platformio.ini` automatically).
3. Connect the LOLIN S3 board via USB-C.
4. Click **Upload** in the PlatformIO toolbar, or run:
   ```sh
   pio run -t upload
   ```
5. Open the **Serial Monitor** at 115200 baud to view PWM and voltage readouts.

### Configuration

The main application is configured through the embedded web GUI at `http://panelclock.local/` once the device is connected to WiFi. On first boot, connect to the fallback AP (`PanelClock-XXYY`) and open `http://192.168.4.1/wifi` to enter WiFi credentials.

## Documentation

| Document | Description |
|----------|-------------|
| [Functional Requirements Specification](Design/functionalRequirementsSpecification.md) | What the system shall do — all functional and non-functional requirements |
| [Software Technical Design](Design/softwareTechnicalDesign.md) | Firmware architecture, FreeRTOS tasks, module design, library choices |
| [Hardware Technical Design](Design/hardwareTechnicalDesign.md) | Circuit design, component values, GPIO assignment, BOM |
| [FOTA Signature Considerations](Design/signatureConciderations.md) | Detached signature scheme analysis and alternatives |
| [PWM Driver Design](Research/PWMDriver.md) | PWM driver API and glitch-free duty update design |
| [PWM Test Plan](Research/PWMTest.md) | RC filter and 80 kHz frequency verification |

## License

See [license.md](license.md) for full details.

**Software** (firmware and all code): Source-available, non-commercial. Free to use and modify for personal and non-commercial purposes; redistribution and commercial use are not permitted.

**Hardware design, documentation, and images**: Licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.

<a rel="license" href="https://creativecommons.org/licenses/by-nc-nd/4.0/"><img alt="Creative Commons License" style="border-width:0" src="https://i.creativecommons.org/l/by-nc-nd/4.0/88x31.png" /></a>

## Disclaimer

This project is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
