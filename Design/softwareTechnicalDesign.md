# Software Technical Design

## PanelMeterClock Firmware

---

## 1. Document Control

### 1.1 Identification

| Field | Value |
|-------|-------|
| Document title | Software Technical Design — PanelMeterClock Firmware |
| Document ID | PMC-STD-001 |
| Version | 0.3 (draft) |
| Date | 2026-06-15 |
| Author | Remko Welling |
| Status | Draft — under review |

### 1.2 Revision History

| Version | Date | Author | Change Summary |
|---------|------|--------|----------------|
| 0.1 | 2026-04-29 | Remko Welling | Initial draft; content migrated from PMC-FRS-001 appendices and supplemented with architecture design |
| 0.2 | 2026-06-09 | Remko Welling | Adopted the Greenhouse-Controller OTA design (new §5.13): dual app banks + paired dual-LittleFS assets, paired atomic commit (FW_DONE + 120 s fallback), 3-fail rollback with healthy-mark, `/api/ota` state machine, OTA-in-progress flag, reboot worker, `coredump` partition. Repartitioned §3.3; added `ota_task` (§4), NVS `system/*` keys (§6), OTA constants (§7), TC-OTA-001 (§8.14), DC-009/DC-010 (§9), IC-SW-004 (§10). Ed25519 firmware signing retained. |
| 0.3 | 2026-06-15 | Remko Welling | Raised meter PWM from 80 kHz to 156.25 kHz to clear the 77.5 kHz DCF77 band; reduced the RC filter capacitor 10 µF → 4.7 µF to match (PMC-HTD-001 §4.4, §5). Updated `PWM_FREQ_HZ` (§7), DC-004 (§9), §5.2; added a DCF77/PWM coexistence step to TC-DCF-001 (§8.7). |

### 1.3 Relationship to the FRS

This document is the companion technical design to **PMC-FRS-001 (Functional Requirements Specification)**. The FRS defines *what* the firmware shall do; this document defines *how* it is structured and implemented. Requirements are identified by their FRS identifiers (e.g., FR-DSP-006).

---

## 2. Target Platform

| Attribute | Value |
|-----------|-------|
| Module | Espressif ESP32-S3-WROOM-1-N16R8 |
| CPU | Xtensa LX7 dual-core, 240 MHz |
| Flash | 16 MB quad SPI (N16) |
| RAM | 512 KB SRAM + 8 MB octal PSRAM (R8) |
| Framework | Espressif ESP-IDF 5.x (mandatory) |
| Build system | PlatformIO |
| RTOS | FreeRTOS (provided by ESP-IDF) |

---

## 3. Build System

### 3.1 `platformio.ini`

```ini
[env:esp32s3]
platform                = espressif32
board                   = esp32-s3-devkitc-1   ; ESP32-S3-WROOM-1 reference; adjust for the project PCB
framework               = espidf
monitor_speed           = 115200
board_build.flash_size  = 16MB
board_build.partitions  = partitions_ota.csv
; N16R8 octal PSRAM is enabled in sdkconfig (CONFIG_SPIRAM, CONFIG_SPIRAM_MODE_OCT).
; No external lib_deps: HTTP server (esp_http_server), WiFi (esp_wifi/esp_netif),
; mDNS (mdns), NVS (nvs_flash) and the UART/GPIO/I2C/LEDC drivers are ESP-IDF components.
```

### 3.2 Library Dependencies

| Library | Source | Purpose | Rationale |
|---------|--------|---------|-----------|
| `esp_wifi` + `esp_netif` + `esp_event` | Built-in (ESP-IDF) | STA and AP mode (FR-NW-001..009) | Core ESP-IDF networking stack |
| `mdns` | Built-in (ESP-IDF component) | mDNS / DNS-SD registration (FR-NW-010..013) | No external dependency |
| `esp_http_server` | Built-in (ESP-IDF) | Embedded HTTP server and OTA upload (FR-WEB-001..053) | Streams the OTA body straight to `esp_ota_write()` |
| `nvs_flash` | Built-in (ESP-IDF) | NVS key-value storage (FR-DSP-010..012, FR-NW-001) | Native ESP-IDF NVS |
| Custom POSIX TZ parser | In-tree (`src/posix_tz.h`) | DST rule engine (FR-DST-001..006) | POSIX TZ strings are compact, updatable via NVS without firmware rebuild |
| GNSS NMEA parser | In-tree (`src/nmea.h`) | NMEA sentence parsing (FR-GPS-003) | Lightweight; parses $GPRMC/$GNRMC only |
| DCF77 time-code decoder | In-tree (`src/dcf77.h`) | DCF77 frame decode, validation and DST bits (FR-DCF-003..011) | Simple edge-timing decoder; no external library |
| RTC driver (DS1307) | In-tree (`src/ds1307.h`) | Battery-backed RTC over I2C (FR-RTC-001..006) | BCD register read/write at I2C 0x68 via the ESP-IDF I2C driver |
| ESP-IDF peripheral drivers (`driver/`) | Built-in (ESP-IDF) | UART (GNSS), GPIO (1PPS / DCF77 / encoder / LEDs), I2C (RTC), LEDC (PWM) | Native peripheral drivers |
| mbedTLS | Built-in (ESP-IDF) | Ed25519 (Ed25519ph + SHA-256) for FOTA signing (FR-SEC-001..005) | Enable `MBEDTLS_EDDSA_C` in `sdkconfig.defaults`; no extra flash cost |

