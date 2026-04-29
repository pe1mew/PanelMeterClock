#include <Arduino.h>
#include "driver/ledc.h"   // ledc_timer_config, ledc_channel_config — handles clock enable
#include "soc/ledc_reg.h"  // LEDC_LSCH0_DUTY_REG, LEDC_LSCH0_CONF1_REG for direct duty write

const int POT_FREQ_PIN = 1;     // ADC1_CH0 - frequency control potentiometer
const int POT_DUTY_PIN = 2;     // ADC1_CH1 - duty cycle control potentiometer
const int PWM_OUT_PIN  = 15;    // LEDC ch0 output -> RC low-pass filter -> meter
const int FREQ_MIN     = 20000; // 20 kHz
const int FREQ_MAX     = 80000; // 80 kHz

// Apply a new duty cycle at the next PWM cycle boundary — no mid-cycle glitch.
// DUTY_START (bit 31) is a self-clearing write trigger; hardware applies the
// new duty from LSCH0_DUTY_REG at the start of the next period.
static inline void ledc_write_duty(uint8_t duty_8bit)
{
    // 1. Write new duty to shadow register (Q4 format: 4 fractional bits)
    REG_WRITE(LEDC_LSCH0_DUTY_REG,  (uint32_t)duty_8bit << 4);
    // 2. Arm output update at next cycle boundary
    REG_WRITE(LEDC_LSCH0_CONF1_REG, LEDC_DUTY_START_LSCH0_M);
    // 3. PARA_UP: latch shadow → active registers (required for low-speed channels)
    REG_SET_BIT(LEDC_LSCH0_CONF0_REG, LEDC_PARA_UP_LSCH0_M);
}

// Reconfigure timer frequency via IDF API (handles clock source correctly).
// Resets the timer counter, so only call on significant frequency changes.
static void timer_set_freq(uint32_t freq_hz)
{
    const ledc_timer_config_t cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&cfg);
}

// ---- Main ---------------------------------------------------------------

int           activeFreq = 0;
unsigned long lastPrint  = 0;

void setup()
{
    Serial0.begin(115200);

    analogReadResolution(12);
    analogSetPinAttenuation(POT_FREQ_PIN, ADC_11db);
    analogSetPinAttenuation(POT_DUTY_PIN, ADC_11db);

    // IDF timer init: enables LEDC peripheral clock, sets divider and clock source
    timer_set_freq(FREQ_MIN);

    // IDF channel init: routes GPIO15 to LEDC ch0 via GPIO matrix, sets initial duty
    const ledc_channel_config_t ch = {
        .gpio_num   = PWM_OUT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 128,   // 50% startup so scope shows signal immediately
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    // Confirm LEDC is live: DUTY_START arms the 50% duty at next cycle edge
    ledc_write_duty(128);
    activeFreq = FREQ_MIN;

    Serial0.println("PWMTest ready.");
    Serial0.println("GPIO1=freq pot | GPIO2=duty pot | GPIO15=PWM");
    Serial0.println("RC filter: R=1k, C=10uF, fc~16 Hz");
    Serial0.println("-----------------------------------------------------------");
}

void loop()
{
    int rawFreq = analogRead(POT_FREQ_PIN);  // 0-4095
    int rawDuty = analogRead(POT_DUTY_PIN);

    int freq = map(rawFreq, 0, 4095, FREQ_MIN, FREQ_MAX);
    int duty = map(rawDuty, 0, 4095, 0, 255);

    // Reconfigure timer only on significant frequency change (causes timer reset)
    if (abs(freq - activeFreq) > 200) {
        timer_set_freq((uint32_t)freq);
        activeFreq = freq;
    }

    // Direct register duty write: applied at next cycle boundary, no mid-cycle glitch
    ledc_write_duty((uint8_t)duty);

    if (millis() - lastPrint >= 1000) {
        lastPrint = millis();
        float dutyPct    = duty / 255.0f * 100.0f;
        float voltageOut = duty / 255.0f * 3.3f;
        Serial0.printf(
            "Freq: %5d Hz | Duty: %3d/255 (%5.1f%%) | V_out ~%.2f V | ADC_f=%4d ADC_d=%4d\n",
            activeFreq, duty, dutyPct, voltageOut, rawFreq, rawDuty);
    }
}
