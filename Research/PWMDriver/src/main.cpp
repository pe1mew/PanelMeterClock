#include <Arduino.h>
#include "pwm_driver.h"

// Select exactly one mode (or neither for full pot control):
//   POT_CONTROL_DISABLED – fixed 80 kHz / fixed duty (FIXED_DUTY)
//   SWEEP_MODE           – fixed 80 kHz, duty sweeps 0→pot-max in 60 steps of 1 s each
// #define POT_CONTROL_DISABLED
#define SWEEP_MODE

const int   POT_FREQ_PIN        = 1;       // ADC1_CH0 - frequency control potentiometer
const int   POT_DUTY_PIN        = 2;       // ADC1_CH1 - duty cycle control potentiometer
const int   FREQ_MIN            = 20000;   // Hz
const int   FREQ_MAX            = 80000;   // Hz
const int   ADC_MIN             = 0;
const int   ADC_MAX             = 4095;    // 12-bit ADC
const int   ADC_RESOLUTION_BITS = 12;
const int   DUTY_MIN            = 0;
const int   DUTY_MAX            = 255;     // 8-bit duty cycle
const int   DUTY_INIT           = 128;     // 50% at startup
const int   FIXED_FREQ          = 80000;   // Hz - used when POT_CONTROL_DISABLED
const int   FIXED_DUTY          = 86;      // /255 - used when POT_CONTROL_DISABLED
const float SUPPLY_VOLTAGE_V    = 3.3f;
const int   SERIAL_BAUD_RATE    = 115200;
const int   PRINT_INTERVAL_MS   = 1000;
const int   CONTROL_INTERVAL_MS = 20;      // 50 Hz update rate
const int   SWEEP_STEPS         = 60;      // number of steps in SWEEP_MODE
const int   SWEEP_STEP_MS       = 1000;    // ms per step in SWEEP_MODE
const int   TASK_STACK_SIZE     = 4096;
const int   TASK_PRIORITY       = 5;
const int   NUM_PWM_CHANNELS    = 3;

// One independent driver instance per output channel.
// Each gets its own GPIO pin, LEDC channel, and LEDC timer.
static pwm_driver_t pwm[NUM_PWM_CHANNELS];

static const int            PWM_PINS[NUM_PWM_CHANNELS]     = { 15, 16, 17 };
static const ledc_channel_t PWM_CHANNELS[NUM_PWM_CHANNELS] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2 };
static const ledc_timer_t   PWM_TIMERS[NUM_PWM_CHANNELS]   = { LEDC_TIMER_0,   LEDC_TIMER_1,   LEDC_TIMER_2   };

static void control_task(void *arg)
{
    unsigned long lastPrint = 0;
#ifdef SWEEP_MODE
    int sweepStep = 0;
#endif

    for (;;) {
#if defined(POT_CONTROL_DISABLED)
        int freq = FIXED_FREQ;
        int duty = FIXED_DUTY;
#elif defined(SWEEP_MODE)
        int maxDuty = map(analogRead(POT_DUTY_PIN), ADC_MIN, ADC_MAX, DUTY_MIN, DUTY_MAX);
        int freq    = FIXED_FREQ;
        int duty    = map(sweepStep, 0, SWEEP_STEPS - 1, 0, maxDuty);
        sweepStep   = (sweepStep + 1) % SWEEP_STEPS;
#else
        int rawFreq = analogRead(POT_FREQ_PIN);
        int rawDuty = analogRead(POT_DUTY_PIN);

        int freq = map(rawFreq, ADC_MIN, ADC_MAX, FREQ_MIN, FREQ_MAX);
        int duty = map(rawDuty, ADC_MIN, ADC_MAX, DUTY_MIN, DUTY_MAX);
#endif

        for (int i = 0; i < NUM_PWM_CHANNELS; i++) {
            pwm_driver_set_freq(&pwm[i], (uint32_t)freq);
            pwm_driver_set_duty(&pwm[i], (uint8_t)duty);
        }

        unsigned long now = millis();
        if (now - lastPrint >= PRINT_INTERVAL_MS) {
            lastPrint = now;
            float dutyPct    = duty / (float)DUTY_MAX * 100.0f;
            float voltageOut = duty / (float)DUTY_MAX * SUPPLY_VOLTAGE_V;
#if defined(POT_CONTROL_DISABLED)
            Serial0.printf(
                "Freq: %5d Hz | Duty: %3d/255 (%5.1f%%) | V_out ~%.2f V"
                " | [fixed]\n",
                freq, duty, dutyPct, voltageOut);
#elif defined(SWEEP_MODE)
            Serial0.printf(
                "Freq: %5d Hz | Duty: %3d/255 (%5.1f%%) | V_out ~%.2f V"
                " | step=%2d/60 max=%3d\n",
                freq, duty, dutyPct, voltageOut, sweepStep, maxDuty);
#else
            Serial0.printf(
                "Freq: %5d Hz | Duty: %3d/255 (%5.1f%%) | V_out ~%.2f V"
                " | ADC_f=%4d ADC_d=%4d\n",
                freq, duty, dutyPct, voltageOut, rawFreq, rawDuty);
#endif
        }

#ifdef SWEEP_MODE
        vTaskDelay(pdMS_TO_TICKS(SWEEP_STEP_MS));
#else
        vTaskDelay(pdMS_TO_TICKS(CONTROL_INTERVAL_MS));
#endif
    }
}

void setup()
{
    Serial0.begin(SERIAL_BAUD_RATE);

#if defined(SWEEP_MODE)
    analogReadResolution(ADC_RESOLUTION_BITS);
    analogSetPinAttenuation(POT_DUTY_PIN, ADC_11db);
#elif !defined(POT_CONTROL_DISABLED)
    analogReadResolution(ADC_RESOLUTION_BITS);
    analogSetPinAttenuation(POT_FREQ_PIN, ADC_11db);
    analogSetPinAttenuation(POT_DUTY_PIN, ADC_11db);
#endif

    for (int i = 0; i < NUM_PWM_CHANNELS; i++) {
        pwm_driver_init(&pwm[i], PWM_PINS[i], PWM_CHANNELS[i], PWM_TIMERS[i],
                        FIXED_FREQ, FIXED_DUTY);
    }

    Serial0.println("PWMDriver ready.");
    Serial0.println("GPIO1=freq pot | GPIO2=duty pot");
    Serial0.println("GPIO15=PWM ch0 | GPIO16=PWM ch1 | GPIO17=PWM ch2");
    Serial0.println("RC filter: R=1k, C=10uF, fc~16 Hz");
    Serial0.println("-----------------------------------------------------------");

    xTaskCreatePinnedToCore(
        control_task, "pwm_ctrl",
        TASK_STACK_SIZE, nullptr, TASK_PRIORITY,
        nullptr, APP_CPU_NUM);
}

void loop()
{
    vTaskDelay(portMAX_DELAY);  // all work is done in control_task
}
