# Software Technical Design

## PanelMeterClock Firmware

---

## 1. Document Control

### 1.1 Identification

| Field | Value |
|-------|-------|
| Document title | Software Technical Design — PanelMeterClock Firmware |
| Document ID | PMC-STD-001 |
| Version | 0.1 (draft) |
| Date | 2026-04-29 |
| Author | Remko Welling |
| Status | Draft — under review |

### 1.2 Revision History

| Version | Date | Author | Change Summary |
|---------|------|--------|----------------|
| 0.1 | 2026-04-29 | Remko Welling | Initial draft; content migrated from PMC-FRS-001 appendices and supplemented with architecture design |

### 1.3 Relationship to the FRS

This document is the companion technical design to **PMC-FRS-001 (Functional Requirements Specification)**. The FRS defines *what* the firmware shall do; this document defines *how* it is structured and implemented. Requirements are identified by their FRS identifiers (e.g., FR-DSP-006).

---

## 2. Target Platform

| Attribute | Value |
|-----------|-------|
| Module | WEMOS LOLIN S3 (ESP32-S3-WROOM-1-N16R8) |
| CPU | Xtensa LX7 dual-core, 240 MHz |
| Flash | 16 MB quad SPI (N16) |
| RAM | 512 KB SRAM + 8 MB octal PSRAM (R8) |
| Framework | Arduino on ESP-IDF 5.x |
| Build system | PlatformIO |
| RTOS | FreeRTOS (provided by ESP-IDF Arduino core) |

---

## 3. Build System

### 3.1 `platformio.ini`

```ini
[env:lolin_s3]
platform         = espressif32
board            = lolin_s3
board_build.arduino.memory_type = qio_opi   ; N16R8: 16 MB quad flash + 8 MB octal PSRAM
framework        = arduino
monitor_speed    = 115200
board_build.partitions = partitions_ota.csv
lib_deps =
    https://github.com/me-no-dev/AsyncTCP
    https://github.com/me-no-dev/ESPAsyncWebServer
```

### 3.2 Library Dependencies

| Library | Source | Purpose | Rationale |
|---------|--------|---------|-----------|
| `ESPmDNS` | Built-in (ESP32 Arduino core) | mDNS / DNS-SD registration (FR-NW-010..013) | No extra dependency; ships with the core |
| `WiFi` | Built-in (ESP32 Arduino core) | STA and AP mode (FR-NW-001..009) | Ships with the core |
| `ESPAsyncWebServer` + `AsyncTCP` | GitHub (me-no-dev) | Embedded HTTP server and OTA streaming (FR-WEB-001..053) | Non-blocking I/O keeps server responsive during flash writes (resolved: TBD-007) |
| `Preferences` | Built-in (ESP32 Arduino core) | NVS key-value storage (FR-DSP-010..012, FR-NW-001) | Thin C++ wrapper over ESP-IDF NVS |
| Custom POSIX TZ parser | In-tree (`src/posix_tz.h`) | DST rule engine (FR-DST-001..006) | POSIX TZ strings are compact, updatable via NVS without firmware rebuild (resolved: TBD-006) |
| GNSS NMEA parser | TBD — TinyGPS++ or custom | NMEA sentence parsing (FR-GPS-003) | Lightweight; parses $GPRMC/$GNRMC only |
| DCF77 time-code decoder | In-tree (`src/dcf77.h`) | DCF77 frame decode, validation and DST bits (FR-DCF-003..011) | Simple edge-timing decoder; no external library |
| RTC driver (DS1307) | RTClib or in-tree (`src/ds1307.h`) | Battery-backed RTC over I2C (FR-RTC-001..006) | BCD register read/write at I2C address 0x68 |
| mbedTLS | Built-in (ESP-IDF) | Ed25519 / RSA-2048 for FOTA signing (FR-SEC-001..005) | Already present in ESP-IDF; no extra flash cost |

### 3.3 Partition Table (`partitions_ota.csv`)

Two OTA application partitions are required (DC-005). Suggested layout:

```
# Name,   Type, SubType, Offset,  Size,    Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1E0000,
app1,     app,  ota_1,   0x1F0000,0x1E0000,
spiffs,   data, spiffs,  0x3D0000,0x30000,
```

The `spiffs` partition holds web GUI assets (HTML, CSS, JS). The public key for FOTA verification (FR-SEC-001) is embedded in the firmware binary at build time; it is not a separate partition.

---

## 4. FreeRTOS Task Architecture

All application work runs in FreeRTOS tasks. The Arduino `loop()` function is left permanently blocked (`vTaskDelay(portMAX_DELAY)`).

| Task name | Core | Priority | Stack (bytes) | Responsibility |
|-----------|------|----------|---------------|----------------|
| `display_task` | APP (1) | 10 | 4096 | Drives the three PWM meters; executes once per 1PPS tick event (FR-DSP-007) |
| `tick_task` | APP (1) | 9 | 2048 | Maintains UTC epoch; synthesises software 1PPS via FreeRTOS timer; arbitrates tick and epoch source; seeds from and writes back the RTC (FR-TIM-001..008, FR-RTC-001..006, FR-BOOT-016) |
| `ntp_task` | PRO (0) | 5 | 4096 | Issues NTP queries; disciplines the epoch (FR-NTP-001..004, FR-BOOT-006) |
| `wifi_task` | PRO (0) | 6 | 4096 | STA connect/reconnect with exponential back-off; AP fallback; mDNS start/stop (FR-NW-001..013) |
| `gnss_task` | PRO (0) | 7 | 4096 | UART read; NMEA parsing; 1PPS interrupt latch (FR-GPS-001..009); only created when GNSS is enabled |
| `dcf77_task` | PRO (0) | 7 | 3072 | DCF77 time-code edge capture, frame decode and validation, per-second tick (FR-DCF-001..012); only created when DCF77 is enabled |
| `http_task` | PRO (0) | 4 | 8192 | Embedded web server; serves all GUI pages (FR-WEB-001..053) |
| `ui_task` | APP (1) | 8 | 2048 | Drives the four status LEDs and services the rotary encoder / time-set (PMC-GUI-001) |

**Inter-task communication:**

| Signal | Mechanism | Produced by | Consumed by |
|--------|-----------|-------------|-------------|
| 1PPS tick event | `xTaskNotifyGive` | `tick_task` (software) or GNSS ISR (hardware) | `display_task` |
| UTC epoch update | Shared `volatile uint64_t` + `portENTER_CRITICAL` | `tick_task`, `ntp_task`, `gnss_task`, `dcf77_task` | All |
| Tick source selection | Atomic flag (`volatile uint8_t`) | `wifi_task`, `gnss_task`, `ntp_task`, `dcf77_task` | `tick_task` |
| Epoch source selection | Atomic flag (`volatile uint8_t`) | `gnss_task`, `ntp_task`, `dcf77_task` | `tick_task` |
| Config change | FreeRTOS event group | `http_task` | `wifi_task`, `ntp_task`, `gnss_task`, `dcf77_task` |

