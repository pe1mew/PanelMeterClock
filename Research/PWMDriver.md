# PWMDriver

PlatformIO project for the ESP32-S3 (WEMOS LOLIN S3).  
Drives three independent LEDC PWM outputs that feed RC low-pass filters to produce analogue voltages for panel meters.

---

## Hardware

| Signal | GPIO | LEDC channel | LEDC timer |
|--------|------|--------------|------------|
| PWM ch0 | 15 | LEDC_CHANNEL_0 | LEDC_TIMER_0 |
| PWM ch1 | 16 | LEDC_CHANNEL_1 | LEDC_TIMER_1 |
| PWM ch2 | 17 | LEDC_CHANNEL_2 | LEDC_TIMER_2 |
| Freq pot | GPIO 1 | ADC1_CH0 | — |
| Duty pot | GPIO 2 | ADC1_CH1 | — |

**RC filter per channel:** R = 1 kΩ, C = 10 µF → f_c ≈ 16 Hz  
**Supply voltage:** 3.3 V  
**Output voltage:** `V_out = duty / 255 × 3.3 V`

Useful duty reference values:

| Duty (/255) | V_out |
|-------------|-------|
| 77  | ~1.00 V |
| 86  | ~1.11 V |
| 128 | ~1.65 V (50%) |
| 255 | 3.30 V (100%) |

---

## Driver design

Each `pwm_driver_t` instance owns exactly one LEDC channel and one LEDC timer.  
Instances share **no** global state — mutex, frequency cache, channel, and timer are all per-instance fields.

```
pwm_driver_t
├── ledc_channel_t    channel       LEDC hardware channel
├── ledc_timer_t      timer         LEDC hardware timer (dedicated per instance)
├── SemaphoreHandle_t mutex         FreeRTOS mutex for thread safety
└── uint32_t          active_freq   last committed frequency (Hz)
```

### Duty update mechanism

Duty is written directly to hardware registers rather than through `ledc_set_duty` + `ledc_update_duty`, to guarantee a glitch-free update at the next PWM cycle boundary:

1. Write new value to the shadow duty register (Q4 fixed-point format, 4 fractional bits: `duty_8bit << 4`).
2. Set `DUTY_START` in `CONF1` — self-clearing bit that arms the update at the next cycle boundary.
3. Set `PARA_UP` in `CONF0` — latches shadow registers into active registers (mandatory for low-speed channels).

Low-speed channel registers have a uniform stride of `0x14` between channels:

```
LEDC_LSCHn_<REG> = LEDC_LSCH0_<REG> + n × 0x14
```

### Frequency hysteresis

`pwm_driver_set_freq` only reconfigures the timer when the requested frequency differs from the last committed frequency by more than **200 Hz**, preventing spurious timer resets from ADC noise.

---

## API

### `pwm_driver_init`

```c
void pwm_driver_init(pwm_driver_t *drv, int gpio_pin,
                     ledc_channel_t channel, ledc_timer_t timer,
                     uint32_t freq_hz, uint8_t duty);
```

Initialises one PWM output. Must be called before any other function on the instance.  
`channel` and `timer` must be unique across all instances.

### `pwm_driver_set_freq`

```c
void pwm_driver_set_freq(pwm_driver_t *drv, uint32_t freq_hz);
```

Sets the output frequency in Hz. Thread-safe. 200 Hz hysteresis applied.

### `pwm_driver_set_duty`

```c
void pwm_driver_set_duty(pwm_driver_t *drv, uint8_t duty_8bit);
```

Sets the 8-bit duty cycle (0–255, where 255 = 100%). Thread-safe. Applied at the next PWM cycle boundary.

---

## Application (`main.cpp`)

Three `pwm_driver_t` instances are declared as a static array and initialised in `setup()`.  
All control runs in a single FreeRTOS task (`pwm_ctrl`) pinned to `APP_CPU_NUM` at priority 5, updating at 50 Hz (`CONTROL_INTERVAL_MS = 20`).

### Compile-time switches

| Define | Effect |
|--------|--------|
| `POT_CONTROL_DISABLED` **(defined)** | Fixed frequency (`FIXED_FREQ = 80000 Hz`) and duty (`FIXED_DUTY = 86/255`). ADC pins not configured. Serial output tagged `[fixed]`. |
| `POT_CONTROL_DISABLED` **(not defined)** | Frequency and duty read from potentiometers on GPIO 1 and GPIO 2. Frequency range: 20–80 kHz, duty range: 0–255. |

### Serial output (115200 baud via `Serial0` / UART0 / CH340)

```
Freq: 80000 Hz | Duty:  86/255 ( 33.7%) | V_out ~1.11 V | [fixed]
```

One line printed per second. The `[fixed]` tag is present when `POT_CONTROL_DISABLED` is defined; replaced by `ADC_f=<n> ADC_d=<n>` raw ADC readings otherwise.

---

## Build & flash

```bash
pio run --target upload
pio device monitor --port COM3 --baud 115200
```