### 3.3 Partition Table (`partitions_ota.csv`)

Dual app banks paired 1:1 with dual LittleFS asset partitions, plus `otadata` and a `coredump` partition (DC-005, §5.13):

```
# Name,   Type, SubType,  Offset,    Size,     Flags
nvs,      data, nvs,      0x9000,    0x5000,
otadata,  data, ota,      0xe000,    0x2000,
app0,     app,  ota_0,    0x10000,   0x200000,
app1,     app,  ota_1,    0x210000,  0x200000,
lfs0,     data, spiffs,   0x410000,  0x180000,
lfs1,     data, spiffs,   0x590000,  0x180000,
coredump, data, coredump, 0x710000,  0x10000,
```

- **`app0`/`app1`** — the two firmware banks (2 MB each); one active, one the OTA target.
- **`lfs0`/`lfs1`** — LittleFS web-asset partitions paired to the app banks (`app0→lfs0`, `app1→lfs1`); the running image mounts its own, the OTA writer writes the other (§5.13).
- **`coredump`** — crash-dump capture; must be erased on a new unit's first (greenfield) flash, not by OTA (DC-010).

The FOTA public key (FR-SEC-001) is embedded in the firmware binary at build time, not a partition.

---

## 4. FreeRTOS Task Architecture

All application work runs in FreeRTOS tasks created from `app_main()` (the ESP-IDF entry point).

| Task name | Core | Priority | Stack (bytes) | Responsibility |
|-----------|------|----------|---------------|----------------|
| `display_task` | APP (1) | 10 | 4096 | Drives the three PWM meters; executes once per 1PPS tick event (FR-DSP-007) |
| `tick_task` | APP (1) | 9 | 2048 | Maintains UTC epoch; synthesises software 1PPS via FreeRTOS timer; arbitrates tick and epoch source; seeds from and writes back the RTC (FR-TIM-001..008, FR-RTC-001..006, FR-BOOT-016) |
| `ntp_task` | PRO (0) | 5 | 4096 | Issues NTP queries; disciplines the epoch (FR-NTP-001..004, FR-BOOT-006) |
| `wifi_task` | PRO (0) | 6 | 4096 | STA connect/reconnect with exponential back-off; AP fallback; mDNS start/stop (FR-NW-001..013) |
| `gnss_task` | PRO (0) | 7 | 4096 | UART read; NMEA parsing; 1PPS interrupt latch (FR-GPS-001..009); only created when GNSS is enabled |
| `dcf77_task` | PRO (0) | 7 | 3072 | DCF77 time-code edge capture, frame decode and validation, per-second tick (FR-DCF-001..012); only created when DCF77 is enabled |
| `http_task` | PRO (0) | 4 | 8192 | Embedded web server; serves the GUI pages and the `/api/ota/*` endpoints; owns the OTA state machine (FR-WEB-001..053, §5.13) |
| `ota_task` | PRO (0) | 5 | 4096 | Background OTA — extracts the buffered asset ZIP to the inactive LittleFS, performs the paired boot-slot commit, schedules the reboot worker (§5.13); only created during an update |
| `ui_task` | APP (1) | 8 | 2048 | Drives the four status LEDs and services the rotary encoder / time-set (PMC-GUI-001) |

**Inter-task communication:**

| Signal | Mechanism | Produced by | Consumed by |
|--------|-----------|-------------|-------------|
| 1PPS tick event | `xTaskNotifyGive` | `tick_task` (software) or GNSS ISR (hardware) | `display_task` |
| UTC epoch update | Shared `volatile uint64_t` + `portENTER_CRITICAL` | `tick_task`, `ntp_task`, `gnss_task`, `dcf77_task` | All |
| Tick source selection | Atomic flag (`volatile uint8_t`) | `wifi_task`, `gnss_task`, `ntp_task`, `dcf77_task` | `tick_task` |
| Epoch source selection | Atomic flag (`volatile uint8_t`) | `gnss_task`, `ntp_task`, `dcf77_task` | `tick_task` |
| Config change | FreeRTOS event group | `http_task` | `wifi_task`, `ntp_task`, `gnss_task`, `dcf77_task` |
| OTA in progress | FreeRTOS event-group bit | `http_task`, `ota_task` | `wifi_task`, `tick_task`, status pushes (defer non-essential work, §5.13) |
| OTA state / progress | Mutex-guarded struct | `http_task`, `ota_task` | `http_task` (`GET /api/ota/status`) |

