# DCFTest — DCF77 decoder proof of concept

Proof of concept for the PanelMeterClock DCF77 time source: receive the
demodulated 77.5 kHz time-code signal, decode and validate the one-minute
frame, and report civil CET/CEST time plus derived UTC over the serial
console.

An **ESP-IDF** project (no Arduino layer). The decoder is packaged as a
self-contained IDF component, the RTOS-aware module that PMC-STD-001 §5.10
specifies for the firmware (`dcf77_task`: simple edge-timing decoder, no
external library). `src/main.c` is only the PoC harness: it wires the
module's callbacks to console printing and stands in for `tick_task`'s
epoch bookkeeping.

```
DCFTest/
├── platformio.ini            framework = espidf, board = lolin_s3
├── CMakeLists.txt            standard IDF project stub
├── sdkconfig.defaults        CONFIG_FREERTOS_HZ=1000
├── components/
│   └── dcf77/                ← the reusable decoder component
│       ├── CMakeLists.txt    (PRIV_REQUIRES driver esp_timer)
│       ├── include/dcf77.h
│       └── dcf77.c
└── src/
    ├── CMakeLists.txt        (PRIV_REQUIRES dcf77 driver)
    └── main.c                PoC harness: app_main(), printing, LED
```

## Hardware

WEMOS LOLIN S3 dev board + DCF77 receiver module (CME6005-based, see
`Documentation/dcf77.md`). Pinning per PMC-HTD-001 §3/§7:

| Module pin | ESP32-S3 GPIO | Direction | Notes |
|------------|---------------|-----------|-------|
| T (time-code out) | 11 | Input | Internal pull-up enabled (open-collector output, IC-HW-005) |
| P1 (PON / power down) | 12 | Output | **Low = receiver on** (IC-HW-006) |
| V | 3V3 | — | ≈ 1–2 mA |
| G | GND | — | |
| DCF status LED (optional) | 5 | Output | Blinks ~1 Hz while acquiring, steady when valid (PMC-GUI-001) |

Site the ferrite antenna away from the ESP32, USB supply and other switching
noise, broadside toward Frankfurt (PMC-HTD-001 §7.4). Reception is usually
poor right next to a PC/monitor — a metre of separation helps a lot.

## Build and monitor

```
pio run -t upload
pio device monitor
```

115200 baud. Output goes to the default ESP-IDF console = **UART0 / the
CH340 USB port**; the board's native-USB port is unused.

Notes:

* The first build downloads the ESP-IDF framework and toolchain packages
  (large, several minutes); subsequent builds are normal.
* `sdkconfig.defaults` pins `CONFIG_FREERTOS_HZ=1000` (IDF defaults to
  100 Hz) so the decoder's `pdMS_TO_TICKS()`-paced edge flush and watchdog
  keep millisecond granularity. The generated `sdkconfig.esp32s3` is
  git-ignored; delete it to regenerate from the defaults.

## What the serial output looks like

```
DCF77 decoder proof of concept — PanelMeterClock Research/DCFTest
...
[bit ]  ? = 1  (197 ms)  waiting for minute mark
[dcf ] minute mark (1986 ms gap)
[bit ]  0 = 0  (103 ms)  M  start of minute (always 0)
[bit ]  1 = 0  (104 ms)  civil warning / weather
...
[bit ] 58 = 1  (195 ms)  P3 date parity
[dcf ] minute mark (1992 ms gap)
[frame] 0 00111010011010 0 0 1 0 0 1 10101100 0010100 010010 101 01100 011001001
[frame] OK: Fri 2026-06-12 14:35:00 CEST = 12:35 UTC
[frame] awaiting confirmation by the next frame (FR-DCF-007)
... (one more minute of bits) ...
[dcf ] minute mark (1989 ms gap)
[frame] 0 01001011010010 0 0 1 0 0 1 01101100 0010100 010010 101 01100 011001001
[frame] OK: Fri 2026-06-12 14:36:00 CEST = 12:36 UTC
==========================================================
[time] CONFIRMED by two consecutive frames (FR-DCF-007)
==========================================================
[time] 2026-06-12 14:36:00 CEST (UTC 12:36:00)
[time] 2026-06-12 14:36:01 CEST (UTC 12:36:01)
...
[stat] LOCKED | second 23 | edges 412 (3 glitches) | bits 142 ok / 1 bad | frames 2 ok / 0 bad | confirmed 1
```