---

## 5. Module Design

### 5.1 Boot State Machine

The firmware progresses through three phases (FR-BOOT-001..017). The table below is the authoritative state-transition design.

| Current state | Event / guard | Next state | Actions on transition |
|--------------|---------------|------------|----------------------|
| *(start)* | Power-on | `PHASE_1` | Initialise epoch to 0; display 00:00:00; start 1 Hz software tick in `tick_task` |
| `PHASE_1` | Always (immediately) | `PHASE_2` | Create `wifi_task`; begin STA association attempt |
| `PHASE_2` | Always (if GNSS enabled) | `PHASE_3` | Create `gnss_task`; start GNSS UART; enable 1PPS GPIO interrupt |
| `PHASE_2` | Always (if DCF77 enabled) | `PHASE_3` | Create `dcf77_task`; power up receiver; enable time-code GPIO interrupt |
| `PHASE_2` | WiFi association success | `PHASE_2` (NTP attempt) | Issue NTP query from `ntp_task`; start mDNS; set DHCP hostname |
| `PHASE_2` | Valid NTP response | `STEADY` (NTP) | Set epoch from NTP; switch tick source to synthesised 1PPS; log sync event (NFR-MNT-002) |
| `PHASE_2` | WiFi or NTP failure | `PHASE_2` (retry) | Increment retry counter; log; wait 15 s then retry (FR-BOOT-008) |
| `PHASE_3` | GNSS fix valid | `STEADY` (GNSS override) | Override epoch from GNSS; switch tick source to hardware 1PPS; log (NFR-MNT-002) |
| `PHASE_3` | GNSS fix lost (> 5 s) | `PHASE_3` (degraded) | Fall back to NTP synthesised 1PPS; log (FR-BOOT-015) |
| `PHASE_3` | DCF77 valid AND no GNSS AND no NTP | `STEADY` (DCF77) | Set epoch from DCF77 (converted to UTC); drive tick from DCF77 second mark, else synthesised 1PPS; log (FR-BOOT-019) |
| `STEADY` (any) | NTP re-sync interval elapsed | `STEADY` | Issue NTP query; step epoch if error > 1 s, else slew (FR-TIM-006) |
| `STEADY` (any) | GNSS fix re-acquired | `STEADY` (GNSS override) | Re-enable hardware 1PPS; update epoch |
| `STEADY` (any) | Watchdog timeout | `PHASE_1` | Hardware reset; boot from scratch (NFR-REL-001..002) |

### 5.2 Display and PWM Driver

**Duty cycle computation** (FR-DSP-006):

```
duty = zero_offset + round( value / range_max × (full_scale − zero_offset) )
```

Where:
- `value` — current time component (0–24 for hours, 0–60 for minutes/seconds)
- `range_max` — 24 (hours) or 60 (minutes/seconds)
- `zero_offset`, `full_scale` — per-meter calibration values from NVS (8-bit, 0–255)
- Result is clamped to [0, 255] before passing to `pwm_driver_set_duty()`

**PWM driver API** (NFR-PORT-001) — the sole interface between the display logic and LEDC hardware:

```cpp
// Research/PWMDriver/src/pwm_driver.h
void pwm_driver_init(pwm_driver_t *drv, int gpio, ledc_channel_t ch,
                     ledc_timer_t tmr, uint32_t freq_hz, uint8_t duty);
void pwm_driver_set_freq(pwm_driver_t *drv, uint32_t freq_hz);
void pwm_driver_set_duty(pwm_driver_t *drv, uint8_t duty);
```

`display_task` blocks on `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` and wakes on each 1PPS event. It applies the UTC-to-local conversion, computes duty for each meter, and calls `pwm_driver_set_duty()` three times. Total latency budget: < 10 ms (NFR-PERF-001).

**GPIO and LEDC assignments:**

| Meter | GPIO | LEDC channel | LEDC timer | Range |
|-------|------|-------------|------------|-------|
| Hours | 15 | `LEDC_CHANNEL_0` | `LEDC_TIMER_0` | 0–24 |
| Minutes | 16 | `LEDC_CHANNEL_1` | `LEDC_TIMER_1` | 0–60 |
| Seconds | 17 | `LEDC_CHANNEL_2` | `LEDC_TIMER_2` | 0–60 |

PWM frequency: 80 000 Hz; resolution: 8-bit; LEDC clock: 80 MHz (DC-004).  
Full-scale duty for 3.0 V output: 232 (= 232/255 × 3.3 V ≈ 3.0 V, FR-DSP-014, DC-003).

### 5.3 Timekeeping Engine

The internal clock is a 64-bit UTC epoch (`uint64_t utc_epoch_s`) plus a sub-second phase accumulator (`int32_t phase_us`).

**Software 1PPS synthesis:** A FreeRTOS timer fires at a nominal 1000 ms period. On each fire, `tick_task` adjusts the timer period by ±1 tick (±1 ms) based on accumulated NTP error, increments the epoch, and notifies `display_task`.

**Hardware 1PPS (GNSS):** A GPIO interrupt ISR calls `vTaskNotifyGiveFromISR` targeting `display_task` directly and increments the epoch from the ISR context via a critical section.

**Tick source arbitration** (FR-BOOT-016..017):

```
Tick mechanism (drives display_task):
  GNSS fix valid?          → hardware GNSS 1PPS ISR      (Priority 1)
  else DCF77 mark present?  → DCF77 per-second-mark ISR   (Priority 2)
  else                     → synthesised software 1PPS   (Priority 3)

Epoch source (sets utc_epoch_s — highest available wins):
  GNSS fix valid    → GNSS UTC                 (Priority 1)
  else NTP synced   → NTP-disciplined epoch    (Priority 2)
  else DCF77 valid  → DCF77 civil time → UTC   (Priority 3)
  else              → RTC (DS1307)             (Priority 4)
```

### 5.4 NTP Client

`ntp_task` sends a single NTPv4 UDP packet (RFC 5905) to the configured server (FR-NTP-001) and waits up to 5 s for a response. On success it computes the offset between the NTP timestamp and the local epoch and applies it:

- Error > 1 s: step (`utc_epoch_s` assigned directly)
- Error ≤ 1 s: slew (adjust FreeRTOS timer period over several seconds)