---

## 5. Module Design

### 5.1 Boot State Machine

The firmware progresses through three phases (FR-BOOT-001..017). The table below is the authoritative state-transition design.

| Current state | Event / guard | Next state | Actions on transition |
|--------------|---------------|------------|----------------------|
| *(start)* | Power-on | `PHASE_1` | Read the RTC and initialise the epoch from it (00:00:00 if the RTC time is invalid); start the 1 Hz software tick in `tick_task` (FR-RTC-002, FR-BOOT-002) |
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

On every transition that sets the epoch from an accurate source — NTP, GNSS or DCF77, including the `STEADY` re-sync transitions above — `tick_task` also writes the corrected UTC back to the DS1307 RTC so the battery-backed time stays accurate (FR-RTC-003, §5.11).

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

PWM frequency: 156 250 Hz (80 MHz ÷ 512, exact); resolution: 8-bit; LEDC clock: 80 MHz (DC-004). The frequency clears the 77.5 kHz DCF77 band — see PMC-HTD-001 §5 and §7.4.  
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

`wifi_task` owns the WiFi state machine using `esp_wifi` / `esp_netif` / `esp_event`. At startup it reads `wifi_ssid` and `wifi_pass` from NVS and brings up STA:

```c
esp_netif_set_hostname(sta_netif, mdns_hostname);   // DHCP Option 12 (FR-NW-013)
esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);          // SSID / password
esp_wifi_start();
esp_wifi_connect();
```

**Reconnect back-off** (FR-NW-003): delay starts at 5 s, doubles on each failure, caps at 300 s.

**AP fallback** (FR-NW-005..009): if STA is not associated within `ap_timeout_s` seconds, activate the AP:

```c
// AP SSID = "PanelClock-AABB" (AABB = last 2 MAC bytes, uppercase hex)
esp_wifi_set_mode(WIFI_MODE_APSTA);
esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
```

**mDNS** (FR-NW-010..013): the `mdns` component is started on `IP_EVENT_STA_GOT_IP` and stopped on `WIFI_EVENT_STA_DISCONNECTED` (handled via `esp_event`). Not active during AP-only mode.

```c
// On IP_EVENT_STA_GOT_IP:
mdns_init();
mdns_hostname_set(mdns_hostname);                       // registers panelclock.local
mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);  // DNS-SD advertisement

// On WIFI_EVENT_STA_DISCONNECTED:
mdns_free();
```

`mdns_hostname` defaults to `"panelclock"` (NVS key `clock/mdns_hostname`). It is the same value passed to `esp_netif_set_hostname()`.

### 5.6 GNSS Subsystem

**Module:** Quectel L76-M33 (GPS/GLONASS/Galileo/BeiDou, 3.3 V, NMEA 0183, 1PPS — see PMC-HTD-001 §6).

`gnss_task` is created only when `gnss_enabled` NVS flag is set (FR-GPS-001..002). It reads NMEA sentences from the GNSS UART, decodes `$GPRMC`/`$GNRMC`, and tracks fix validity. When a valid fix is obtained (FR-GPS-005), it writes the GNSS UTC time to `utc_epoch_s` and signals the tick source arbitrator to switch to hardware 1PPS.

**Hardware assignments (resolved):**

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| 1PPS input | **GPIO 10** | Input, no pull | Rising-edge interrupt; 3.3 V-tolerant |
| UART RX | **GPIO 18** | Input | UART1, ESP-IDF UART driver |
| UART TX | **GPIO 21** | Output | UART1; leave unconnected if no module config needed |

**UART initialisation:**

```c
uart_config_t cfg = { .baud_rate = gnss_baud, .data_bits = UART_DATA_8_BITS,
                      .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1 };
uart_param_config(UART_NUM_1, &cfg);
uart_set_pin(UART_NUM_1, GNSS_UART_TX_GPIO, GNSS_UART_RX_GPIO, -1, -1);
uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0);
```

`gnss_baud` is read from NVS key `clock/gnss_baud` (default 9600). Changing it via the web GUI GNSS page takes effect after `gnss_task` is restarted (no reboot required).

**1PPS interrupt:**

```c
gpio_config_t io = { .pin_bit_mask = 1ULL << GNSS_1PPS_GPIO,
                     .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE };
gpio_config(&io);
gpio_install_isr_service(0);
gpio_isr_handler_add(GNSS_1PPS_GPIO, gnss_pps_isr, NULL);
```

The ISR calls `vTaskNotifyGiveFromISR` to unblock `display_task` and increments `utc_epoch_s` inside a critical section. The interrupt is detached when `gnss_task` is stopped (GNSS disabled).

### 5.7 DST Engine

The DST engine is called once per network reconnection and every 24 hours (FR-DST-004). It selects a location source per the priority hierarchy (FRS Section 5.5) and resolves the applicable UTC offset using the **custom POSIX TZ string parser**.

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

