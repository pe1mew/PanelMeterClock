/**
 * @file dcf77.c
 * @brief DCF77 edge-timing decoder implementation (see dcf77.h).
 *
 * Data flow:
 *   GPIO ANYEDGE ISR ──(timestamped edges)──▶ FreeRTOS queue ──▶ decoder task
 *
 * The decoder task removes glitches (a pair of edges closer together than
 * cfg.glitch_ms annihilates), measures mark widths and mark-to-mark periods,
 * classifies bits, assembles the minute frame and validates it. All
 * callbacks run in the decoder task context.
 *
 * The GPIO ISR service is installed with flags 0; the per-pin handler is
 * therefore not required to be IRAM-resident. If the application installs
 * the service with ESP_INTR_FLAG_IRAM instead, the handler here is already
 * IRAM_ATTR but gpio_get_level() is not — replace it with a register read
 * in that case.
 */

#include "dcf77.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"

// ---------------------------------------------------------------------------
// Internal types and constants
// ---------------------------------------------------------------------------

/** One GPIO transition as captured by the ISR. */
typedef struct {
    int64_t t_us;  /**< esp_timer timestamp of the edge. */
    uint8_t level; /**< GPIO level after the edge. */
} dcf77_edge_t;

static const int      EDGE_QUEUE_LEN = 64; // absorbs noise bursts between task wake-ups
static const uint32_t TASK_POLL_MS   = 25; // paces pending-edge flush and the signal watchdog
static const uint8_t  BIT_UNREADABLE = 0xFF;

/** Decoder working state; lives on the decoder task stack. */
typedef struct {
    // glitch filter
    bool         have_pending;
    dcf77_edge_t pending;    // newest edge, accepted only if no annihilating edge follows
    uint8_t      last_level; // level after the last accepted edge; 0xFF = unknown
    // mark measurement
    int64_t mark_start_us;   // accepted leading edge of the current/last mark; <0 = none
    bool    in_mark;
    // frame assembly
    int16_t bit_index;       // next bit slot; -1 = not minute-aligned
    uint8_t bits[DCF77_FRAME_BITS + 1]; // +1 slot tolerates a leap-second minute
    bool    frame_dirty;     // an unreadable pulse occurred this minute
    // frame confirmation (FR-DCF-007)
    int64_t prev_frame_epoch; // utc_epoch of the previous valid frame; <0 = none
    // signal watchdog
    int64_t last_edge_us;
    bool    no_signal_reported;
} decoder_t;

#define STATUS_LOCK(dcf)   portENTER_CRITICAL(&(dcf)->status_mux)
#define STATUS_UNLOCK(dcf) portEXIT_CRITICAL(&(dcf)->status_mux)

static void emit(dcf77_t *dcf, const dcf77_event_t *evt)
{
    if (dcf->cfg.event_cb) {
        dcf->cfg.event_cb(evt, dcf->cfg.cb_ctx);
    }
}

static void set_state(dcf77_t *dcf, dcf77_state_t state)
{
    STATUS_LOCK(dcf);
    dcf->status.state = state;
    STATUS_UNLOCK(dcf);
}

// ---------------------------------------------------------------------------
// Calendar helpers
// ---------------------------------------------------------------------------

/** Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/** ISO weekday (1 = Monday … 7 = Sunday) for days since 1970-01-01. */
static uint8_t civil_weekday(int64_t days)
{
    return (uint8_t)((days + 3) % 7 + 1); // 1970-01-01 was a Thursday (4)
}

static uint8_t days_in_month(uint16_t y, uint8_t m)
{
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) {
        return 29;
    }
    return dim[m - 1];
}

// ---------------------------------------------------------------------------
// Frame decoding (bit layout per PTB / blinkenlight decoder)
// ---------------------------------------------------------------------------

/** Decode an LSB-first BCD field; returns 0xFF when a digit is no valid BCD. */
static uint8_t bcd_field(const uint8_t *bits, uint8_t start, uint8_t len)
{
    static const uint8_t weight[8] = {1, 2, 4, 8, 10, 20, 40, 80};
    uint8_t lo = 0;
    uint8_t hi = 0;
    for (uint8_t i = 0; i < len; i++) {
        if (bits[start + i]) {
            if (i < 4) {
                lo += weight[i];
            } else {
                hi += weight[i];
            }
        }
    }
    if (lo > 9 || hi > 90) {
        return 0xFF;
    }
    return lo + hi;
}

