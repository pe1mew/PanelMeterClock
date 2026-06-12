/**
 * @file main.c
 * @brief DCF77 decoder proof of concept (PanelMeterClock Research/DCFTest).
 *
 * Two interchangeable decoder components are available:
 *
 *   USE_PHASE_DECODER = 0 — components/dcf77: simple edge-timing decoder
 *       (PMC-STD-001 §5.10 baseline, permissively licensed).
 *   USE_PHASE_DECODER = 1 — components/dcf77p: phase-locked exhaustive
 *       decoder, C port of Udo Klein's blinkenlight decoder. NOTE:
 *       GPL-3.0 — linking it makes the firmware GPL.
 *
 * Either way the harness prints decoding progress over the default ESP-IDF
 * console (UART0 → CH340 USB port) and, once the time is trusted, a running
 * clock line every second.
 *
 * Pinning per PMC-HTD-001 §3: time-code GPIO 11, PON GPIO 12, DCF status
 * LED GPIO 5 (blinking = acquiring, steady = valid, per PMC-GUI-001).
 */

#define USE_PHASE_DECODER 1

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#if USE_PHASE_DECODER
#include "dcf77p.h"
#else
#include "dcf77.h"
#endif

#define LED_DCF_GPIO       GPIO_NUM_5 /* front-panel DCF status LED (PMC-HTD-001 §3) */
#define LED_INTERVAL_MS    250        /* loop pace; LED toggles every 2nd pass (~1 Hz blink) */
#define STATUS_INTERVAL_MS 10000      /* periodic status summary */

static SemaphoreHandle_t log_mutex; // groups multi-line prints from decoder and main task

/** printf to the console; safe to call from multiple tasks. */
static void log_print(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    fputs(buf, stdout);
    xSemaphoreGive(log_mutex);
}

// ---------------------------------------------------------------------------
// Shared presentation helpers
// ---------------------------------------------------------------------------

typedef struct {
    int y, m, d, hh, mm, ss;
} civil_t;

/** Civil date/time from an epoch (Howard Hinnant's civil_from_days). */
static civil_t civil_from_epoch(int64_t epoch)
{
    civil_t c;
    int64_t days = epoch / 86400;
    int64_t rem  = epoch % 86400;
    c.hh = (int)(rem / 3600);
    c.mm = (int)((rem / 60) % 60);
    c.ss = (int)(rem % 60);

    days += 719468;
    const int64_t  era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp  = (5 * doy + 2) / 153;
    c.d = (int)(doy - (153 * mp + 2) / 5 + 1);
    c.m = (int)(mp < 10 ? mp + 3 : mp - 9);
    c.y = (int)(yoe + era * 400 + (c.m <= 2));
    return c;
}