`http_task` runs the ESP-IDF **`esp_http_server`**. Static assets are served from the **active** LittleFS partition (§3.3, mounted through the VFS) by a wildcard `GET` URI handler; dynamic data (status page fields) is served via a lightweight JSON API polled by JavaScript (FR-WEB-034). Firmware and asset uploads are `POST` handlers under `/api/ota/*` that stream into the OTA subsystem (§5.13).

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

**Firmware + asset update** is handled by the OTA subsystem over `POST /api/ota/firmware`, `POST /api/ota/assets` and `GET /api/ota/status` (the `/update` page is the browser front-end). Firmware and web assets are written to the inactive bank / inactive LittleFS and committed together; the full protocol, state machine, paired commit and rollback are specified in **§5.13**. The two-file detached **Ed25519** signature packaging (FR-SEC-001..006) is retained — rationale and alternatives in `Design/signatureConciderations.md`.

### 5.9 NVS Storage

All persistent configuration uses the ESP-IDF **NVS** library (`nvs_flash` / `nvs_open`), namespaces `"clock"` and `"meter"`. Keys, types, and defaults are listed in Section 6 below. NVS corruption is detected at startup from the `nvs_flash_init()` return code (`ESP_ERR_NVS_NO_FREE_PAGES` / `ESP_ERR_NVS_NEW_VERSION_FOUND`); on corruption the partition is erased and re-initialised to factory defaults (NFR-REL-003).

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

### 5.13 OTA Update Subsystem

A network-only update path that flashes a new **firmware image and its matching web-asset bundle as one atomic unit**, survives power loss at any byte, and auto-rolls-back firmware that fails to boot. Adapted from the field-tested Greenhouse-Controller OTA design, with PMC's Ed25519 firmware signing retained. Cloud-pull, delta, resumable and concurrent updates are out of scope.

**Pillars**

- **Dual-bank firmware** (`app0`/`app1`, §3.3). Exactly one bank is active; the OTA writer targets the other. `otadata` records the boot bank and the bootloader swaps it atomically.
- **Coupled asset partitions** (`lfs0`/`lfs1`, §3.3), paired 1:1 with the app banks (`app0→lfs0`, `app1→lfs1`). The running image mounts its own LittleFS; the OTA writer writes the *other*. This prevents a refreshed browser landing on a UI built for a different firmware/API version.
- **Paired atomic commit.** The boot-slot swap happens only **after** the firmware is verified *and* the assets are extracted — there is never a state where new firmware runs against old assets.
- **Streaming firmware, buffered assets.** Firmware bytes stream to the inactive bank via `esp_ota_write()` (flat RAM cost); the asset ZIP is buffered whole in PSRAM (available on the N16R8), then extracted by `ota_task`.
- **Three-fail rollback** with a healthy-marking signal.

**HTTP protocol** (admin auth; `Content-Length` required on POSTs, else `400`):

| Method · Path | Body | Response |
|---|---|---|
| `POST /api/ota/firmware` | firmware `.bin` + detached Ed25519 signature | `200 {ok, awaiting_assets:true}` once written **and signature-verified** to the inactive bank (boot slot **not** yet swapped) |
| `POST /api/ota/assets` | STORE-only `.zip` (`application/zip`) | `202` — body buffered to PSRAM; extraction runs in `ota_task`; client polls status |
| `GET /api/ota/status` | — | `200 {ok, state, progress, error, bank, accepted}` |

`state ∈ { idle, fw_writing, fw_verifying, assets_buffering, assets_writing, fw_done, rebooting, error }`; `accepted` reflects the healthy/rollback flag.

**State machine** (mutex-guarded; the mutex is **not** held during flash erase/program, which can take seconds):

```
IDLE ──POST firmware──► FW_WRITING ──► FW_VERIFYING ──► FW_DONE ──120 s timeout──┐
  │                                                       │                      │
  │                                                  POST assets                 ▼
  └──POST assets──► ASSETS_BUFFERING ──► ASSETS_WRITING ──┴──────────────────► REBOOTING
   any state on error → ERROR (error string set);   ERROR / FW_DONE accept a fresh POST
```

