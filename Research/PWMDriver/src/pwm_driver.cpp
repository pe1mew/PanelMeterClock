#include "pwm_driver.h"
#include "soc/ledc_reg.h"  // LEDC_LSCH0_* register addresses and bit masks

// ESP32-S3 LEDC low-speed channel registers have a uniform stride of 0x14
// between consecutive channels.  Channel N's registers sit at:
//   LEDC_LSCH0_<REG> + N * 0x14
// The DUTY_START (bit 31) and PARA_UP (bit 26) bit positions are identical
// across all low-speed channel CONF1/CONF0 registers.
static void ledc_write_duty(ledc_channel_t ch, uint8_t duty_8bit)
{
    uint32_t duty_reg  = LEDC_LSCH0_DUTY_REG  + ch * 0x14u;
    uint32_t conf1_reg = LEDC_LSCH0_CONF1_REG + ch * 0x14u;
    uint32_t conf0_reg = LEDC_LSCH0_CONF0_REG + ch * 0x14u;

    // 1. Write new duty to shadow register (Q4 format: 4 fractional bits)
    REG_WRITE(duty_reg,  (uint32_t)duty_8bit << 4);
    // 2. Arm output update at next cycle boundary (self-clearing bit)
    REG_WRITE(conf1_reg, LEDC_DUTY_START_LSCH0_M);
    // 3. PARA_UP: latch shadow → active registers (required for low-speed channels)
    REG_SET_BIT(conf0_reg, LEDC_PARA_UP_LSCH0_M);
}

static void timer_set_freq(ledc_timer_t timer, uint32_t freq_hz)
{
    const ledc_timer_config_t cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = timer,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&cfg);
}

void pwm_driver_init(pwm_driver_t *drv, int gpio_pin,
                     ledc_channel_t channel, ledc_timer_t timer,
                     uint32_t freq_hz, uint8_t duty)
{
    drv->channel     = channel;
    drv->timer       = timer;
    drv->mutex       = xSemaphoreCreateMutex();
    drv->active_freq = freq_hz;

    timer_set_freq(timer, freq_hz);

    const ledc_channel_config_t cfg = {
        .gpio_num   = gpio_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = channel,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = timer,
        .duty       = duty,
        .hpoint     = 0,
    };
    ledc_channel_config(&cfg);
    ledc_write_duty(channel, duty);
}

void pwm_driver_set_freq(pwm_driver_t *drv, uint32_t freq_hz)
{
    xSemaphoreTake(drv->mutex, portMAX_DELAY);
    if ((uint32_t)abs((int32_t)freq_hz - (int32_t)drv->active_freq) > 200u) {
        timer_set_freq(drv->timer, freq_hz);
        drv->active_freq = freq_hz;
    }
    xSemaphoreGive(drv->mutex);
}

void pwm_driver_set_duty(pwm_driver_t *drv, uint8_t duty_8bit)
{
    xSemaphoreTake(drv->mutex, portMAX_DELAY);
    ledc_write_duty(drv->channel, duty_8bit);
    xSemaphoreGive(drv->mutex);
}