A first confirmed time takes 2–5 minutes with decent reception: partial
first minute → one full frame → confirmation by the next frame.

## How it works

```
GPIO 11 CHANGE ISR ──(timestamped edges)──▶ FreeRTOS queue ──▶ dcf77 task
                                                              (core 0, prio 7,
                                                               PMC-STD-001 §4)
```

* **Glitch filter** — two edges closer together than 20 ms annihilate, so RF
  spikes never reach the decoder. Acceptance of a real edge is delayed by at
  most ~45 ms; events carry the original edge timestamp.
* **Bit classification** — mark width 40–130 ms → `0`, 140–260 ms → `1`,
  anything else poisons the frame and is reported (`PULSE_REJECT`).
* **Second/minute sync** — leading-edge-to-leading-edge period 800–1200 ms →
  next second; 1700–2300 ms → missing 59th mark = minute boundary. Other
  periods drop bit alignment (`RESYNC`).
* **Frame validation (FR-DCF-006)** — 59 bits, bit 0 = 0, bit 20 = 1,
  Z1 ≠ Z2, even parity P1/P2/P3, BCD digit validity, field ranges,
  day-in-month, and weekday cross-checked against the date.
* **Confirmation (FR-DCF-007)** — a decoded time is only reported as
  `TIME_CONFIRMED` once two consecutive frames are exactly one minute apart.
* **UTC (FR-DCF-009)** — broadcast time is civil CET/CEST; the module derives
  the UTC epoch using the Z1/Z2 zone bits (CET = UTC+1, CEST = UTC+2).
* **Ticks (FR-DCF-012)** — every accepted mark leading edge fires a tick
  callback; in the firmware this feeds `tick_task` as the Priority-2 tick
  source.

## Polarity (FR-DCF-004)

The decoder expects the carrier-reduction mark as a **HIGH** pulse by
default. If your module idles high and pulls low during the mark, the PoC
will print rejected marks of ~800/900 ms and a hint; set

```c
cfg.signal_inverted = true;
```

in `app_main()`.

## Reusing the component in the firmware

* Copy `components/dcf77/` into the firmware's `components/` directory and
  add `dcf77` to the consumer's `PRIV_REQUIRES` — done. The component
  depends only on `driver` (GPIO), `esp_timer` and FreeRTOS; the header is
  C++-safe (`extern "C"` guards). Multiple instances are supported (the ISR
  receives the instance pointer).
* The component installs the shared GPIO ISR service with flags 0 and
  tolerates `ESP_ERR_INVALID_STATE` if the firmware installed it already
  (the GNSS 1PPS and encoder inputs will share it). If the firmware installs
  the service with `ESP_INTR_FLAG_IRAM`, replace the `gpio_get_level()` call
  in the ISR with a register read (see note in `dcf77.c`).
* Callbacks run in the decoder task context — keep them short; hand data to
  other tasks via the usual primitives (the firmware will set the shared
  epoch and notify `tick_task` instead of printing).
* `dcf77_stop()` disables the pin interrupt, removes the handler and holds
  PON off, matching the "disabled consumes no resources" requirement
  (FR-DCF-002); the firmware's NVS flag `dcf77_enabled` maps to start/stop.
* Constants meant for `config.h` (PMC-STD-001 §7): `DCF77_SIGNAL_GPIO` 11,
  `DCF77_PON_GPIO` 12, plus the timing windows in `dcf77_config_default()`.

## Known limitations (PoC scope)

* No free-running bridge across reception gaps — the printed clock simply
  stops when marks stop; in the firmware that is `tick_task`'s job
  (FR-TIM-007). Re-confirmation resumes it.
* `signal_lost_ms` is 5 s for fast bench feedback; the firmware uses
  `DCF77_SIGNAL_LOST_TIMEOUT_S` = 300 (FR-DCF-010).
* A leap-second minute (60 marks) is rejected as a 60-bit frame — one bad
  minute, then normal recovery.
* The simple edge-timing decoder needs a reasonably clean signal. If
  reception at the installation site turns out marginal, the upgrade path is
  Udo Klein's phase-locked exhaustive decoder
  (`Documentation/src/blinkenLightDC77DecoderAll.c`,
  https://github.com/udoklein/dcf77), which this PoC's bit layout and tick
  classification are based on.