/** true when the number of 1-bits in [start..end_incl] is even. */
static bool parity_even(const uint8_t *bits, uint8_t start, uint8_t end_incl)
{
    uint8_t ones = 0;
    for (uint8_t i = start; i <= end_incl; i++) {
        ones += bits[i];
    }
    return (ones & 1) == 0;
}

/**
 * Validate a complete 59-bit frame and decode it (FR-DCF-005/006/009).
 * On failure *reason names the first failed check.
 */
static bool frame_decode(const uint8_t *bits, dcf77_time_t *t, const char **reason)
{
    if (bits[0] != 0) {
        *reason = "start bit (bit 0) is not 0";
        return false;
    }
    if (bits[20] != 1) {
        *reason = "time start bit (bit 20) is not 1";
        return false;
    }
    if (bits[17] == bits[18]) {
        *reason = "zone bits Z1/Z2 are equal";
        return false;
    }
    if (!parity_even(bits, 21, 28)) {
        *reason = "minute parity (P1) failed";
        return false;
    }
    if (!parity_even(bits, 29, 35)) {
        *reason = "hour parity (P2) failed";
        return false;
    }
    if (!parity_even(bits, 36, 58)) {
        *reason = "date parity (P3) failed";
        return false;
    }

    const uint8_t minute  = bcd_field(bits, 21, 7);
    const uint8_t hour    = bcd_field(bits, 29, 6);
    const uint8_t day     = bcd_field(bits, 36, 6);
    const uint8_t weekday = bcd_field(bits, 42, 3);
    const uint8_t month   = bcd_field(bits, 45, 5);
    const uint8_t year2   = bcd_field(bits, 50, 8);

    if (minute > 59) {
        *reason = "minute out of range";
        return false;
    }
    if (hour > 23) {
        *reason = "hour out of range";
        return false;
    }
    if (month < 1 || month > 12) {
        *reason = "month out of range";
        return false;
    }
    if (year2 > 99) {
        *reason = "year out of range";
        return false;
    }
    if (day < 1 || day > days_in_month(2000 + year2, month)) {
        *reason = "day out of range";
        return false;
    }
    if (weekday < 1 || weekday > 7) {
        *reason = "weekday out of range";
        return false;
    }

    const int64_t days = days_from_civil(2000 + year2, month, day);
    if (civil_weekday(days) != weekday) {
        *reason = "weekday does not match date";
        return false;
    }

    t->year    = 2000 + year2;
    t->month   = month;
    t->day     = day;
    t->weekday = weekday;
    t->hour    = hour;
    t->minute  = minute;
    t->cest    = bits[17] != 0;
    t->dst_change_announced  = bits[16] != 0;
    t->leap_second_announced = bits[19] != 0;
    t->call_bit              = bits[15] != 0;
    // The broadcast time is civil CET/CEST; CET = UTC+1, CEST = UTC+2 (FR-DCF-009).
    const int64_t civil_epoch = days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60;
    t->utc_epoch = civil_epoch - (t->cest ? 2 : 1) * 3600;
    return true;
}

// ---------------------------------------------------------------------------
// Edge interrupt
// ---------------------------------------------------------------------------

