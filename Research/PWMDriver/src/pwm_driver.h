/**
 * @file pwm_driver.h
 * @brief Single-channel LEDC PWM driver for ESP32.
 *
 * Each pwm_driver_t instance independently controls one LEDC channel
 * and one LEDC timer. Instances share no state, giving full separation
 * of concern.
 */

#pragma once
#include <stdint.h>
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/** @brief Per-instance driver state. Allocate one per PWM output. */
typedef struct {
    ledc_channel_t    channel;     /**< LEDC channel assigned to this instance. */
    ledc_timer_t      timer;       /**< LEDC timer assigned to this instance. */
    SemaphoreHandle_t mutex;       /**< Per-instance mutex for thread safety. */
    uint32_t          active_freq; /**< Last committed frequency (Hz). */
} pwm_driver_t;

/**
 * @brief Initialise a single PWM output.
 *
 * Must be called once before using any other function on this instance.
 *
 * @param drv      Pointer to an uninitialized pwm_driver_t instance.
 * @param gpio_pin GPIO output pin.
 * @param channel  LEDC channel to use (must be unique per instance).
 * @param timer    LEDC timer to use (must be unique per instance).
 * @param freq_hz  Starting frequency in Hz (e.g. 80000).
 * @param duty     Starting 8-bit duty cycle (0–255).
 */
void pwm_driver_init(pwm_driver_t *drv, int gpio_pin,
                     ledc_channel_t channel, ledc_timer_t timer,
                     uint32_t freq_hz, uint8_t duty);

/**
 * @brief Set the frequency for this PWM instance.
 *
 * Thread-safe. Applies a hysteresis of 200 Hz to avoid spurious timer
 * resets when the requested frequency is close to the current value.
 *
 * @param drv     Pointer to an initialized pwm_driver_t instance.
 * @param freq_hz Desired frequency in Hz.
 */
void pwm_driver_set_freq(pwm_driver_t *drv, uint32_t freq_hz);

/**
 * @brief Set the duty cycle for this PWM instance.
 *
 * Thread-safe. The new duty cycle is applied at the next PWM cycle
 * boundary — no mid-cycle glitch.
 *
 * @param drv       Pointer to an initialized pwm_driver_t instance.
 * @param duty_8bit Duty cycle value (0–255, where 255 = 100%).
 */
void pwm_driver_set_duty(pwm_driver_t *drv, uint8_t duty_8bit);
