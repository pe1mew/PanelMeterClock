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
| Module | WEMOS LOLIN S3 (ESP32-S3-WROOM-1) |
| CPU | Xtensa LX7 dual-core, 240 MHz |
| Flash | 16 MB (QSPI) |
| RAM | 512 KB SRAM + 8 MB PSRAM (WROOM-1) |
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
| `ESPAsyncWebServer` + `AsyncTCP` | GitHub (me-no-dev) | Embedded HTTP server and OTA streaming (FR-WEB-001..046) | Non-blocking I/O keeps server responsive during flash writes (resolved: TBD-007) |
| `Preferences` | Built-in (ESP32 Arduino core) | NVS key-value storage (FR-DSP-010..012, FR-NW-001) | Thin C++ wrapper over ESP-IDF NVS |
| Custom POSIX TZ parser | In-tree (`src/posix_tz.h`) | DST rule engine (FR-DST-001..006) | POSIX TZ strings are compact, updatable via NVS without firmware rebuild (resolved: TBD-006) |
| GNSS NMEA parser | TBD — TinyGPS++ or custom | NMEA sentence parsing (FR-GPS-003) | Lightweight; parses $GPRMC/$GNRMC only |
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
| `tick_task` | APP (1) | 9 | 2048 | Maintains UTC epoch; synthesises software 1PPS via FreeRTOS timer; arbitrates tick source (FR-TIM-001..006, FR-BOOT-016) |
| `ntp_task` | PRO (0) | 5 | 4096 | Issues NTP queries; disciplines the epoch (FR-NTP-001..004, FR-BOOT-006) |
| `wifi_task` | PRO (0) | 6 | 4096 | STA connect/reconnect with exponential back-off; AP fallback; mDNS start/stop (FR-NW-001..013) |
| `gnss_task` | PRO (0) | 7 | 4096 | UART read; NMEA parsing; 1PPS interrupt latch (FR-GPS-001..009); only created when GNSS is enabled |
| `http_task` | PRO (0) | 4 | 8192 | Embedded web server; serves all GUI pages (FR-WEB-001..046) |

**Inter-task communication:**

| Signal | Mechanism | Produced by | Consumed by |
|--------|-----------|-------------|-------------|
| 1PPS tick event | `xTaskNotifyGive` | `tick_task` (software) or GNSS ISR (hardware) | `display_task` |
| UTC epoch update | Shared `volatile uint64_t` + `portENTER_CRITICAL` | `tick_task`, `ntp_task`, `gnss_task` | All |
| Tick source selection | Atomic flag (`volatile uint8_t`) | `wifi_task`, `gnss_task`, `ntp_task` | `tick_task` |
| Config change | FreeRTOS event group | `http_task` | `wifi_task`, `ntp_task`, `gnss_task` |

---

## 5. Module Design

### 5.1 Boot State Machine

The firmware progresses through three phases (FR-BOOT-001..017). The table below is the authoritative state-transition design.

| Current state | Event / guard | Next state | Actions on transition |
|--------------|---------------|------------|----------------------|
| *(start)* | Power-on | `PHASE_1` | Initialise epoch to 0; display 00:00:00; start 1 Hz software tick in `tick_task` |
| `PHASE_1` | Always (immediately) | `PHASE_2` | Create `wifi_task`; begin STA association attempt |
| `PHASE_2` | Always (if GNSS enabled) | `PHASE_3` | Create `gnss_task`; start GNSS UART; enable 1PPS GPIO interrupt |
| `PHASE_2` | WiFi association success | `PHASE_2` (NTP attempt) | Issue NTP query from `ntp_task`; start mDNS; set DHCP hostname |
| `PHASE_2` | Valid NTP response | `STEADY` (NTP) | Set epoch from NTP; switch tick source to synthesised 1PPS; log sync event (NFR-MNT-002) |
| `PHASE_2` | WiFi or NTP failure | `PHASE_2` (retry) | Increment retry counter; log; wait 15 s then retry (FR-BOOT-008) |
| `PHASE_3` | GNSS fix valid | `STEADY` (GNSS override) | Override epoch from GNSS; switch tick source to hardware 1PPS; log (NFR-MNT-002) |
| `PHASE_3` | GNSS fix lost (> 5 s) | `PHASE_3` (degraded) | Fall back to NTP synthesised 1PPS; log (FR-BOOT-015) |
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
GNSS enabled AND fix valid?
  YES → hardware 1PPS ISR drives display_task (Priority 1)
  NO  → NTP sync obtained?
          YES → software FreeRTOS timer drives display_task (Priority 2)
          NO  → free-running 1 Hz software timer (Priority 3)
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
├── /status        Clock Status      (FR-WEB-030..034)
├── /calibrate     Meter Calibration (FR-WEB-010..015)
├── /wifi          WiFi Configuration(FR-WEB-020..024)
├── /gnss          GNSS Configuration(FR-WEB-025..027)
└── /update        Firmware Update   (FR-WEB-040..046)
```

All pages share a navigation bar with links to all five endpoints (FR-WEB-002..003).

**FOTA flow** (FR-WEB-040..046):

1. Browser POSTs firmware binary + detached signature file.
2. Server buffers signature; computes SHA-256 digest of the binary stream.
3. Verifies Ed25519/RSA-2048 signature against the embedded public key (FR-SEC-004).
4. On success: writes binary to inactive OTA partition; calls `esp_ota_set_boot_partition()`; schedules reboot in 10 s.
5. On failure: discards data; returns error code to browser (FR-WEB-042).

### 5.9 NVS Storage

All persistent configuration uses the ESP-IDF `Preferences` library (namespace `"clock"` and `"meter"`). Keys, types, and defaults are listed in Section 6 below. NVS corruption is detected at startup by checking the `Preferences.begin()` return value; on corruption the namespace is cleared and factory defaults are written (NFR-REL-003).

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