After the initial sync the task sleeps for `ntp_interval_s` (default 3600 s) before re-querying (FR-NTP-003).

### 5.5 WiFi Manager and mDNS

`wifi_task` owns the WiFi state machine. At startup it reads `wifi_ssid` and `wifi_pass` from NVS and calls:

```cpp
WiFi.setHostname(mdns_hostname);   // DHCP Option 12 (FR-NW-013)
WiFi.begin(ssid, password);
```

**Reconnect back-off** (FR-NW-003): delay starts at 5 s, doubles on each failure, caps at 300 s.

**AP fallback** (FR-NW-005..009): if STA is not associated within `ap_timeout_s` seconds, activate the AP:

```cpp
WiFi.softAP("PanelClock-AABB");   // AABB = last 2 MAC bytes, uppercase hex
```

**mDNS** (FR-NW-010..013): started on `ARDUINO_EVENT_WIFI_STA_GOT_IP`, stopped on `ARDUINO_EVENT_WIFI_STA_DISCONNECTED`. Not active during AP-only mode.

```cpp
// On STA_GOT_IP:
MDNS.begin(mdns_hostname);              // registers panelclock.local
MDNS.addService("http", "tcp", 80);    // DNS-SD advertisement

// On STA_DISCONNECTED:
MDNS.end();
```

`mdns_hostname` defaults to `"panelclock"` (NVS key `clock/mdns_hostname`). It is the same value passed to `WiFi.setHostname()`.

### 5.6 GNSS Subsystem

**Module:** Quectel L76-M33 (GPS/GLONASS/Galileo/BeiDou, 3.3 V, NMEA 0183, 1PPS — see PMC-HTD-001 §6).

`gnss_task` is created only when `gnss_enabled` NVS flag is set (FR-GPS-001..002). It reads NMEA sentences from the GNSS UART, decodes `$GPRMC`/`$GNRMC`, and tracks fix validity. When a valid fix is obtained (FR-GPS-005), it writes the GNSS UTC time to `utc_epoch_s` and signals the tick source arbitrator to switch to hardware 1PPS.

**Hardware assignments (resolved):**

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| 1PPS input | **GPIO 10** | Input, no pull | Rising-edge interrupt; 3.3 V-tolerant (TBD-001 resolved) |
| UART RX | **GPIO 18** | Input | UART1 (`Serial1`) (TBD-002 resolved) |
| UART TX | **GPIO 21** | Output | UART1; leave unconnected if no module config needed |

**UART initialisation:**

```cpp
Serial1.begin(gnss_baud, SERIAL_8N1, GNSS_UART_RX_GPIO, GNSS_UART_TX_GPIO);
```

`gnss_baud` is read from NVS key `clock/gnss_baud` (default 9600). Changing it via the web GUI GNSS page takes effect after `gnss_task` is restarted (no reboot required).

**1PPS interrupt:**

```cpp
attachInterrupt(GNSS_1PPS_GPIO, gnss_pps_isr, RISING);
```

The ISR calls `vTaskNotifyGiveFromISR` to unblock `display_task` and increments `utc_epoch_s` inside a critical section. The interrupt is detached when `gnss_task` is stopped (GNSS disabled).

### 5.7 DST Engine

The DST engine is called once per network reconnection and every 24 hours (FR-DST-004). It selects a location source per the priority hierarchy (FRS Section 5.5) and resolves the applicable UTC offset using the **custom POSIX TZ string parser** (TBD-006 resolved).

**POSIX TZ string approach:** The timezone rule is stored in NVS key `clock/posix_tz` as a standard POSIX TZ string (e.g., `CET-1CEST,M3.5.0,M10.5.0/3` for Central European Time). The parser (`src/posix_tz.h`) computes the current UTC offset and DST state from this string and the UTC epoch, with no external dependencies. The string can be updated via the web GUI or populated automatically from the geolocation response, so timezone rules are updatable without a firmware rebuild.

**IP geolocation (Priority 2, resolved):**

Primary endpoint: `http://ip-api.com/json/?fields=timezone`  
Fallback endpoint: `http://worldtimeapi.org/api/ip`

The firmware tries the primary endpoint first with a 5-second timeout. On failure or non-200 response it falls back to worldtimeapi.org. The `timezone` field from either response (IANA name, e.g., `"Europe/Amsterdam"`) is used to look up the corresponding POSIX TZ string from a compact compile-time table embedded in the firmware. The resolved POSIX TZ string is then written to `clock/posix_tz` in NVS, making it available across reboots even if the internet is subsequently unreachable.

**DST source selection flowchart:**

```
GNSS enabled AND fix valid AND coordinates available?
  YES → use GNSS coordinates → resolve IANA timezone → look up POSIX TZ string → apply (Priority 1)
  NO  → internet reachable?
          YES → GET http://ip-api.com/json/?fields=timezone
                (fallback: http://worldtimeapi.org/api/ip)
                → resolve POSIX TZ string → write to NVS → apply (Priority 2)
          NO  → use posix_tz string already in NVS → apply (Priority 2b, cached)
                if NVS empty → no DST correction; apply UTC base offset only (Priority 3)
```

DST transitions are applied atomically by writing the new UTC offset to a `volatile int32_t utc_offset_s` guarded by a critical section (FR-DST-003).

### 5.8 HTTP Server and Web GUI

`http_task` uses **ESPAsyncWebServer** (TBD-007 resolved). Static assets are served from the SPIFFS partition via `server.serveStatic("/", SPIFFS, "/www/").setDefaultFile("index.html")`. Dynamic data (status page fields) is served via a lightweight JSON API polled by JavaScript (FR-WEB-034).

**Site map:**

```
/ (root — redirects to /status)
├── /status        Clock Status      (FR-WEB-030..035)
├── /calibrate     Meter Calibration (FR-WEB-010..015)
├── /wifi          WiFi Configuration(FR-WEB-020..024)
├── /gnss          GNSS Configuration(FR-WEB-025..027)
├── /dcf77         DCF77 Configuration(FR-WEB-050..053)
└── /update        Firmware Update   (FR-WEB-040..046)
```

All pages share a navigation bar with links to all six endpoints (FR-WEB-002..003).

**FOTA flow** (FR-WEB-040..046):

1. Browser POSTs firmware binary + detached signature file.
2. Server buffers signature; computes SHA-256 digest of the binary stream.
3. Verifies Ed25519/RSA-2048 signature against the embedded public key (FR-SEC-004).
4. On success: writes binary to inactive OTA partition; calls `esp_ota_set_boot_partition()`; schedules reboot in 10 s.
5. On failure: discards data; returns error code to browser (FR-WEB-042).