- **Firmware path.** Stream to the inactive bank; `esp_ota_end()` validates the IDF SHA-256, then the **Ed25519 signature** is verified against the embedded public key (FR-SEC-001..006). The boot partition is **not** set here — the subsystem enters `FW_DONE` and waits up to `OTA_ASSET_FALLBACK_MS` (120 s) for an asset bundle, so a firmware-only push still commits.
- **Asset path.** `ota_task` parses the buffered ZIP — **STORE / method-0 only** (DC-009); a DEFLATE entry is rejected with a diagnostic — and extracts entries to the inactive LittleFS. If that partition fails to mount (fresh unit) it is **formatted first** (format-on-mount-failure). An **asset-only** session (no firmware uploaded this cycle) writes to the **active** LittleFS and does not swap the boot slot.
- **Paired commit.** Once the inactive LittleFS contents are durable, `ota_task` calls `esp_ota_set_boot_partition()` and schedules the reboot. A firmware rollback automatically points the image back at its own paired LittleFS, which already matches.
- **Three-fail rollback.** Every boot increments NVS `system/ota_fail_cnt`; after `OTA_HEALTHY_MS` (30 s) of healthy uptime `ota_mark_healthy()` resets it to 0. Reaching `OTA_FAIL_THRESHOLD` (3) before a healthy mark calls `esp_ota_mark_app_invalid_rollback_and_reboot()`, reverting to the previous bank. Deliberate restarts set NVS `system/ota_intentional` (paired with a "software reset" reason) so planned reboots are not counted.
- **Reboot scheduling.** The reset is performed by a small dedicated worker task (≈ 4 KB stack) spawned from the timer callback — never directly from the FreeRTOS timer-service task, whose stack the `esp_wifi_stop()` teardown would overflow.
- **OTA-in-progress flag.** An event-group bit (set on entering any active state, cleared on every exit including error) lets other tasks defer non-essential work (status pushes, reconnect timers) so the flash write is not slowed by bus contention.

Build-side artifacts — STORE-only ZIP construction, per-release archiving, asset-version stamping, and the operator push client — are tooling outside this document.

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
| `clock` | `gnss_baud` | uint32 | `9600` | IC-HW-004 |
| `clock` | `gnss_lat_cache` | float | `0.0` | FR-GPS-009 |
| `clock` | `gnss_lon_cache` | float | `0.0` | FR-GPS-009 |
| `clock` | `dcf77_enabled` | uint8 | `0` (disabled) | FR-DCF-001 |
| `clock` | `dcf77_invert` | uint8 | `0` (non-inverted) | FR-DCF-004 |
| `clock` | `posix_tz` | string | `"CET-1CEST,M3.5.0,M10.5.0/3"` | FR-DST-001 |
| `meter` | `h_zero` | uint8 | `0` | FR-DSP-010 |
| `meter` | `h_full` | uint8 | `232` | FR-DSP-011, FR-DSP-014 |
| `meter` | `m_zero` | uint8 | `0` | FR-DSP-010 |
| `meter` | `m_full` | uint8 | `232` | FR-DSP-011, FR-DSP-014 |
| `meter` | `s_zero` | uint8 | `0` | FR-DSP-010 |
| `meter` | `s_full` | uint8 | `232` | FR-DSP-011, FR-DSP-014 |
| `system` | `ota_fail_cnt` | uint8 | `0` | §5.13 (3-fail rollback) |
| `system` | `ota_intentional` | uint8 | `0` | §5.13 (planned-reboot exempt) |
| `system` | `asset_version` | string | `""` | §5.13 (status; from asset manifest) |

All key names shall be defined as `constexpr char[]` constants in a single header (`nvs_keys.h`); no literal strings shall appear in application code (NFR-MNT-001).

---

## 7. Named Constants

All magic numbers shall be defined as named constants. The following table lists the most critical ones; the complete set lives in `config.h`.

| Constant | Value | Requirement |
|----------|-------|-------------|
| `PWM_FREQ_HZ` | 156250 | FR-DSP-008, DC-004 |
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
| `GNSS_1PPS_GPIO` | 10 | IC-HW-003 |
| `GNSS_UART_RX_GPIO` | 18 | IC-HW-004 |
| `GNSS_UART_TX_GPIO` | 21 | IC-HW-004 |
| `GNSS_DEFAULT_BAUD` | 9600 | IC-HW-004 |
| `DCF77_SIGNAL_GPIO` | 11 | IC-HW-005 |
| `DCF77_PON_GPIO` | 12 | IC-HW-006 |
| `DCF77_ENABLED_DEFAULT` | 0 | FR-DCF-002 |
| `DCF77_SIGNAL_LOST_TIMEOUT_S` | 300 | FR-DCF-010 |
| `RTC_I2C_ADDR` | 0x68 | IC-HW-007, FR-RTC-001 |
| `I2C_SDA_GPIO` | 8 | IC-HW-007 |
| `I2C_SCL_GPIO` | 9 | IC-HW-007 |
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
| `GEOIP_PRIMARY_URL` | `"http://ip-api.com/json/?fields=timezone"` | FR-DST-004 |
| `GEOIP_FALLBACK_URL` | `"http://worldtimeapi.org/api/ip"` | FR-DST-004 |
| `GEOIP_TIMEOUT_MS` | 5000 | FR-DST-004 |
| `POSIX_TZ_DEFAULT` | `"CET-1CEST,M3.5.0,M10.5.0/3"` | FR-DST-001 |
| `OTA_HEALTHY_MS` | 30000 | §5.13 (healthy-mark delay) |
| `OTA_FAIL_THRESHOLD` | 3 | §5.13 (rollback trigger) |
| `OTA_ASSET_FALLBACK_MS` | 120000 | §5.13 (firmware-only commit) |
| `OTA_REBOOT_TASK_STACK` | 4096 | §5.13 (reboot worker) |

