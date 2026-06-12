# DCFTest — DCF77 decoder proof of concept

Proof of concept for the PanelMeterClock DCF77 time source: receive the
demodulated 77.5 kHz time-code signal, decode and validate the one-minute
frame, and report civil CET/CEST time plus derived UTC over the serial
console.

An **ESP-IDF** project (no Arduino layer) with **two interchangeable
decoder components**, both RTOS-aware (dedicated FreeRTOS task, core 0,
priority 7 per PMC-STD-001 §4) with the same callback-driven shape:

* `components/dcf77` — **simple edge-timing decoder**, the PMC-STD-001
  §5.10 baseline. Edge interrupt → pulse-width windows → frame validation →
  consecutive-frame confirmation. Original code, no license constraints.
* `components/dcf77p` — **phase-locked exhaustive decoder**, a C port of
  Udo Klein's blinkenlight decoder (www.blinkenlight.net). 1 kHz sampling →
  100-bin phase convolution → per-second tick classification → 60-bin
  sync-mark scoring → per-field maximum-likelihood (Hamming) binning with
  prediction-driven bin rings. Decodes through noise where individual
  pulses are unreadable. **GPL-3.0** (see below).

`src/main.c` is only the PoC harness: it wires the selected decoder's
callbacks to console printing and stands in for `tick_task`'s epoch
bookkeeping.

```
DCFTest/
├── platformio.ini            framework = espidf, board = lolin_s3
├── CMakeLists.txt            standard IDF project stub
├── sdkconfig.defaults        FREERTOS_HZ=1000, 16 MB flash
├── components/
│   ├── dcf77/                ← edge-timing decoder component
│   │   ├── CMakeLists.txt
│   │   ├── include/dcf77.h
│   │   └── dcf77.c
│   └── dcf77p/               ← phase-locked exhaustive decoder component
│       ├── CMakeLists.txt
│       ├── LICENSE           GPL-3.0 (full text)
│       ├── include/dcf77p.h
│       └── dcf77p.c
└── src/
    ├── CMakeLists.txt        (PRIV_REQUIRES dcf77 dcf77p driver)
    └── main.c                PoC harness: app_main(), printing, LED
```

## Choosing the decoder

`src/main.c`, first line after the header comment:

```c
#define USE_PHASE_DECODER 1   // 1 = dcf77p (Klein port), 0 = dcf77 (edge timing)
```

**License consequence:** `dcf77p` is a derivative of GPL-3.0 code, with the
full text in `components/dcf77p/LICENSE`. Any firmware image that links
`dcf77p` must be distributed under GPL-3.0 terms. The `dcf77` component
carries no such obligation — with `USE_PHASE_DECODER 0` the component is
still compiled but not linked into the image. This was an accepted
trade-off for this research (FDS/TDS deliberately unchanged; the STD still
specifies the simple decoder for the firmware).

Port deviations from the reference sketch
(`Documentation/src/blinkenLightDC77DecoderAll.c`) are documented in
`dcf77p.c`: the year-decoder bit-56 weight bug is fixed, the unused
200-line signal predictor is replaced by its one-line leap-second closed
form, and the AVR busy-wait output is replaced by task-context callbacks.

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

With the phase-locked decoder (`USE_PHASE_DECODER 1`) the output instead
shows the acquisition stages and per-minute tick classifications:

```
Decoder: dcf77p — phase-locked exhaustive (Udo Klein port, GPL-3.0)
...
==== state: OFF -> ACQUIRING_PHASE ====
[stat] ACQUIRING_PHASE | sec 0 | ticks S:0 ?:0 0:0 1:0 | phase 64-58 | ...
==== state: ACQUIRING_PHASE -> ACQUIRING_SECOND ====
[tick] 0100110100101S01101011000010100010010101011000110010010S010  (no second lock)
==== state: ACQUIRING_SECOND -> DECODING ====
[dec ] --:--:23  date 20----    (fields locking)
[dec ] --:36:24  date 2026-06-12  (fields locking)
[min ] 00011101001101000101101011000010100010010101011000110010010
==== state: DECODING -> TIME_VALID ====
[time] full date/time determined by exhaustive decoding
[time] 2026-06-12 Fri 14:36:31 CEST (UTC 12:36:31)
[time] 2026-06-12 Fri 14:36:32 CEST (UTC 12:36:32)
```

With good signal the phase lock arrives within ~1 minute and the full time
typically within 3–6 minutes; with poor signal the exhaustive decoder keeps
integrating and can take tens of minutes — but it converges at noise levels
where the edge-timing decoder never produces a clean frame at all.

## How it works — dcf77 (edge timing)

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

## How it works — dcf77p (phase-locked exhaustive)

```
1 kHz esp_timer cb ──(10 ms majority bits)──▶ FreeRTOS queue ──▶ dcf77p task
(sampling only)                                                 (core 0, prio 7)
```

* **Phase lock** — each 10 ms slot feeds a saturating up/down counter in
  one of 100 phase bins (cap 300 ≈ 5 min of integration). A sliding
  convolution with the expected pulse shape (2× weight first 100 ms, 1×
  second 100 ms) finds the second boundary; the same kernel 200 ms out of
  phase estimates the noise floor. No edges are ever measured.
* **Tick classification** — the 220 ms window after each boundary is split
  into two 110 ms halves; their majorities yield sync-mark / 0 / 1 /
  undefined per second.
* **Second lock** — a 60-bin scoring scheme (+6 for a sync mark, ±1/−2
  cross-evidence from 0/1 bits, exploiting "bit 0 = 0, bit 20 = 1") finds
  which second of the minute we are in.
* **Exhaustive field decoding** — every minute, each calendar field scores
  *all* its candidate values (60 minutes, 24 hours, 31 days, …) by Hamming
  distance against the received bits (parity included for minute/hour);
  evidence accumulates per candidate in rings that the predicted time keeps
  rotating in lock-step (timezone jumps and month lengths handled by the
  prediction). The best candidate wins once it leads the runner-up by ≥ 2 —
  effectively a maximum-likelihood decoder integrating over minutes.
* **Output** — at every predicted second boundary the controller advances
  the reconstructed time and publishes it (`DCF77P_EVT_SECOND` + tick
  callback, FR-DCF-012); `valid` is set once all fields are determined and
  mutually plausible, and the UTC epoch is derived from the zone bits
  (FR-DCF-009). Trust is structural here: a field value only emerges after
  consistent evidence across minutes, which subsumes the two-frame
  confirmation idea of FR-DCF-007.

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
* The simple edge-timing decoder needs a reasonably clean signal; for
  marginal sites the phase-locked `dcf77p` component is the implemented
  alternative (`USE_PHASE_DECODER 1`, GPL-3.0).
* `dcf77p` ports the decoder stages of Klein's sketch but not the later
  library's local-clock flywheel or crystal frequency tuning — during deep
  fades the published seconds keep ticking from the phase prediction, but
  long outages degrade the locks rather than free-running on a disciplined
  local oscillator.
* `dcf77p` samples via a 1 kHz `esp_timer` callback (task-dispatched). That
  is jitter-tolerant by design (10 ms bins, minutes of integration), but a
  heavily loaded system could still starve it; the firmware-grade upgrade
  is a GPTimer ISR feeding the same queue.