### 5.9 NVS Storage

All persistent configuration uses the ESP-IDF `Preferences` library (namespace `"clock"` and `"meter"`). Keys, types, and defaults are listed in Section 6 below. NVS corruption is detected at startup by checking the `Preferences.begin()` return value; on corruption the namespace is cleared and factory defaults are written (NFR-REL-003).

### 5.10 DCF77 Subsystem

**Receiver:** A longwave 77.5 kHz DCF77 receiver module with a ferrite-rod antenna and a demodulated, open-collector time-code output (see PMC-HTD-001 §7).

`dcf77_task` is created only when the `dcf77_enabled` NVS flag is set (FR-DCF-001..002). When DCF77 is disabled the receiver is held off via its enable line and the time-code interrupt is detached.

**Signal decoding:** The demodulated signal carries one amplitude-reduced mark per second (≈ 100 ms = binary 0, ≈ 200 ms = binary 1); the mark is omitted in second 59 to delimit the minute. An edge-triggered GPIO interrupt timestamps each edge; `dcf77_task` measures the active-pulse width to recover each bit and assembles the 59-bit minute frame. Bits 17–18 carry the CEST/CET announcement; the minute, hour and date fields are protected by even-parity bits.

**Hardware assignments (resolved):**

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| Time-code input | **GPIO 11** | Input, internal pull-up | Edge interrupt; polarity configurable (FR-DCF-004) |
| Receiver enable (PON) | **GPIO 12** | Output | Drives the module on/off; held off when DCF77 disabled |

**Validation and override:** A DCF77 time is accepted only after two consecutive, parity-valid, time-consistent minute frames (FR-DCF-007). The decoded CET/CEST time is converted to UTC using the time-zone bits Z1/Z2 (FR-DCF-009) and written to `utc_epoch_s` only while no valid GNSS fix and no NTP sync are present (FR-DCF-008). A subsequent GNSS fix or NTP sync overrides it. Loss of reception for longer than the configurable timeout (`DCF77_SIGNAL_LOST_TIMEOUT_S`, default 300 s) leaves the epoch running on the synthesised 1PPS (FR-DCF-010).

**DST change (FR-DCF-011):** Z1/Z2 give the current CET/CEST offset and the A1 bit announces a pending change one hour ahead, so the UTC derived from DCF77 stays continuous across a CET↔CEST transition. Display daylight saving remains governed by the DST engine (§5.7), independent of the DCF77 zone bits.

**Tick reference (FR-DCF-012):** the same edge interrupt that decodes the time-code also yields a once-per-second on-time mark. While GNSS 1PPS is absent and DCF77 marks are arriving, `tick_task` advances the 1-second tick from the DCF77 edge and phase-aligns the synthesised 1PPS to it; reception gaps fall back to the software 1PPS seamlessly.

**Polarity:** `dcf77_invert` (NVS) selects the active edge/level to match the receiver's output stage.

### 5.11 RTC (DS1307)

**Device:** Maxim **DS1307Z** I2C real-time clock (address `0x68`), 32.768 kHz crystal, coin-cell backup on VBAT (PMC-HTD-001 §8). The DS1307 operates at 5 V, so the I2C bus is level-shifted to the ESP32-S3's 3.3 V (PMC-HTD-001 §8).

The RTC is the persistent time-of-day store and the Priority-4 time source:

- **Boot:** `utc_epoch_s` is seeded from the RTC. The DS1307 clock-halt / oscillator-stopped flag is checked; if set (first use or lost backup power) the time is treated as invalid — the clock starts at 00:00:00 and the RTC indicator shows invalid (FR-RTC-005, PMC-GUI-001).
- **Discipline:** whenever GNSS, NTP or DCF77 yields accurate UTC, the corrected time is written back to the DS1307 (FR-RTC-003) so the retained time stays accurate across power cycles.
- **Fallback:** when no higher source is available, the displayed time is kept by the synthesised 1 Hz tick seeded from the RTC and is periodically re-aligned to it.
- **Manual set:** the rotary encoder writes the DS1307 directly (FR-RTC-004, PMC-GUI-001); the value is held on the backup battery.

All times in the DS1307 are stored as **UTC**, consistent with the internal epoch; conversion to local time for display is applied downstream (§5.7).

**Hardware assignment (resolved):** I2C `SDA` = GPIO 8, `SCL` = GPIO 9 (PMC-HTD-001 §3). The DS1307 `SQW/OUT` pin is unused in this design.

### 5.12 Panel Indicators and Control

Implements the physical operator interface specified in PMC-GUI-001; serviced by `ui_task`.

**Status LEDs (UI-IND):** four outputs drive the front-panel RTC/DCF/NTP/GNSS LEDs (GPIO 4–7, push-pull, series resistors). Each LED is off (source disabled/absent), blinking at `UI_BLINK_HZ` (≈ 1 Hz, acquiring) or steady (valid). The RTC LED is steady while the clock runs on the RTC and blinks ≈ 1 Hz when the RTC time is invalid; it blinks at `UI_SET_BLINK_HZ` (≈ 2 Hz) while set mode is active. The active source is the highest-priority lit LED (GNSS > NTP > DCF > RTC).

**Rotary encoder (UI-CTL):** quadrature channels on GPIO 13/14 and a push button on GPIO 47, all inputs with internal pull-ups and firmware debounce. A long press (`UI_LONGPRESS_MS`, 2 s) enters set mode at Hours; rotation adjusts the active field (wrapping); a short press advances Hours → Minutes → Seconds; a short press after Seconds writes the new time to the RTC (§5.11). Inactivity for `UI_SET_TIMEOUT_S` (30 s) exits without applying. While a field is edited the corresponding meter tracks the value. In normal mode the encoder has no effect (reserved).

Pin assignments are in PMC-HTD-001 §3; constants in §7.

---

## 6. NVS Key Inventory

