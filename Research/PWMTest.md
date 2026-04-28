# Plan: Research/PWMTest — PWM Panel Meter Verification

## Context

The project uses an ESP32-S3-WROOM-1 to drive three analog panel meters. The ESP32-S3 has **no built-in DAC**, so PWM + RC low-pass filter is the candidate approach. This test program verifies whether PWM at ultrasonic frequencies, filtered through a single RC stage, produces a stable enough DC voltage to reliably deflect an analog panel meter. Two potentiometers allow live adjustment of frequency and duty cycle while serial output reports the current settings every second.

---

## Technical Decisions

### PWM Frequency Range: 20 kHz – 80 kHz

- Lower bound: 20 kHz (just above human hearing — no audible whine from the filter/meter coil)
- Upper bound: 80 kHz (practical maximum with 8-bit resolution at 80 MHz LEDC clock)
- Resolution: **8-bit** (256 steps) — sufficient for meter deflection, and allows the full 20–80 kHz range without prescaler overflow
- Pot maps linearly: ADC 0–4095 → 20 kHz–80 kHz

### RC Low-pass Filter: R = 1 kΩ, C = 10 µF

- Cut-off frequency: **fc = 1 / (2π × 1000 × 10×10⁻⁶) ≈ 15.9 Hz**
- Panel meters have mechanical time constants of ~0.3–1 s, so 16 Hz cutoff is well-matched
- Attenuation at 20 kHz: ≈ 1/1258 ≈ **−62 dB** → residual ripple < 3 mV peak-to-peak at full scale → negligible
- Use electrolytic or tantalum 10 µF (observe polarity; + toward GPIO output)

### GPIO Pin Assignments

| Signal | GPIO | Notes |
|---|---|---|
| POT1 wiper (frequency) | GPIO1 | ADC1_CH0 — safe with no WiFi |
| POT2 wiper (duty cycle) | GPIO2 | ADC1_CH1 |
| PWM output | GPIO15 | LEDC CH0 — not a strapping pin |
| 3.3V | 3V3 pin | Power for both pots |
| GND | GND pin | Common ground |

Strapping pins (GPIO0, GPIO3, GPIO45, GPIO46) are deliberately avoided.

---

## Files to Create

```
Research/
└── PWMTest/
    ├── PWMTest_Plan.md      ← this plan as project documentation
    ├── platformio.ini
    └── src/
        └── main.cpp
```

---

## File Contents

### `Research/PWMTest/platformio.ini`

```ini
[env:esp32s3]
platform      = espressif32
board         = esp32-s3-devkitc-1
framework     = arduino
monitor_speed = 115200
build_flags   =
    -DARDUINO_USB_CDC_ON_BOOT=1
```

> `esp32-s3-devkitc-1` uses the ESP32-S3-WROOM-1 module — correct chip target.  
> `ARDUINO_USB_CDC_ON_BOOT=1` routes Serial over USB-CDC (no separate USB-UART adapter needed if using native USB port).

---

### `Research/PWMTest/src/main.cpp`

```cpp
#include <Arduino.h>

const int POT_FREQ_PIN   = 1;      // ADC1_CH0
const int POT_DUTY_PIN   = 2;      // ADC1_CH1
const int PWM_OUT_PIN    = 15;     // LEDC output
const int PWM_CHANNEL    = 0;
const int PWM_RESOLUTION = 8;      // 8-bit: duty 0–255
const int FREQ_MIN       = 20000;  // Hz
const int FREQ_MAX       = 80000;  // Hz

int  activeFreq = 0;
unsigned long lastPrint = 0;

void setup() {
    Serial.begin(115200);

    analogReadResolution(12);
    analogSetPinAttenuation(POT_FREQ_PIN, ADC_11db);
    analogSetPinAttenuation(POT_DUTY_PIN, ADC_11db);

    ledcSetup(PWM_CHANNEL, FREQ_MIN, PWM_RESOLUTION);
    ledcAttachPin(PWM_OUT_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0);

    Serial.println("PWMTest ready. Adjust pots to change frequency and duty cycle.");
    Serial.println("GPIO1=Freq pot | GPIO2=Duty pot | GPIO15=PWM out");
}

void loop() {
    int rawFreq = analogRead(POT_FREQ_PIN);
    int rawDuty = analogRead(POT_DUTY_PIN);

    int freq      = map(rawFreq, 0, 4095, FREQ_MIN, FREQ_MAX);
    int dutyCycle = map(rawDuty, 0, 4095, 0, 255);

    if (abs(freq - activeFreq) > 200) {
        ledcSetup(PWM_CHANNEL, freq, PWM_RESOLUTION);
        activeFreq = freq;
    }

    ledcWrite(PWM_CHANNEL, dutyCycle);

    if (millis() - lastPrint >= 1000) {
        lastPrint = millis();
        float dutyPct    = dutyCycle / 255.0f * 100.0f;
        float voltageOut = dutyCycle / 255.0f * 3.3f;
        Serial.printf(
            "Freq: %5d Hz | Duty: %3d/255 (%5.1f%%) | V_out ~%.2f V | ADC_f=%4d ADC_d=%4d\n",
            activeFreq, dutyCycle, dutyPct, voltageOut, rawFreq, rawDuty
        );
    }
}
```

---

## Breadboard Wiring

```
ESP32-S3-WROOM-1
┌─────────────┐
│  3V3  ──────┼─── POT1 pin1 ───── POT2 pin1
│  GND  ──────┼─── POT1 pin3 ───── POT2 pin3
│  GPIO1 ─────┼─── POT1 wiper (pin2)
│  GPIO2 ─────┼─── POT2 wiper (pin2)
│  GPIO15 ────┼─── R (1 kΩ) ──┬─── Panel meter (+)
│             │               C (10 µF, + side toward GPIO15)
│             │               │
│  GND  ──────┼───────────────┴─── Panel meter (−)
└─────────────┘
```

Summary of breadboard connections (5 wires + RC + meter):

| ESP32-S3 pin | Connect to |
|---|---|
| 3V3 | Both pot pin 1 ends (high side) |
| GND | Both pot pin 3 ends (low side) + C− + meter − |
| GPIO1 | POT1 wiper |
| GPIO2 | POT2 wiper |
| GPIO15 | R (1 kΩ) in-line → meter + and C+ to GND |

---

## Verification

1. Flash with `pio run -t upload`
2. Open serial monitor (`pio device monitor`) — confirm 1 s interval output
3. Rotate POT1 fully CCW → frequency should read ~20 kHz; fully CW → ~80 kHz
4. Rotate POT2 fully CCW → duty 0%, meter at zero; fully CW → duty 100%, meter full-scale
5. Mid-point on POT2 → meter should settle at approximately mid-scale (~1.65 V)
6. Observe meter needle: should be stable with no visible flutter at any pot position
7. If meter shows flutter, verify C polarity and value (10 µF minimum)