static const char *weekday_name(uint8_t wd)
{
    static const char *const names[] = {"?", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    return names[wd <= 7 ? wd : 0];
}

#if USE_PHASE_DECODER
// ===========================================================================
// Harness for the phase-locked exhaustive decoder (components/dcf77p)
// ===========================================================================

static dcf77p_t dcfp;

// One classification character per second of the current minute:
// 'S' sync mark, '0'/'1' data bits, '?' undefined, '.' not (yet) received.
static char tick_line[62];
static bool tick_line_dirty = false;
static int  unsynced_pos    = 0;

static void tick_line_reset(void)
{
    memset(tick_line, '.', 61);
    tick_line[61]   = '\0';
    tick_line_dirty = false;
    unsynced_pos    = 0;
}

static const char *state_name(dcf77p_state_t s)
{
    static const char *const names[] =
        {"OFF", "ACQUIRING_PHASE", "ACQUIRING_SECOND", "DECODING", "TIME_VALID"};
    return names[s];
}

/** "--" or two-digit value, for partially decoded fields. */
static const char *fmt2(uint8_t v, char buf[4])
{
    if (v == 0xFF) {
        return "--";
    }
    snprintf(buf, 4, "%02u", v);
    return buf;
}

static void on_dcfp_event(const dcf77p_event_t *evt, void *ctx)
{
    (void)ctx;
    static const char tick_char[4] = {'S', '?', '0', '1'};

    switch (evt->type) {
    case DCF77P_EVT_TICK:
        if (evt->tick_second <= 60) {
            tick_line[evt->tick_second] = tick_char[evt->tick];
            tick_line_dirty             = true;
        } else {
            // no second lock yet: show the raw classification stream
            tick_line[unsynced_pos++] = tick_char[evt->tick];
            tick_line_dirty           = true;
            if (unsynced_pos >= 60) {
                tick_line[unsynced_pos] = '\0';
                log_print("[tick] %s  (no second lock)\n", tick_line);
                tick_line_reset();
            }
        }
        break;

    case DCF77P_EVT_SECOND: {
        const dcf77p_time_t *t = &evt->time;

        // flush the per-minute classification line at the minute boundary
        if (t->second == 0 && tick_line_dirty && unsynced_pos == 0) {
            log_print("[min ] %s\n", tick_line);
            tick_line_reset();
        }

        if (t->valid) {
            const civil_t u = civil_from_epoch(t->utc_epoch);
            log_print("[time] %04u-%02u-%02u %s %02u:%02u:%02u %s (UTC %02d:%02d:%02d)%s%s\n",
                      2000 + t->year2, t->month, t->day, weekday_name(t->weekday),
                      t->hour, t->minute, t->second, t->cest ? "CEST" : "CET",
                      u.hh, u.mm, u.ss,
                      t->dst_change_announced ? " [DST change announced]" : "",
                      t->leap_second_announced ? " [leap second announced]" : "");
        } else if (t->second <= 60) {
            // second lock exists; calendar fields still converging
            char b1[4], b2[4], b3[4], b4[4], b5[4];
            log_print("[dec ] %s:%s:%02u  date 20%s-%s-%s  (fields locking)\n",
                      fmt2(t->hour, b1), fmt2(t->minute, b2), t->second,
                      fmt2(t->year2, b3), fmt2(t->month, b4), fmt2(t->day, b5));
        }
        break;
    }

    case DCF77P_EVT_STATE_CHANGE:
        log_print("==== state: %s -> %s ====\n",
                  state_name(evt->old_state), state_name(evt->new_state));
        if (evt->new_state == DCF77P_STATE_TIME_VALID) {
            log_print("[time] full date/time determined by exhaustive decoding\n");
        }
        break;
    }
}

static void decoder_setup(void)
{
    log_print("Decoder: dcf77p — phase-locked exhaustive (Udo Klein port, GPL-3.0)\n");
    log_print("1 kHz sampling, 100 phase bins, per-field maximum-likelihood binning\n");
    log_print("------------------------------------------------------------------\n");

    tick_line_reset();

    dcf77p_config_t cfg = dcf77p_config_default();
    cfg.event_cb = on_dcfp_event;

    if (!dcf77p_init(&dcfp, &cfg) || !dcf77p_start(&dcfp)) {
        log_print("[dcf ] FATAL: decoder init/start failed\n");
        return;
    }
    log_print("[dcf ] receiver powered, sampling — phase lock in ~1 min, full time in 3-10 min\n");
}

static void decoder_status_line(void)
{
    dcf77p_status_t st;
    dcf77p_get_status(&dcfp, &st);
    log_print("[stat] %s | sec %lu | ticks S:%lu ?:%lu 0:%lu 1:%lu | phase %lu-%lu | "
              "sync %u-%u | min %u-%u hr %u-%u day %u-%u wd %u-%u mon %u-%u yr %u-%u | drop %lu\n",
              state_name(st.state), (unsigned long)st.seconds,
              (unsigned long)st.ticks[DCF77P_TICK_SYNC],
              (unsigned long)st.ticks[DCF77P_TICK_UNDEFINED],
              (unsigned long)st.ticks[DCF77P_TICK_ZERO],
              (unsigned long)st.ticks[DCF77P_TICK_ONE],
              (unsigned long)st.quality.phase_lock, (unsigned long)st.quality.phase_noise,
              st.quality.second.lock, st.quality.second.noise,
              st.quality.minute.lock, st.quality.minute.noise,
              st.quality.hour.lock, st.quality.hour.noise,
              st.quality.day.lock, st.quality.day.noise,
              st.quality.weekday.lock, st.quality.weekday.noise,
              st.quality.month.lock, st.quality.month.noise,
              st.quality.year.lock, st.quality.year.noise,
              (unsigned long)st.slots_dropped);
}

static bool decoder_led_steady(void)
{
    dcf77p_status_t st;
    dcf77p_get_status(&dcfp, &st);
    return st.state == DCF77P_STATE_TIME_VALID;
}

static bool decoder_led_off(void)
{
    dcf77p_status_t st;
    dcf77p_get_status(&dcfp, &st);
    return st.state == DCF77P_STATE_OFF;
}

#else  /* !USE_PHASE_DECODER */
// ===========================================================================
// Harness for the simple edge-timing decoder (components/dcf77)
// ===========================================================================

static dcf77_t dcf;

// Running clock, maintained from decoder events (decoder task context only).
static int64_t utc_epoch  = -1;    // current UTC second; -1 = no confirmed time yet
static bool    fresh_sync = false; // epoch was (re)set at the current minute mark
static bool    show_cest  = false; // zone of the last valid frame, for display

// Inverted-polarity detection: with the wrong polarity setting the decoder
// "marks" are the ~800/900 ms idle phases.
static uint32_t long_mark_count    = 0;
static bool     polarity_hint_done = false;

/** Field annotation for the per-bit progress lines. */
static const char *bit_label(int16_t idx)
{
    if (idx == 0)  return "M  start of minute (always 0)";
    if (idx <= 14) return "civil warning / weather";
    if (idx == 15) return "R  call bit";
    if (idx == 16) return "A1 DST change announcement";
    if (idx == 17) return "Z1 zone bit (CEST)";
    if (idx == 18) return "Z2 zone bit (CET)";
    if (idx == 19) return "A2 leap second announcement";
    if (idx == 20) return "S  start of time (always 1)";
    if (idx <= 27) return "minute (BCD)";
    if (idx == 28) return "P1 minute parity";
    if (idx <= 34) return "hour (BCD)";
    if (idx == 35) return "P2 hour parity";
    if (idx <= 41) return "day of month (BCD)";
    if (idx <= 44) return "day of week";
    if (idx <= 49) return "month (BCD)";
    if (idx <= 57) return "year (BCD)";
    if (idx == 58) return "P3 date parity";
    return "?";
}

/** Render frame bits grouped by field: M | weather | R A1 Z1 Z2 A2 | S | ... */
static void format_frame_bits(const uint8_t *bits, uint8_t count, char *out, size_t cap)
{
    static const uint8_t group_after[] = {0, 14, 15, 16, 17, 18, 19, 20, 28, 35, 41, 44, 49};
    size_t pos = 0;
    for (uint8_t i = 0; i < count && pos + 2 < cap; i++) {
        out[pos++] = (bits[i] == 0) ? '0' : (bits[i] == 1) ? '1' : 'x';
        for (size_t g = 0; g < sizeof(group_after); g++) {
            if (i == group_after[g] && pos + 1 < cap) {
                out[pos++] = ' ';
                break;
            }
        }
    }
    out[pos] = '\0';
}

static void on_dcf_event(const dcf77_event_t *evt, void *ctx)
{
    (void)ctx;
    dcf77_status_t st;
    dcf77_get_status(&dcf, &st);

    switch (evt->type) {
    case DCF77_EVT_NO_SIGNAL:
        log_print("[dcf ] no edges seen — check receiver power, PON level, polarity and antenna siting\n");
        break;

    case DCF77_EVT_BIT:
        if (st.state == DCF77_STATE_LOCKED) {
            break; // suppress per-bit lines once locked
        }
        if (evt->bit_index >= 0) {
            log_print("[bit ] %2d = %d  (%3u ms)  %s\n",
                      evt->bit_index, evt->bit_value, evt->measured_ms, bit_label(evt->bit_index));
        } else {
            log_print("[bit ]  ? = %d  (%3u ms)  waiting for minute mark\n",
                      evt->bit_value, evt->measured_ms);
        }
        break;

    case DCF77_EVT_PULSE_REJECT:
        log_print("[dcf ] unreadable mark of %u ms (expect ~100 or ~200 ms)\n", evt->measured_ms);
        if (!polarity_hint_done && evt->measured_ms >= 600 && evt->measured_ms <= 960) {
            if (++long_mark_count >= 5) {
                polarity_hint_done = true;
                log_print("[hint] marks of ~800/900 ms suggest an inverted signal — "
                          "set cfg.signal_inverted = true in app_main()\n");
            }
        }
        break;

    case DCF77_EVT_RESYNC:
        log_print("[dcf ] resync: %s (%u ms) — bit alignment lost\n", evt->reason, evt->measured_ms);
        break;

    case DCF77_EVT_MINUTE_MARK:
        log_print("[dcf ] minute mark (%u ms gap)\n", evt->measured_ms);
        break;

    case DCF77_EVT_FRAME_OK: {
        char bitstr[96];
        format_frame_bits(evt->bits, evt->bit_count, bitstr, sizeof(bitstr));
        const dcf77_time_t *t = &evt->time;
        const civil_t       u = civil_from_epoch(t->utc_epoch);
        log_print("[frame] %s\n", bitstr);
        log_print("[frame] OK: %s %04u-%02u-%02u %02u:%02u:00 %s = %02d:%02d UTC%s%s%s\n",
                  weekday_name(t->weekday), t->year, t->month, t->day, t->hour, t->minute,
                  t->cest ? "CEST" : "CET", u.hh, u.mm,
                  t->dst_change_announced ? " [DST change announced]" : "",
                  t->leap_second_announced ? " [leap second announced]" : "",
                  t->call_bit ? " [call bit]" : "");
        show_cest = t->cest;
        if (!st.time_valid) {
            log_print("[frame] awaiting confirmation by the next frame (FR-DCF-007)\n");
        }
        break;
    }

    case DCF77_EVT_FRAME_REJECT: {
        char bitstr[96];
        format_frame_bits(evt->bits, evt->bit_count, bitstr, sizeof(bitstr));
        log_print("[frame] %s\n", bitstr);
        log_print("[frame] rejected (%u bits): %s\n", evt->bit_count, evt->reason);
        break;
    }

    case DCF77_EVT_TIME_CONFIRMED:
        utc_epoch  = evt->time.utc_epoch;
        fresh_sync = true;
        if (st.confirmations <= 1) { // the snapshot already includes this confirmation
            log_print("==========================================================\n");
            log_print("[time] CONFIRMED by two consecutive frames (FR-DCF-007)\n");
            log_print("==========================================================\n");
        } else {
            log_print("[time] re-confirmed at minute mark\n");
        }
        break;
    }
}

static void on_dcf_tick(int64_t t_us, int16_t second, void *ctx)
{
    (void)t_us;
    (void)second;
    (void)ctx;
    if (utc_epoch < 0) {
        return; // no confirmed time yet — acquisition progress is printed instead
    }
    if (fresh_sync) {
        fresh_sync = false; // epoch was set at this very mark
    } else {
        utc_epoch++;
    }
    const civil_t l = civil_from_epoch(utc_epoch + (show_cest ? 2 : 1) * 3600);
    const civil_t u = civil_from_epoch(utc_epoch);
    log_print("[time] %04d-%02d-%02d %02d:%02d:%02d %s (UTC %02d:%02d:%02d)\n",
              l.y, l.m, l.d, l.hh, l.mm, l.ss, show_cest ? "CEST" : "CET",
              u.hh, u.mm, u.ss);
}

static void decoder_setup(void)
{
    log_print("Decoder: dcf77 — simple edge-timing (PMC-STD-001 #5.10 baseline)\n");
    log_print("------------------------------------------------------------------\n");

    dcf77_config_t cfg = dcf77_config_default();
    cfg.event_cb = on_dcf_event;
    cfg.tick_cb  = on_dcf_tick;

    if (!dcf77_init(&dcf, &cfg) || !dcf77_start(&dcf)) {
        log_print("[dcf ] FATAL: decoder init/start failed\n");
        return;
    }
    log_print("[dcf ] receiver powered, decoding — a confirmed time takes 2-5 minutes\n");
}

static void decoder_status_line(void)
{
    dcf77_status_t st;
    dcf77_get_status(&dcf, &st);
    static const char *const state_names[] =
        {"OFF", "NO_SIGNAL", "SYNCING", "COLLECTING", "LOCKED"};
    log_print("[stat] %s | second %d | edges %lu (%lu glitches) | bits %lu ok / %lu bad | "
              "frames %lu ok / %lu bad | confirmed %lu\n",
              state_names[st.state], st.second,
              (unsigned long)st.edges, (unsigned long)st.glitches,
              (unsigned long)st.bits_ok, (unsigned long)st.pulses_rejected,
              (unsigned long)st.frames_ok, (unsigned long)st.frames_rejected,
              (unsigned long)st.confirmations);
}

static bool decoder_led_steady(void)
{
    dcf77_status_t st;
    dcf77_get_status(&dcf, &st);
    return st.state == DCF77_STATE_LOCKED;
}

static bool decoder_led_off(void)
{
    dcf77_status_t st;
    dcf77_get_status(&dcf, &st);
    return st.state == DCF77_STATE_OFF;
}

#endif /* USE_PHASE_DECODER */

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void app_main(void)
{
    log_mutex = xSemaphoreCreateMutex();

    const gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << LED_DCF_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_DCF_GPIO, 0);

    log_print("\n");
    log_print("DCF77 decoder proof of concept — PanelMeterClock Research/DCFTest\n");
    log_print("Time-code GPIO 11 (input, pull-up) | PON GPIO 12 (low = on) | DCF LED GPIO 5\n");
    log_print("Polarity: mark = HIGH; set cfg.signal_inverted for inverted modules (FR-DCF-004)\n");
    log_print("Decoder task: core 0, priority 7 (PMC-STD-001 #4)\n");
    decoder_setup();

    // LED + periodic status
    uint32_t iter   = 0;
    bool     led_on = false;
    for (;;) {
        // DCF LED per PMC-GUI-001: blink ~1 Hz while acquiring, steady when valid.
        if (decoder_led_steady()) {
            led_on = true;
        } else if (decoder_led_off()) {
            led_on = false;
        } else if (iter % 2 == 0) {
            led_on = !led_on;
        }
        gpio_set_level(LED_DCF_GPIO, led_on ? 1 : 0);

        if (iter > 0 && iter % (STATUS_INTERVAL_MS / LED_INTERVAL_MS) == 0) {
            decoder_status_line();
        }

        iter++;
        vTaskDelay(pdMS_TO_TICKS(LED_INTERVAL_MS));
    }
}