| Namespace | Key | Type | Default | Owning requirement |
|-----------|-----|------|---------|-------------------|
| `clock` | `wifi_ssid` | string | `""` | FR-NW-001 |
| `clock` | `wifi_pass` | string | `""` | FR-NW-001 |
| `clock` | `ap_timeout_s` | uint32 | `60` | FR-NW-005 |
| `clock` | `mdns_hostname` | string | `"panelclock"` | FR-NW-010 |
| `clock` | `ntp_server` | string | `"pool.ntp.org"` | FR-NTP-001 |
| `clock` | `ntp_interval_s` | uint32 | `3600` | FR-NTP-003 |
| `clock` | `gnss_enabled` | uint8 | `0` (disabled) | FR-GPS-001 |
| `clock` | `gnss_baud` | uint32 | `9600` | TBD-002 (resolved) |
| `clock` | `gnss_lat_cache` | float | `0.0` | FR-GPS-009 |
| `clock` | `gnss_lon_cache` | float | `0.0` | FR-GPS-009 |
| `clock` | `dcf77_enabled` | uint8 | `0` (disabled) | FR-DCF-001 |
| `clock` | `dcf77_invert` | uint8 | `0` (non-inverted) | FR-DCF-004 |
| `clock` | `posix_tz` | string | `"CET-1CEST,M3.5.0,M10.5.0/3"` | FR-DST-001, TBD-006 (resolved) |
| `meter` | `h_zero` | uint8 | `0` | FR-DSP-010 |
| `meter` | `h_full` | uint8 | `232` | FR-DSP-011, FR-DSP-014 |
| `meter` | `m_zero` | uint8 | `0` | FR-DSP-010 |
| `meter` | `m_full` | uint8 | `232` | FR-DSP-011, FR-DSP-014 |
| `meter` | `s_zero` | uint8 | `0` | FR-DSP-010 |
| `meter` | `s_full` | uint8 | `232` | FR-DSP-011, FR-DSP-014 |

All key names shall be defined as `constexpr char[]` constants in a single header (`nvs_keys.h`); no literal strings shall appear in application code (NFR-MNT-001).

---

## 7. Named Constants

All magic numbers shall be defined as named constants. The following table lists the most critical ones; the complete set lives in `config.h`.

| Constant | Value | Requirement |
|----------|-------|-------------|
| `PWM_FREQ_HZ` | 80000 | FR-DSP-008 |
| `PWM_RESOLUTION_BITS` | 8 | DC-004 |
| `PWM_FULL_SCALE_DUTY` | 232 | FR-DSP-014, DC-003 |
| `METER_HOURS_GPIO` | 15 | IC-HW-001 |
| `METER_MINUTES_GPIO` | 16 | IC-HW-001 |
| `METER_SECONDS_GPIO` | 17 | IC-HW-001 |
| `NTP_DEFAULT_SERVER` | `"pool.ntp.org"` | FR-NTP-002 |
| `NTP_DEFAULT_INTERVAL_S` | 3600 | FR-NTP-003 |
| `WIFI_AP_TIMEOUT_DEFAULT_S` | 60 | FR-NW-005 |
| `MDNS_DEFAULT_HOSTNAME` | `"panelclock"` | FR-NW-010 |
| `WIFI_RECONNECT_MIN_S` | 5 | FR-NW-003 |
| `WIFI_RECONNECT_MAX_S` | 300 | FR-NW-003 |
| `NTP_RETRY_INTERVAL_S` | 15 | FR-BOOT-008 |
| `WATCHDOG_TIMEOUT_S` | 30 | NFR-REL-001 |
| `GNSS_FIX_LOST_TIMEOUT_S` | 5 | FR-GPS-007 |
| `GNSS_1PPS_GPIO` | 10 | IC-HW-003, TBD-001 (resolved) |
| `GNSS_UART_RX_GPIO` | 18 | IC-HW-004, TBD-002 (resolved) |
| `GNSS_UART_TX_GPIO` | 21 | IC-HW-004, TBD-002 (resolved) |
| `GNSS_DEFAULT_BAUD` | 9600 | TBD-002 (resolved) |
| `DCF77_SIGNAL_GPIO` | 11 | IC-HW-005, TBD-008 (resolved) |
| `DCF77_PON_GPIO` | 12 | IC-HW-006, TBD-009 (resolved) |
| `DCF77_ENABLED_DEFAULT` | 0 | FR-DCF-002 |
| `DCF77_SIGNAL_LOST_TIMEOUT_S` | 300 | FR-DCF-010 |
| `RTC_I2C_ADDR` | 0x68 | IC-HW-007, FR-RTC-001 |
| `I2C_SDA_GPIO` | 8 | IC-HW-007, TBD-010 (resolved) |
| `I2C_SCL_GPIO` | 9 | IC-HW-007, TBD-010 (resolved) |
| `LED_RTC_GPIO` | 4 | IC-HW-008, PMC-GUI-001 |
| `LED_DCF_GPIO` | 5 | IC-HW-008, PMC-GUI-001 |
| `LED_NTP_GPIO` | 6 | IC-HW-008, PMC-GUI-001 |
| `LED_GNSS_GPIO` | 7 | IC-HW-008, PMC-GUI-001 |
| `ENC_A_GPIO` | 13 | IC-HW-009, PMC-GUI-001 |
| `ENC_B_GPIO` | 14 | IC-HW-009, PMC-GUI-001 |
| `ENC_BTN_GPIO` | 47 | IC-HW-009, PMC-GUI-001 |
| `UI_LONGPRESS_MS` | 2000 | PMC-GUI-001 UI-CTL-002 |
| `UI_SET_TIMEOUT_S` | 30 | PMC-GUI-001 UI-CTL-007 |
| `UI_BLINK_HZ` | 1 | PMC-GUI-001 UI-IND |
| `UI_SET_BLINK_HZ` | 2 | PMC-GUI-001 UI-IND-008 |
| `DST_REFRESH_INTERVAL_S` | 86400 | FR-DST-004 |
| `GEOIP_PRIMARY_URL` | `"http://ip-api.com/json/?fields=timezone"` | FR-DST-004, TBD-005 (resolved) |
| `GEOIP_FALLBACK_URL` | `"http://worldtimeapi.org/api/ip"` | FR-DST-004, TBD-005 (resolved) |
| `GEOIP_TIMEOUT_MS` | 5000 | FR-DST-004 |
| `POSIX_TZ_DEFAULT` | `"CET-1CEST,M3.5.0,M10.5.0/3"` | FR-DST-001, TBD-006 (resolved) |

---

## 8. Open Technical Issues

Each item below must be resolved before the indicated module can be implemented. The analysis and options are provided to accelerate the decision; the decision itself and the rationale shall be recorded here when made.

---

### TBD-001 — GNSS 1PPS Input GPIO Pin — ✅ RESOLVED

**Decision:** **GPIO 10** — interrupt-capable, free on LOLIN S3, clear of all other assigned peripherals. Configured as input with no internal pull resistor (`IC-HW-003`). See constants `GNSS_1PPS_GPIO` (§7) and GNSS subsystem design (§5.6).

---

### TBD-002 — GNSS UART Peripheral and Baud Rate — ✅ RESOLVED