---

## 8. Verification and Test Criteria

This section defines the technical test methods and pass/fail criteria that verify the functional requirements of PMC-FRS-001. Each test case (TC-*) maps to the FRS traceability matrix; the FRS states the observable acceptance criterion, this section states the method, setup and measurable pass criterion. Hardware-level checks (electrical, mechanical) are in PMC-HTD-001 §13.

### 8.1 Display Subsystem — TC-DSP-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Inject known local times (00:00, 06:00, 12:00, 18:00, 23:59:59) via the debug time command | Each needle matches the expected position within ±1 scale division (≈ ±1 LSB of the 8-bit duty) | FR-DSP-001..003 |
| 2 | Configure a non-zero UTC offset; read meters at a known UTC instant | Meters show local time, not UTC | FR-DSP-003a |
| 3 | Sweep each channel 0→max in 16 steps; measure V_out at the RC filter node | V_out linear vs. duty (R² ≥ 0.999); ≈0 V at zero, ≈3.0 V at full scale | FR-DSP-004..006 |
| 4 | Observe needles for 10 min against a 1PPS reference | Exactly one step per second; no visible flicker, glitch or overshoot | FR-DSP-007..009 |
| 5 | Set per-meter calibration, power-cycle, re-read; then apply defaults reset | Calibration persists across power cycle; defaults place full scale below the end-stop | FR-DSP-010..014 |

### 8.2 Boot and Time-Source Acquisition — TC-BOOT-001..004

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Power on with no network / GNSS / DCF77 | Display lives at 00:00:00 within ~2 s and advances at 1 Hz | FR-BOOT-001..004 |
| 2 | Provide WiFi + NTP | Clock joins WiFi and corrects to NTP time; failures retried and shown on status | FR-BOOT-005..010 |
| 3 | Provide a GNSS fix | Time switches to GNSS; tick follows the GNSS pulse | FR-BOOT-011..013 |
| 4 | Remove the GNSS fix | Falls back to NTP/internal with no jump > 1 s | FR-BOOT-014..015 |
| 5 | Enable DCF77 with only DCF77 available, then restore NTP/GNSS | DCF77 sets time; NTP/GNSS override cleanly; source changes logged | FR-BOOT-018..021 |
| 6 | Force tick-source changes | Seconds never jump > 1 count; each change logged | FR-BOOT-016..017 |

### 8.3 Timekeeping — TC-TIM-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Set a known UTC and a non-zero offset | Internal time is UTC; local display = UTC + offset/DST | FR-TIM-001..003 |
| 2 | Drive ticks from GNSS pulse, internal, then DCF77 mark | Seconds advance correctly on each; gaps bridged smoothly | FR-TIM-004..007 |
| 3 | Inject a large then a small error at re-sync | Large error stepped; small error corrected without a visible jump | FR-TIM-006 |
| 4 | Present GNSS, NTP, DCF77 together, then remove in priority order | Highest-priority source governs at each stage | FR-TIM-008 |

### 8.4 NTP Synchronisation — TC-NTP-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Configure a custom server; then clear it | Syncs via the configured server, and via the default when unset | FR-NTP-001..002 |
| 2 | Wait past the re-sync interval | Re-syncs automatically on schedule | FR-NTP-003 |
| 3 | Open the status page | NTP server, last sync, quality and next sync shown | FR-NTP-004 |

### 8.5 GNSS — TC-GPS-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Toggle GNSS off/on; reboot | Setting persists; no GNSS activity when off | FR-GPS-001..002 |
| 2 | Acquire a fix | Time, fix status and position obtained; pulse drives the tick | FR-GPS-003..004 |
| 3 | Valid fix alongside NTP; then lose the fix | GNSS governs; lost fix falls back automatically | FR-GPS-005..007 |
| 4 | Power-cycle after a fix | Last position retained as a hint; feeds DST | FR-GPS-008..009 |

### 8.6 Daylight Saving — TC-DST-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Source location from GNSS, then IP geolocation, then DCF77 only (CET) | Timezone/DST resolved from the highest available source | FR-DST-001 |
| 2 | Cross a CET↔CEST change with the network down | Local display shifts one hour at the right moment, smoothly, offline | FR-DST-002..003, FR-DST-006 |
| 3 | Reconnect on the IP-geolocation source | Location refreshed on reconnect and daily | FR-DST-004 |
| 4 | Open the status page | DST state, offset and source shown | FR-DST-005 |