static void IRAM_ATTR dcf77_isr(void *arg)
{
    dcf77_t *dcf = (dcf77_t *)arg;
    dcf77_edge_t e;
    e.t_us  = esp_timer_get_time();
    e.level = (uint8_t)gpio_get_level((gpio_num_t)dcf->cfg.signal_gpio);
    BaseType_t woken = pdFALSE;
    // On queue overflow the edge is dropped; the decoder resyncs via the
    // same-level check in accept_edge().
    xQueueSendFromISR(dcf->edge_queue, &e, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

/** Drop bit alignment; report it only when alignment actually existed. */
static void resync(dcf77_t *dcf, decoder_t *d, uint16_t measured_ms, const char *reason)
{
    const bool was_aligned = d->bit_index >= 0;
    d->bit_index        = -1;
    d->frame_dirty      = false;
    d->prev_frame_epoch = -1;

    STATUS_LOCK(dcf);
    dcf->status.second = -1;
    dcf->status.state  = DCF77_STATE_SYNCING;
    STATUS_UNLOCK(dcf);

    if (was_aligned) {
        dcf77_event_t evt = {0};
        evt.type        = DCF77_EVT_RESYNC;
        evt.t_us        = esp_timer_get_time();
        evt.measured_ms = measured_ms;
        evt.reason      = reason;
        emit(dcf, &evt);
    }
}

/** A complete minute ended on this edge: validate, decode, confirm. */
static void frame_finalize(dcf77_t *dcf, decoder_t *d, int64_t t_us, uint16_t period_ms)
{
    dcf77_event_t evt = {0};
    evt.type        = DCF77_EVT_MINUTE_MARK;
    evt.t_us        = t_us;
    evt.measured_ms = period_ms;
    emit(dcf, &evt);

    if (d->bit_index >= 0) {
        memset(&evt, 0, sizeof(evt));
        evt.t_us      = t_us;
        evt.bit_count = (uint8_t)d->bit_index;
        evt.bits      = d->bits;

        const char  *reason = NULL;
        dcf77_time_t t      = {0};
        bool         ok     = false;
        if (d->bit_index != DCF77_FRAME_BITS) {
            reason = "frame is not 59 bits";
        } else if (d->frame_dirty) {
            reason = "frame contains unreadable pulses";
        } else {
            ok = frame_decode(d->bits, &t, &reason);
        }

        if (ok) {
            evt.type = DCF77_EVT_FRAME_OK;
            evt.time = t;
            STATUS_LOCK(dcf);
            dcf->status.frames_ok++;
            STATUS_UNLOCK(dcf);
            emit(dcf, &evt);

            // FR-DCF-007: trust the time only after two consecutive
            // consistent frames (exactly one minute apart).
            if (d->prev_frame_epoch >= 0 && t.utc_epoch == d->prev_frame_epoch + 60) {
                STATUS_LOCK(dcf);
                dcf->status.confirmations++;
                dcf->status.time_valid = true;
                dcf->status.last_time  = t;
                dcf->status.state      = DCF77_STATE_LOCKED;
                STATUS_UNLOCK(dcf);
                evt.type = DCF77_EVT_TIME_CONFIRMED;
                emit(dcf, &evt);
            }
            d->prev_frame_epoch = t.utc_epoch;
        } else {
            d->prev_frame_epoch = -1; // consecutive chain broken
            evt.type   = DCF77_EVT_FRAME_REJECT;
            evt.reason = reason;
            STATUS_LOCK(dcf);
            dcf->status.frames_rejected++;
            if (dcf->status.state == DCF77_STATE_LOCKED) {
                dcf->status.state = DCF77_STATE_COLLECTING;
            }
            STATUS_UNLOCK(dcf);
            emit(dcf, &evt);
        }
    }

    // This edge is the leading edge of second 0 of the new minute.
    d->bit_index   = 0;
    d->frame_dirty = false;
    STATUS_LOCK(dcf);
    if (dcf->status.state != DCF77_STATE_LOCKED) {
        dcf->status.state = DCF77_STATE_COLLECTING;
    }
    STATUS_UNLOCK(dcf);
}

/** Accepted leading edge of a mark: classify the period, fire the tick. */
static void mark_start(dcf77_t *dcf, decoder_t *d, int64_t t_us)
{
    const dcf77_config_t *cfg = &dcf->cfg;

    if (d->mark_start_us >= 0) {
        const int64_t period_ms = (t_us - d->mark_start_us) / 1000;
        if (period_ms >= cfg->period_min_ms && period_ms <= cfg->period_max_ms) {
            // normal second
        } else if (period_ms >= cfg->minute_min_ms && period_ms <= cfg->minute_max_ms) {
            frame_finalize(dcf, d, t_us, (uint16_t)period_ms);
        } else {
            resync(dcf, d, (uint16_t)(period_ms > UINT16_MAX ? UINT16_MAX : period_ms),
                   "mark period outside second/minute windows");
        }
    } else if (d->bit_index < 0) {
        set_state(dcf, DCF77_STATE_SYNCING);
    }

    d->mark_start_us = t_us;
    d->in_mark       = true;

    STATUS_LOCK(dcf);
    dcf->status.second = d->bit_index;
    STATUS_UNLOCK(dcf);

    // FR-DCF-012: every mark's leading edge is an on-time second boundary.
    if (cfg->tick_cb) {
        cfg->tick_cb(t_us, d->bit_index, cfg->cb_ctx);
    }
}

/** Accepted trailing edge of a mark: classify the width, commit the bit. */
static void mark_end(dcf77_t *dcf, decoder_t *d, int64_t t_us)
{
    const dcf77_config_t *cfg = &dcf->cfg;
    d->in_mark = false;

    const int64_t width_ms = (t_us - d->mark_start_us) / 1000;
    uint8_t value = BIT_UNREADABLE;
    if (width_ms >= cfg->zero_min_ms && width_ms <= cfg->zero_max_ms) {
        value = 0;
    } else if (width_ms >= cfg->one_min_ms && width_ms <= cfg->one_max_ms) {
        value = 1;
    }

    dcf77_event_t evt = {0};
    evt.t_us        = t_us;
    evt.measured_ms = (uint16_t)(width_ms > UINT16_MAX ? UINT16_MAX : width_ms);
    evt.bit_index   = d->bit_index;
    evt.bit_value   = (int8_t)value;

    if (d->bit_index >= 0) {
        if (d->bit_index > DCF77_FRAME_BITS) {
            // more marks than even a leap-second minute allows
            resync(dcf, d, evt.measured_ms, "no minute mark for over a minute");
            return;
        }
        d->bits[d->bit_index] = value;
        if (value == BIT_UNREADABLE) {
            d->frame_dirty = true;
        }
        d->bit_index++;
    }

    STATUS_LOCK(dcf);
    if (value == BIT_UNREADABLE) {
        dcf->status.pulses_rejected++;
    } else {
        dcf->status.bits_ok++;
    }
    STATUS_UNLOCK(dcf);

    evt.type = (value == BIT_UNREADABLE) ? DCF77_EVT_PULSE_REJECT : DCF77_EVT_BIT;
    emit(dcf, &evt);
}

/** Process one glitch-filtered edge. */
static void accept_edge(dcf77_t *dcf, decoder_t *d, const dcf77_edge_t *e)
{
    if (e->level == d->last_level) {
        // an edge was lost (queue overflow) — timing can no longer be trusted
        d->last_level    = e->level;
        d->in_mark       = false;
        d->mark_start_us = -1;
        resync(dcf, d, 0, "missed an edge");
        return;
    }
    d->last_level = e->level;

    const uint8_t mark_level = dcf->cfg.signal_inverted ? 0 : 1;
    if (e->level == mark_level) {
        mark_start(dcf, d, e->t_us);
    } else if (d->in_mark) {
        mark_end(dcf, d, e->t_us);
    }
}

static void dcf77_task_fn(void *arg)
{
    dcf77_t  *dcf = (dcf77_t *)arg;
    decoder_t d;
    memset(&d, 0, sizeof(d));
    d.last_level       = 0xFF;
    d.mark_start_us    = -1;
    d.bit_index        = -1;
    d.prev_frame_epoch = -1;
    d.last_edge_us     = esp_timer_get_time();

    const int64_t glitch_us = (int64_t)dcf->cfg.glitch_ms * 1000;

    while (dcf->running) {
        dcf77_edge_t e;
        if (xQueueReceive(dcf->edge_queue, &e, pdMS_TO_TICKS(TASK_POLL_MS)) == pdTRUE) {
            STATUS_LOCK(dcf);
            dcf->status.edges++;
            STATUS_UNLOCK(dcf);
            d.last_edge_us = e.t_us;
            if (d.no_signal_reported) {
                d.no_signal_reported = false;
                set_state(dcf, DCF77_STATE_SYNCING);
            }

            if (d.have_pending) {
                if (e.t_us - d.pending.t_us < glitch_us) {
                    // a spike: the pending edge and this edge annihilate
                    d.have_pending = false;
                    STATUS_LOCK(dcf);
                    dcf->status.glitches++;
                    STATUS_UNLOCK(dcf);
                    continue;
                }
                accept_edge(dcf, &d, &d.pending);
            }
            d.pending      = e;
            d.have_pending = true;
        } else {
            const int64_t now = esp_timer_get_time();
            // no annihilating edge followed within the glitch window
            if (d.have_pending && now - d.pending.t_us >= glitch_us) {
                accept_edge(dcf, &d, &d.pending);
                d.have_pending = false;
            }
            if (!d.no_signal_reported &&
                now - d.last_edge_us >= (int64_t)dcf->cfg.signal_lost_ms * 1000) {
                d.no_signal_reported = true;
                d.bit_index          = -1;
                d.in_mark            = false;
                d.mark_start_us      = -1;
                d.frame_dirty        = false;
                d.prev_frame_epoch   = -1;
                STATUS_LOCK(dcf);
                dcf->status.second = -1;
                dcf->status.state  = DCF77_STATE_NO_SIGNAL;
                STATUS_UNLOCK(dcf);
                dcf77_event_t evt = {0};
                evt.type = DCF77_EVT_NO_SIGNAL;
                evt.t_us = now;
                emit(dcf, &evt);
            }
        }
    }

    dcf->task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

dcf77_config_t dcf77_config_default(void)
{
    dcf77_config_t c = {0};
    c.signal_gpio     = 11;    // IC-HW-005
    c.pon_gpio        = 12;    // IC-HW-006
    c.pon_on_level    = 0;     // module PDN/P1: low = receiver on
    c.signal_inverted = false; // mark (carrier reduction) = HIGH
    c.internal_pullup = true;  // open-collector time-code output

    c.glitch_ms     = 20;
    c.zero_min_ms   = 40;      // nominal 100 ms
    c.zero_max_ms   = 130;
    c.one_min_ms    = 140;     // nominal 200 ms
    c.one_max_ms    = 260;
    c.period_min_ms = 800;     // nominal 1000 ms
    c.period_max_ms = 1200;
    c.minute_min_ms = 1700;    // nominal 2000 ms (missing 59th mark)
    c.minute_max_ms = 2300;
    c.signal_lost_ms = 5000;   // PoC value; firmware uses DCF77_SIGNAL_LOST_TIMEOUT_S

    c.task_stack    = 4096;
    c.task_priority = 7;       // PMC-STD-001 §4
    c.task_core     = 0;       // PRO core, PMC-STD-001 §4
    return c;
}

bool dcf77_init(dcf77_t *dcf, const dcf77_config_t *cfg)
{
    if (dcf == NULL || cfg == NULL) {
        return false;
    }
    memset(dcf, 0, sizeof(*dcf));
    dcf->cfg = *cfg;
    const portMUX_TYPE mux_init = portMUX_INITIALIZER_UNLOCKED;
    dcf->status_mux    = mux_init;
    dcf->status.state  = DCF77_STATE_OFF;
    dcf->status.second = -1;
    dcf->edge_queue    = xQueueCreate(EDGE_QUEUE_LEN, sizeof(dcf77_edge_t));
    return dcf->edge_queue != NULL;
}

bool dcf77_start(dcf77_t *dcf)
{
    if (dcf == NULL || dcf->edge_queue == NULL || dcf->task != NULL) {
        return false;
    }

    const gpio_num_t sig_pin = (gpio_num_t)dcf->cfg.signal_gpio;
    const gpio_config_t sig_cfg = {
        .pin_bit_mask = 1ULL << sig_pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = dcf->cfg.internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    if (gpio_config(&sig_cfg) != ESP_OK) {
        return false;
    }

    if (dcf->cfg.pon_gpio >= 0) {
        const gpio_config_t pon_cfg = {
            .pin_bit_mask = 1ULL << dcf->cfg.pon_gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&pon_cfg) != ESP_OK) {
            return false;
        }
        // pulse off→on so the receiver AGC restarts from a defined state
        gpio_set_level((gpio_num_t)dcf->cfg.pon_gpio, dcf->cfg.pon_on_level ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level((gpio_num_t)dcf->cfg.pon_gpio, dcf->cfg.pon_on_level ? 1 : 0);
    }

    xQueueReset(dcf->edge_queue);
    STATUS_LOCK(dcf);
    memset(&dcf->status, 0, sizeof(dcf->status));
    dcf->status.state  = DCF77_STATE_NO_SIGNAL;
    dcf->status.second = -1;
    STATUS_UNLOCK(dcf);

    dcf->running = true;
    TaskHandle_t th = NULL;
    if (xTaskCreatePinnedToCore(dcf77_task_fn, "dcf77", dcf->cfg.task_stack, dcf,
                                dcf->cfg.task_priority, &th, dcf->cfg.task_core) != pdPASS) {
        dcf->running = false;
        return false;
    }
    dcf->task = th;

    // The shared per-pin ISR service may already be installed by the application.
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        dcf77_stop(dcf);
        return false;
    }
    if (gpio_isr_handler_add(sig_pin, dcf77_isr, dcf) != ESP_OK) {
        dcf77_stop(dcf);
        return false;
    }
    return true;
}

void dcf77_stop(dcf77_t *dcf)
{
    if (dcf == NULL || !dcf->running) {
        return;
    }
    const gpio_num_t sig_pin = (gpio_num_t)dcf->cfg.signal_gpio;
    gpio_intr_disable(sig_pin);
    gpio_isr_handler_remove(sig_pin);
    dcf->running = false;
    while (dcf->task != NULL) { // task self-deletes within one poll interval
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (dcf->cfg.pon_gpio >= 0) {
        // hold the receiver off while disabled (FR-DCF-002)
        gpio_set_level((gpio_num_t)dcf->cfg.pon_gpio, dcf->cfg.pon_on_level ? 0 : 1);
    }
    set_state(dcf, DCF77_STATE_OFF);
}

void dcf77_get_status(dcf77_t *dcf, dcf77_status_t *out)
{
    if (dcf == NULL || out == NULL) {
        return;
    }
    STATUS_LOCK(dcf);
    *out = dcf->status;
    STATUS_UNLOCK(dcf);
}