**Decision:** **UART1 (`Serial1`), RX GPIO 18, TX GPIO 21, default 9600 baud, 8-N-1.** Baud rate is configurable via NVS key `clock/gnss_baud` (default 9600) and settable through the web GUI GNSS page without a firmware rebuild. See constants `GNSS_UART_RX_GPIO`, `GNSS_UART_TX_GPIO`, `GNSS_DEFAULT_BAUD` (§7) and GNSS subsystem design (§5.6).

---

### TBD-003 — HTTPS Support

**Blocking:** `IC-SW-003` (currently deferred to v1.1). **Not blocking v1.0.**

**Context:** HTTP/1.1 on port 80 is the v1.0 baseline. HTTPS would require TLS termination inside the firmware. The ESP-IDF includes mbedTLS, so no extra library is needed, but the cost is significant:

| Factor | Impact |
|--------|--------|
| Flash footprint | mbedTLS TLS 1.2 stack adds ≈ 150–200 KB to the binary |
| RAM | TLS handshake buffers require ≈ 40–80 KB heap per connection |
| Certificate management | A self-signed certificate causes browser warnings; a CA-signed certificate requires a domain name and renewal process |
| OTA interaction | FOTA already uses signature verification (FR-SEC-001); HTTPS adds transport-layer encryption but does not replace application-layer signing |

**Decision criteria:** Evaluate actual binary size after v1.0 build. If both OTA partitions still have at least 100 KB headroom after adding mbedTLS, HTTPS is feasible. If headroom is insufficient, the partition table can be revised (requires FOTA to deliver the new partition table, which is not supported — DC-005 — so a physical flash is needed).

**Owner:** Firmware developer. **Needed before:** v1.1 milestone.

---

### TBD-004 — FOTA Signing Algorithm: Ed25519 vs RSA-2048

**Blocking:** `http_task` FOTA verification, `FR-SEC-001`, `FR-SEC-005`.

**Context:** mbedTLS is already present in ESP-IDF 5.x. Both algorithms are supported, but with different trade-offs:

| Criterion | Ed25519 | RSA-2048 |
|-----------|---------|----------|
| Signature size | 64 bytes | 256 bytes |
| Verification speed on LX7 @ 240 MHz | < 1 ms | 5–15 ms |
| Key size (public) | 32 bytes | 270 bytes (DER) |
| mbedTLS support in ESP-IDF 5.x | Yes (`mbedtls_eddsa_*`) | Yes (`mbedtls_rsa_*`) |
| Tooling maturity | Excellent (`openssl`, `age`, `signify`) | Excellent |
| SHA-256 digest compatibility (FR-SEC-005) | SHA-512 is standard for Ed25519; SHA-256 requires explicit use of Ed25519ph or pre-hash | Native SHA-256 with PKCS#1 v1.5 or PSS |

**Recommendation:** **Ed25519** — smaller keys, faster verification, smaller signatures stored alongside firmware packages. Use the pre-hash variant (Ed25519ph with SHA-256) to satisfy FR-SEC-005 without SHA-512.

**Action required:** Confirm that the ESP-IDF 5.x mbedTLS configuration used by the `espressif32` PlatformIO platform enables `MBEDTLS_EDDSA_C` by default (it is disabled in some SDK configurations to save flash). If not, enable it via `sdkconfig.defaults`.

**Owner:** Firmware developer. **Needed before:** FR-SEC implementation.

---

### TBD-005 — IP Geolocation Service — ✅ RESOLVED

**Decision:** Primary endpoint **`http://ip-api.com/json/?fields=timezone`** (no API key, 45 req/min free, returns IANA timezone name). Fallback endpoint **`http://worldtimeapi.org/api/ip`** (no key, best-effort). Firmware tries primary with a 5-second timeout; on failure or non-200 response it retries via the fallback. The IANA timezone name is resolved to a POSIX TZ string (see TBD-006) which is written to NVS for offline use. See constants `GEOIP_PRIMARY_URL`, `GEOIP_FALLBACK_URL`, `GEOIP_TIMEOUT_MS` (§7) and DST engine design (§5.7).

---

### TBD-006 — Timezone Rules Library — ✅ RESOLVED

**Decision:** **Custom POSIX TZ string parser** (`src/posix_tz.h`, ≈ 150 lines). The active timezone rule is stored in NVS key `clock/posix_tz` as a standard POSIX TZ string (e.g., `CET-1CEST,M3.5.0,M10.5.0/3`). A compile-time lookup table maps IANA timezone names (received from the geolocation API) to their POSIX TZ equivalents. Because the rule lives in NVS it is updatable via the web GUI or geolocation response without a firmware rebuild. See NVS key `posix_tz` (§6), constant `POSIX_TZ_DEFAULT` (§7), and DST engine design (§5.7).

---

### TBD-007 — HTTP Server Library — ✅ RESOLVED

**Decision:** **`ESPAsyncWebServer` + `AsyncTCP`** (GitHub: me-no-dev). Non-blocking event-driven model keeps the server responsive during OTA flash writes for progress reporting (FR-WEB-044). `serveStatic()` handles SPIFFS asset serving with no boilerplate. Added to `platformio.ini` `lib_deps` (§3.1). See HTTP server design (§5.8).

---

### TBD-008 — DCF77 Time-Code Input GPIO Pin — ✅ RESOLVED

**Decision:** **GPIO 11** — interrupt-capable, free on LOLIN S3, configured as an input with the internal pull-up enabled to suit an open-collector receiver output. Signal polarity is configurable via NVS key `clock/dcf77_invert` (FR-DCF-004). See constant `DCF77_SIGNAL_GPIO` (§7), DCF77 subsystem design (§5.10) and PMC-HTD-001 §3, §7.

---

### TBD-009 — DCF77 Receiver Enable (PON) GPIO Pin — ✅ RESOLVED

**Decision:** **GPIO 12** — push-pull output driving the receiver's power-on/enable line; the module is held off when DCF77 is disabled (FR-DCF-002). See constant `DCF77_PON_GPIO` (§7), DCF77 subsystem design (§5.10) and PMC-HTD-001 §3, §7.

---

### TBD-010 — RTC (DS1307) I2C Pins and 5 V Interface — ✅ RESOLVED

**Decision:** **DS1307Z** at I2C address `0x68`, **SDA = GPIO 8, SCL = GPIO 9**. The DS1307 runs at 5 V; the I2C bus is level-shifted to 3.3 V (bidirectional MOSFET shifter, PMC-HTD-001 §8). A 32.768 kHz crystal and a coin-cell backup on VBAT are required. See constants `RTC_I2C_ADDR`, `I2C_SDA_GPIO`, `I2C_SCL_GPIO` (§7) and RTC design (§5.11). *(A 3.3 V RTC such as the DS3231 would avoid the level shifter and offer better accuracy — noted as an alternative, not selected.)*

