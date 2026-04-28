# PWMTest — PWM Panel Meter Verification

## Purpose

Verify whether PWM output from the ESP32-S3-WROOM-1, filtered through a single RC low-pass stage, produces a stable enough DC voltage to reliably deflect an analog panel meter. The ESP32-S3 has no built-in DAC, so PWM + RC filter is the primary candidate for driving the three meters in the final design.

Two potentiometers allow live adjustment of PWM frequency and duty cycle. Serial output reports current settings every second.

---

## Hardware

**MCU:** ESP32-S3-WROOM-1  
**Framework:** Arduino via PlatformIO (`esp32-s3-devkitc-1` board target)  
**Panel meter:** Siemens 1604P DC10.00GF024T005 (moving-coil, 10 V FSD)

---

## PWM Frequency Range: 20 kHz – 80 kHz

| Parameter | Value | Reason |
|---|---|---|
| Lower bound | 20 kHz | Just above human hearing — no audible whine |
| Upper bound | 80 kHz | Maximum practical with 8-bit resolution at 80 MHz LEDC clock |
| Resolution | 8-bit (0–255) | Sufficient for meter deflection across full range |
| Pot mapping | Linear: ADC 0–4095 → 20–80 kHz | Simple, predictable |

---

## RC Low-Pass Filter: R = 1 kΩ, C = 10 µF

| Parameter | Value |
|---|---|
| Cut-off frequency | fc = 1 / (2π × 1 kΩ × 10 µF) ≈ **15.9 Hz** |
| Attenuation at 20 kHz | ≈ −62 dB → residual ripple < 3 mV pk-pk |
| Panel meter bandwidth | ~1–3 Hz (mechanical) — well within filter rolloff |
| Component notes | Electrolytic or tantalum; observe polarity (+ toward GPIO15) |

---

## GPIO Pin Assignments

| Signal | GPIO | Notes |
|---|---|---|
| POT1 wiper (frequency) | **GPIO1** | ADC1_CH0 — safe, no WiFi conflict |
| POT2 wiper (duty cycle) | **GPIO2** | ADC1_CH1 |
| PWM output | **GPIO15** | LEDC channel 0 — not a strapping pin |
| Power | 3V3 pin | High side of both pots |
| Common | GND pin | Low side of both pots, C−, meter − |

Strapping pins (GPIO0, GPIO3, GPIO45, GPIO46) are avoided.

---

## Breadboard Wiring

```
ESP32-S3-WROOM-1
┌─────────────┐
│  3V3  ──────┼─── POT1 pin1 ───── POT2 pin1
│  GND  ──────┼─── POT1 pin3 ───── POT2 pin3
│  GPIO1 ─────┼─── POT1 wiper (pin2)
│  GPIO2 ─────┼─── POT2 wiper (pin2)
│  GPIO15 ────┼─── R (1 kΩ) ─┬─── Panel meter (+)
│             │               C (10 µF, + side toward GPIO15)
│             │               │
│  GND  ──────┼───────────────┴─── Panel meter (−)
└─────────────┘
```

**Connections summary (5 jumper wires + 2 passives + meter):**

| ESP32-S3 pin | Connects to |
|---|---|
| 3V3 | POT1 pin 1 and POT2 pin 1 (high side) |
| GND | POT1 pin 3, POT2 pin 3, C−, meter − |
| GPIO1 | POT1 wiper |
| GPIO2 | POT2 wiper |
| GPIO15 | R (1 kΩ) → node → meter (+) and C+ |

---

## Serial Output Format

Printed every 1 second at 115200 baud:

```
Freq: 40000 Hz | Duty: 128/255 ( 50.2%) | V_out ~1.65 V | ADC_f=2048 ADC_d=2050
```

---

## Verification Checklist

- [ ] Flash: `pio run -t upload`
- [ ] Open monitor: `pio device monitor`
- [ ] POT1 fully CCW → Freq ≈ 20 kHz; fully CW → Freq ≈ 80 kHz
- [ ] POT2 fully CCW → Duty 0%, meter at zero deflection
- [ ] POT2 fully CW → Duty 100%, meter at full-scale
- [ ] POT2 mid → meter settles near mid-scale (~1.65 V)
- [ ] Needle stable with no visible flutter at all pot positions
- [ ] No audible noise from meter coil or RC network

If flutter is observed: confirm C = 10 µF (not µH), check polarity, verify GND continuity.