### 8.7 DCF77 — TC-DCF-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Toggle off/on; reboot; test both polarities | Off by default; setting persists; decodes with either polarity | FR-DCF-001..004 |
| 2 | Feed a clean signal, then a corrupt one | Date/time + CET/CEST recovered; bad frames rejected; time used only after confirmation | FR-DCF-005..007 |
| 3 | DCF77 only, then add NTP/GNSS | DCF77 sets UTC (correct CET/CEST offset); NTP/GNSS override it | FR-DCF-008..009 |
| 4 | Interrupt reception; cross a CET↔CEST change | No display disturbance; UTC continuous across the change | FR-DCF-010..011 |
| 5 | Remove the GNSS pulse with DCF77 present | Seconds tick follows the DCF77 mark; gaps fall back smoothly | FR-DCF-012 |
| 6 | Decode DCF77 with all three meters sweeping full scale (worst-case PWM activity) | Frames still decode with no significant rise in parity errors vs. meters static — confirms the 156.25 kHz PWM does not jam 77.5 kHz reception (cross-ref HV-11) | FR-DCF-005..007, DC-004, HW-011 |

### 8.8 Network and Discovery — TC-NW-001..002

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Provide valid credentials | Connects and gets an address automatically | FR-NW-001..002 |
| 2 | Drop and restore WiFi | Reconnects on its own | FR-NW-003 |
| 3 | Start with no joinable network | Open fallback AP appears after the timeout; full GUI works on it; closes on joining | FR-NW-005..009 |
| 4 | From a LAN client | Resolves as `panelclock.local`; web service discoverable; not advertised in AP-only mode | FR-NW-010..013 |

### 8.9 Web GUI — TC-WEB-001..006

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Load the GUI offline in a stock browser | All pages load from the device; nav works; no internet needed | FR-WEB-001..005 |
| 2 | Use each config page (calibrate, WiFi, GNSS, DCF77) | Settings apply and persist; calibration live-preview works | FR-WEB-010..027, 050..053 |
| 3 | Open the status page | All status fields present and auto-refresh | FR-WEB-030..035 |
| 4 | Upload good and tampered firmware | Good installs and reboots; tampered rejected with a clear error, clock unchanged | FR-WEB-040..046 |

### 8.10 Security / FOTA — TC-SEC-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Inspect device key handling | Only the public verification key is present; it survives updates | FR-SEC-001..003 |
| 2 | Upload validly signed, unsigned, and altered packages | Only the valid one is accepted; others rejected before any write, with a logged reason | FR-SEC-004..006 |
| 3 | Deliver a signed update carrying a new key | New key installs; later updates verified against it | FR-SEC-007 |

### 8.11 Non-Functional — TC-NFR-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Measure tick-to-meter latency and page load | Meters update within ~10 ms; pages load in a few seconds; display unaffected by sync | NFR-PERF-001..003 |
| 2 | Hang the firmware; corrupt stored settings | Auto-resets within ~30 s and reboots; corrupt settings reset to defaults and logged | NFR-REL-001..003 |
| 3 | Power off / reset | Needles fall to zero promptly | NFR-PWR-001 |
| 4 | Review configurability, logging and structure | Settings change at runtime without reflash; key events logged; drive layer isolated | NFR-MNT-001..002, NFR-PORT-001 |

### 8.12 Real-Time Clock — TC-RTC-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Set a time (sync or encoder), remove main power briefly, re-boot | Clock resumes the correct time of day from the RTC | FR-RTC-001..002, FR-RTC-004 |
| 2 | Sync via GNSS/NTP/DCF77, then cold-boot offline | Boot time matches the previously-synced time (RTC written back) | FR-RTC-003 |
| 3 | Remove the backup battery, then boot | Time invalid → starts at 00:00:00; RTC indicator shows invalid | FR-RTC-005 |
| 4 | Provide a higher source while on RTC time | Higher source overrides the RTC | FR-RTC-006 |

### 8.13 Panel Indicators and Control — TC-UI-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | Drive each source to off / acquiring / valid | Each LED shows off / ≈1 Hz blink / steady accordingly; active source readable by priority | UI-IND-001..009 |
| 2 | Remove the RTC backup battery and boot | RTC LED blinks (invalid) and the clock starts at 00:00:00 | UI-IND-006, FR-RTC-005 |
| 3 | Long-press, rotate each field, short-press through H/M/S, confirm | Set mode entered; meters track edits; time written to the RTC and retained | UI-CTL-002..006, FR-RTC-004 |
| 4 | Enter set mode and wait out the timeout | Returns to normal display with the time unchanged | UI-CTL-007 |

### 8.14 OTA Update — TC-OTA-001