---

## 9. Verification and Test Criteria

This section defines the technical test methods and pass/fail criteria that verify the functional requirements of PMC-FRS-001. Each test case (TC-*) maps to the FRS traceability matrix; the FRS states the observable acceptance criterion, this section states the method, setup and measurable pass criterion. Hardware-level checks (electrical, mechanical) are in PMC-HTD-001 §13.

### 9.1 Display Subsystem — TC-DSP-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Inject known local times (00:00, 06:00, 12:00, 18:00, 23:59:59) via the debug time command | Each needle matches the expected position within ±1 scale division (≈ ±1 LSB of the 8-bit duty) | FR-DSP-001..003 |
| 2 | Configure a non-zero UTC offset; read meters at a known UTC instant | Meters show local time, not UTC | FR-DSP-003a |
| 3 | Sweep each channel 0→max in 16 steps; measure V_out at the RC filter node | V_out linear vs. duty (R² ≥ 0.999); ≈0 V at zero, ≈3.0 V at full scale | FR-DSP-004..006 |
| 4 | Observe needles for 10 min against a 1PPS reference | Exactly one step per second; no visible flicker, glitch or overshoot | FR-DSP-007..009 |
| 5 | Set per-meter calibration, power-cycle, re-read; then apply defaults reset | Calibration persists across power cycle; defaults place full scale below the end-stop | FR-DSP-010..014 |

### 9.2 Boot and Time-Source Acquisition — TC-BOOT-001..004

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Power on with no network / GNSS / DCF77 | Display lives at 00:00:00 within ~2 s and advances at 1 Hz | FR-BOOT-001..004 |
| 2 | Provide WiFi + NTP | Clock joins WiFi and corrects to NTP time; failures retried and shown on status | FR-BOOT-005..010 |
| 3 | Provide a GNSS fix | Time switches to GNSS; tick follows the GNSS pulse | FR-BOOT-011..013 |
| 4 | Remove the GNSS fix | Falls back to NTP/internal with no jump > 1 s | FR-BOOT-014..015 |
| 5 | Enable DCF77 with only DCF77 available, then restore NTP/GNSS | DCF77 sets time; NTP/GNSS override cleanly; source changes logged | FR-BOOT-018..021 |
| 6 | Force tick-source changes | Seconds never jump > 1 count; each change logged | FR-BOOT-016..017 |

### 9.3 Timekeeping — TC-TIM-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Set a known UTC and a non-zero offset | Internal time is UTC; local display = UTC + offset/DST | FR-TIM-001..003 |
| 2 | Drive ticks from GNSS pulse, internal, then DCF77 mark | Seconds advance correctly on each; gaps bridged smoothly | FR-TIM-004..007 |
| 3 | Inject a large then a small error at re-sync | Large error stepped; small error corrected without a visible jump | FR-TIM-006 |
| 4 | Present GNSS, NTP, DCF77 together, then remove in priority order | Highest-priority source governs at each stage | FR-TIM-008 |

### 9.4 NTP Synchronisation — TC-NTP-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Configure a custom server; then clear it | Syncs via the configured server, and via the default when unset | FR-NTP-001..002 |
| 2 | Wait past the re-sync interval | Re-syncs automatically on schedule | FR-NTP-003 |
| 3 | Open the status page | NTP server, last sync, quality and next sync shown | FR-NTP-004 |

### 9.5 GNSS — TC-GPS-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Toggle GNSS off/on; reboot | Setting persists; no GNSS activity when off | FR-GPS-001..002 |
| 2 | Acquire a fix | Time, fix status and position obtained; pulse drives the tick | FR-GPS-003..004 |
| 3 | Valid fix alongside NTP; then lose the fix | GNSS governs; lost fix falls back automatically | FR-GPS-005..007 |
| 4 | Power-cycle after a fix | Last position retained as a hint; feeds DST | FR-GPS-008..009 |

### 9.6 Daylight Saving — TC-DST-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Source location from GNSS, then IP geolocation, then DCF77 only (CET) | Timezone/DST resolved from the highest available source | FR-DST-001 |
| 2 | Cross a CET↔CEST change with the network down | Local display shifts one hour at the right moment, smoothly, offline | FR-DST-002..003, FR-DST-006 |
| 3 | Reconnect on the IP-geolocation source | Location refreshed on reconnect and daily | FR-DST-004 |
| 4 | Open the status page | DST state, offset and source shown | FR-DST-005 |

### 9.7 DCF77 — TC-DCF-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Toggle off/on; reboot; test both polarities | Off by default; setting persists; decodes with either polarity | FR-DCF-001..004 |
| 2 | Feed a clean signal, then a corrupt one | Date/time + CET/CEST recovered; bad frames rejected; time used only after confirmation | FR-DCF-005..007 |
| 3 | DCF77 only, then add NTP/GNSS | DCF77 sets UTC (correct CET/CEST offset); NTP/GNSS override it | FR-DCF-008..009 |
| 4 | Interrupt reception; cross a CET↔CEST change | No display disturbance; UTC continuous across the change | FR-DCF-010..011 |
| 5 | Remove the GNSS pulse with DCF77 present | Seconds tick follows the DCF77 mark; gaps fall back smoothly | FR-DCF-012 |

### 9.8 Network and Discovery — TC-NW-001..002

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Provide valid credentials | Connects and gets an address automatically | FR-NW-001..002 |
| 2 | Drop and restore WiFi | Reconnects on its own | FR-NW-003 |
| 3 | Start with no joinable network | Open fallback AP appears after the timeout; full GUI works on it; closes on joining | FR-NW-005..009 |
| 4 | From a LAN client | Resolves as `panelclock.local`; web service discoverable; not advertised in AP-only mode | FR-NW-010..013 |

### 9.9 Web GUI — TC-WEB-001..006

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Load the GUI offline in a stock browser | All pages load from the device; nav works; no internet needed | FR-WEB-001..005 |
| 2 | Use each config page (calibrate, WiFi, GNSS, DCF77) | Settings apply and persist; calibration live-preview works | FR-WEB-010..027, 050..053 |
| 3 | Open the status page | All status fields present and auto-refresh | FR-WEB-030..035 |
| 4 | Upload good and tampered firmware | Good installs and reboots; tampered rejected with a clear error, clock unchanged | FR-WEB-040..046 |

### 9.10 Security / FOTA — TC-SEC-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Inspect device key handling | Only the public verification key is present; it survives updates | FR-SEC-001..003 |
| 2 | Upload validly signed, unsigned, and altered packages | Only the valid one is accepted; others rejected before any write, with a logged reason | FR-SEC-004..006 |
| 3 | Deliver a signed update carrying a new key | New key installs; later updates verified against it | FR-SEC-007 |

