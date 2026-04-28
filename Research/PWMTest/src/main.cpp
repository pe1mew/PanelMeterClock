#include <Arduino.h>

const int POT_FREQ_PIN   = 1;      // ADC1_CH0 - frequency control potentiometer
const int POT_DUTY_PIN   = 2;      // ADC1_CH1 - duty cycle control potentiometer
const int PWM_OUT_PIN    = 15;     // LEDC PWM output -> RC low-pass filter -> meter
const int PWM_CHANNEL    = 0;
const int PWM_RESOLUTION = 8;      // 8-bit: duty range 0-255
const int FREQ_MIN       = 20000;  // 20 kHz - lower bound (above audible range)
const int FREQ_MAX       = 80000;  // 80 kHz - upper bound

int           activeFreq = 0;
unsigned long lastPrint  = 0;

void setup() {
    Serial.begin(115200);

    analogReadResolution(12);                              // 12-bit ADC: 0-4095
    analogSetPinAttenuation(POT_FREQ_PIN, ADC_11db);      // full 0-3.3 V range
    analogSetPinAttenuation(POT_DUTY_PIN, ADC_11db);

    ledcSetup(PWM_CHANNEL, FREQ_MIN, PWM_RESOLUTION);
    ledcAttachPin(PWM_OUT_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0);

    Serial.println("PWMTest ready.");
    Serial.println("GPIO1 = frequency pot | GPIO2 = duty cycle pot | GPIO15 = PWM out");
    Serial.println("RC filter: R=1k, C=10uF, fc~16Hz");
    Serial.println("-----------------------------------------------------------");
}

void loop() {
    int rawFreq = analogRead(POT_FREQ_PIN);   // 0-4095
    int rawDuty = analogRead(POT_DUTY_PIN);   // 0-4095

    int freq      = map(rawFreq, 0, 4095, FREQ_MIN, FREQ_MAX);
    int dutyCycle = map(rawDuty, 0, 4095, 0, 255);

    // Reconfigure LEDC timer only when frequency changes significantly to avoid glitches
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