| Step | Method / setup | Pass criterion | Verifies |
|------|----------------|----------------|----------|
| 1 | `POST /api/ota/firmware` a validly-signed image, then `POST /api/ota/assets` a STORE-only ZIP | Firmware verified to the inactive bank, assets extracted to the inactive LittleFS, boot slot swapped only after both; device reboots into the new pair | §5.13 (paired commit) |
| 2 | Cut power partway through the firmware write, the asset write, and the commit (three runs) | After every cut the device boots the previous bank with its matching assets; no firmware/asset mismatch | §5.13 (power-safe, coupled assets) |
| 3 | `POST /api/ota/firmware` only; send no assets | After `OTA_ASSET_FALLBACK_MS` the firmware-only update commits and boots | §5.13 (120 s fallback) |
| 4 | Upload firmware that never reaches a healthy mark; let it boot repeatedly | After `OTA_FAIL_THRESHOLD` failed boots the device rolls back to the previous bank; a planned reboot (`ota_intentional`) is not counted | §5.13 (3-fail rollback) |
| 5 | `POST /api/ota/assets` a DEFLATE-compressed ZIP; separately POST an unsigned / altered firmware | DEFLATE rejected with a diagnostic; unsigned/altered firmware rejected before any boot-slot change | §5.13, DC-009, FR-SEC-004..006 |
| 6 | `POST /api/ota/assets` only (no firmware) on a running unit | Assets written to the **active** LittleFS; boot slot unchanged | §5.13 (asset-only) |

---

## 9. Design Constraints

These constraints (moved from PMC-FRS-001) bound the implementation. Hardware specifics are detailed in PMC-HTD-001.

| ID | Constraint |
|----|------------|
| DC-001 | Target is the ESP32-S3-WROOM-1-N16R8 module (16 MB quad flash, 8 MB octal PSRAM) on the project PCB. The firmware shall be built on **Espressif ESP-IDF 5.x** (PlatformIO, `framework = espidf`) running on FreeRTOS. The Arduino framework, other application frameworks, bare-metal and alternative RTOSes are not permitted. |
| DC-002 | The ESP32-S3 has no analogue DAC; all meter drive is produced by PWM + RC filtering via the LEDC peripheral. |
| DC-003 | Each meter is modified so 3 V gives full-scale deflection; design point 0–3 V (duty 0–232); the 3.3 V GPIO maximum is not used as full scale (see PMC-HTD-001 §4.3). |
| DC-004 | PWM is 8-bit (0–255) at **156.25 kHz** from the 80 MHz LEDC clock (exact ÷2 integer divider) — 256 discrete positions, no fractional duty. The frequency is chosen so the PWM fundamental and its harmonics stay clear of the 77.5 kHz DCF77 band (PMC-HTD-001 §5, §7.4, HW-011). |
| DC-005 | The flash partition table must include two OTA application banks **and two paired LittleFS asset partitions, plus an `otadata` and a `coredump` partition** (§3.3, §5.13); it is a build-time configuration and cannot be changed by FOTA. |
| DC-006 | The FOTA public key is embedded in the firmware binary at build time, excluded from OTA writes, and not modifiable at runtime. |
| DC-007 | DCF77 reception is limited to Central Europe (≈ 2 000 km from Mainflingen) and depends on local signal strength; it is an optional, region-specific source the design shall not rely on. |
| DC-008 | The DS1307Z RTC operates at 5 V; its I2C bus is level-shifted to the 3.3 V logic of the ESP32-S3, and it requires a 32.768 kHz crystal and a coin-cell backup (PMC-HTD-001 §8). |
| DC-009 | The on-device OTA ZIP extractor accepts STORE (method 0) entries only; DEFLATE is rejected (§5.13). The release asset bundle must therefore be built STORE-only. |
| DC-010 | The `coredump` partition must be erased on a new unit's first (greenfield) flash; OTA cannot write or erase it (§3.3, §5.13). |

---

## 10. External Interfaces

Moved from PMC-FRS-001. Pin-level detail is in PMC-HTD-001 §3; protocol and peripheral detail is in the module designs (§5).

### 10.1 Hardware Interfaces

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

### 10.2 Software and Protocol Interfaces

| ID | Interface |
|----|-----------|
| IC-SW-001 | NTP per NTPv4 (RFC 5905), UDP port 123. |
| IC-SW-002 | GNSS via NMEA 0183; at minimum `$GPRMC` / `$GNRMC` are parsed. |
| IC-SW-003 | Web GUI over HTTP/1.1, TCP port 80; HTTPS is out of scope. |
| IC-SW-004 | FOTA updates firmware and web assets together over `POST /api/ota/firmware`, `POST /api/ota/assets` and `GET /api/ota/status`; dual-slot app banks + paired dual LittleFS; assets delivered as a STORE-only ZIP; firmware Ed25519-signed (§5.13). |
| IC-SW-005 | DCF77 decoded from the one-minute amplitude-modulated time-code frame (PTB DCF77 standard). |

### 10.3 Human Interfaces

| ID | Interface |
|----|-----------|
| IC-HMI-001 | Primary UI is the embedded web GUI (FR-WEB-*). |
| IC-HMI-002 | Secondary diagnostic interface: serial debug stream at 115 200 baud, 8-N-1, on UART0 / USB-CDC. |