### 9.11 Non-Functional — TC-NFR-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Measure tick-to-meter latency and page load | Meters update within ~10 ms; pages load in a few seconds; display unaffected by sync | NFR-PERF-001..003 |
| 2 | Hang the firmware; corrupt stored settings | Auto-resets within ~30 s and reboots; corrupt settings reset to defaults and logged | NFR-REL-001..003 |
| 3 | Power off / reset | Needles fall to zero promptly | NFR-PWR-001 |
| 4 | Review configurability, logging and structure | Settings change at runtime without reflash; key events logged; drive layer isolated | NFR-MNT-001..002, NFR-PORT-001 |

### 9.12 Real-Time Clock — TC-RTC-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Set a time (sync or encoder), remove main power briefly, re-boot | Clock resumes the correct time of day from the RTC | FR-RTC-001..002, FR-RTC-004 |
| 2 | Sync via GNSS/NTP/DCF77, then cold-boot offline | Boot time matches the previously-synced time (RTC written back) | FR-RTC-003 |
| 3 | Remove the backup battery, then boot | Time invalid → starts at 00:00:00; RTC indicator shows invalid | FR-RTC-005 |
| 4 | Provide a higher source while on RTC time | Higher source overrides the RTC | FR-RTC-006 |

### 9.13 Panel Indicators and Control — TC-UI-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Drive each source to off / acquiring / valid | Each LED shows off / ≈1 Hz blink / steady accordingly; active source readable by priority | UI-IND-001..009 |
| 2 | Remove the RTC backup battery and boot | RTC LED blinks (invalid) and the clock starts at 00:00:00 | UI-IND-006, FR-RTC-005 |
| 3 | Long-press, rotate each field, short-press through H/M/S, confirm | Set mode entered; meters track edits; time written to the RTC and retained | UI-CTL-002..006, FR-RTC-004 |
| 4 | Enter set mode and wait out the timeout | Returns to normal display with the time unchanged | UI-CTL-007 |

---

## 10. Design Constraints

These constraints (moved from PMC-FRS-001) bound the implementation. Hardware specifics are detailed in PMC-HTD-001.

| ID | Constraint |
|----|------------|
| DC-001 | Target is the WEMOS LOLIN S3 (ESP32-S3-WROOM-1-N16R8: 16 MB quad flash, 8 MB octal PSRAM); build with PlatformIO + Arduino on ESP-IDF 5.x; firmware runs on FreeRTOS (no bare-metal or alternative RTOS). |
| DC-002 | The ESP32-S3 has no analogue DAC; all meter drive is produced by PWM + RC filtering via the LEDC peripheral. |
| DC-003 | Each meter is modified so 3 V gives full-scale deflection; design point 0–3 V (duty 0–232); the 3.3 V GPIO maximum is not used as full scale (see PMC-HTD-001 §4.3). |
| DC-004 | PWM is 8-bit (0–255) at 80 kHz from the 80 MHz LEDC clock — 256 discrete positions, no fractional duty. |
| DC-005 | The flash partition table must include two OTA application partitions; it is a build-time configuration and cannot be changed by FOTA. |
| DC-006 | The FOTA public key is embedded in the firmware binary at build time, excluded from OTA writes, and not modifiable at runtime. |
| DC-007 | DCF77 reception is limited to Central Europe (≈ 2 000 km from Mainflingen) and depends on local signal strength; it is an optional, region-specific source the design shall not rely on. |
| DC-008 | The DS1307Z RTC operates at 5 V; its I2C bus is level-shifted to the 3.3 V logic of the ESP32-S3, and it requires a 32.768 kHz crystal and a coin-cell backup (PMC-HTD-001 §8). |

---

## 11. External Interfaces

Moved from PMC-FRS-001. Pin-level detail is in PMC-HTD-001 §3; protocol and peripheral detail is in the module designs (§5).

### 11.1 Hardware Interfaces

| ID | Interface |
|----|-----------|
| IC-HW-001 | Meter PWM outputs use the GPIO / LEDC channels and timers assigned in PMC-HTD-001 §3 and §5. |
| IC-HW-002 | Each PWM channel has a dedicated RC low-pass filter (cutoff ≈ 16 Hz; PMC-HTD-001 §4.4). |
| IC-HW-003 | GNSS 1PPS on GPIO 10 — input, no internal pull, rising-edge interrupt (PMC-HTD-001 §3). |
| IC-HW-004 | GNSS UART on UART1 (RX GPIO 18, TX GPIO 21), default 9 600 baud, configurable (PMC-HTD-001 §6). |
| IC-HW-005 | DCF77 time-code on GPIO 11 — interrupt-capable input with internal pull-up, polarity configurable (PMC-HTD-001 §7). |
| IC-HW-006 | DCF77 enable (PON) on GPIO 12 — push-pull output; receiver held off when DCF77 disabled (PMC-HTD-001 §7). |
| IC-HW-007 | DS1307Z RTC on I2C — SDA GPIO 8, SCL GPIO 9, address 0x68; 5 V part with the bus level-shifted to 3.3 V (PMC-HTD-001 §3, §8). |
| IC-HW-008 | Four front-panel status LEDs (RTC/DCF/NTP/GNSS) on GPIO 4–7 — push-pull outputs with series resistors (PMC-HTD-001 §3). |
| IC-HW-009 | Rotary encoder on GPIO 13 (A) / 14 (B) and push button on GPIO 47 — inputs with internal pull-ups, firmware debounced (PMC-HTD-001 §3). |

### 11.2 Software and Protocol Interfaces

| ID | Interface |
|----|-----------|
| IC-SW-001 | NTP per NTPv4 (RFC 5905), UDP port 123. |
| IC-SW-002 | GNSS via NMEA 0183; at minimum `$GPRMC` / `$GNRMC` are parsed. |
| IC-SW-003 | Web GUI over HTTP/1.1, TCP port 80 (HTTPS deferred — §8 TBD-003). |
| IC-SW-004 | FOTA uses the ESP-IDF dual-slot OTA partition layout. |
| IC-SW-005 | DCF77 decoded from the one-minute amplitude-modulated time-code frame (PTB DCF77 standard). |

### 11.3 Human Interfaces

| ID | Interface |
|----|-----------|
| IC-HMI-001 | Primary UI is the embedded web GUI (FR-WEB-*). |
| IC-HMI-002 | Secondary diagnostic interface: serial debug stream at 115 200 baud, 8-N-1, on UART0 / USB-CDC. |
