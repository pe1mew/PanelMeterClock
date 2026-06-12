/**
 * @file dcf77p.c
 * @brief DCF77 phase-locked exhaustive decoder — ESP-IDF port of Udo
 *        Klein's blinkenlight decoder (see dcf77p.h).
 *
 * Copyright 2013 Udo Klein (original decoder, www.blinkenlight.net)
 * Copyright 2026 Remko Welling (ESP-IDF port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/
 *
 * Port notes — structure maps 1:1 onto the reference sketch
 * (Documentation/src/blinkenLightDC77DecoderAll.c):
 *
 *   AVR Timer2 1 kHz ISR            → esp_timer 1 kHz callback (sampling
 *                                     + 10-sample majority, stage 0)
 *   DCF77_Demodulator               → phase_binning / phase_detection /
 *                                     detector_stage_2 / decode_220ms
 *   DCF77_Second_Decoder            → sec_* (60-bin sync-mark scoring)
 *   DCF77_{Minute,...,Year}_Decoder → kbins_* + *_process_tick
 *   DCF77_Flag_Decoder              → flag_*
 *   DCF77_Encoder                   → enc_* (time prediction)
 *   DCF77_Clock_Controller          → ctrl_process_tick / ctrl_flush
 *
 * Deliberate deviations from the reference sketch:
 *  1. Year decoder bit 56 weight corrected from 0x20 to 0x40 (sketch bug;
 *     fixed the same way in Klein's later library).
 *  2. The leap-second probe `get_current_signal(now) != sync_mark` at
 *     second 59 is replaced by its closed form
 *     `leap_second_scheduled && minute == 0` — the only case in which
 *     get_current_signal() does not return sync_mark at second 59. The
 *     200-line signal predictor is otherwise unused and not ported.
 *  3. The busy-waiting get_current_time() is replaced by callbacks from
 *     the decoder task (RTOS event-driven instead of AVR polling).
 */

#include "dcf77p.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_err.h"

// ---------------------------------------------------------------------------
// Constants (Klein's tuning values)
// ---------------------------------------------------------------------------

#define PBIN_COUNT            100  /* phase bins of 10 ms per second        */
#define SAMPLES_PER_BIN       10   /* 1 kHz sampling, 10 ms bins            */
#define PHASE_BIN_MAX         300  /* integration cap "N": N * 30 ppm < 1   */
#define PHASE_LOCK_THRESHOLD  30   /* convolution max-noise gate            */
#define SECOND_LOCK_THRESHOLD 12   /* sync-mark binning lock threshold      */

#define SLOT_QUEUE_LEN 128         /* 10 ms slots; 1.28 s of buffering      */
#define TASK_POLL_MS   100

typedef struct {
    int64_t t_us;
    uint8_t bit;
} slot_msg_t;

#define STATUS_LOCK(p)   portENTER_CRITICAL(&(p)->status_mux)
#define STATUS_UNLOCK(p) portEXIT_CRITICAL(&(p)->status_mux)

static void emit(dcf77p_t *p, const dcf77p_event_t *evt)
{
    if (p->cfg.event_cb) {
        p->cfg.event_cb(evt, p->cfg.cb_ctx);
    }
}

// ---------------------------------------------------------------------------
// Arithmetic tools (Klein's Arithmetic_Tools / BCD)
// ---------------------------------------------------------------------------

static inline void bounded_add_u8(uint8_t *v, uint8_t n)
{
    *v = (*v >= 255 - n) ? 255 : *v + n;
}

static inline void bounded_sub_u8(uint8_t *v, uint8_t n)
{
    *v = (*v <= n) ? 0 : *v - n;
}

static inline uint8_t bit_count8(uint8_t v)
{
    const uint8_t t1 = (v & 0x55) + ((v >> 1) & 0x55);
    const uint8_t t2 = (t1 & 0x33) + ((t1 >> 2) & 0x33);
    return (t2 & 0x0f) + (t2 >> 4);
}

static inline uint8_t parity8(uint8_t v)
{
    v = (v & 0xf) ^ (v >> 4);
    v = (v & 0x3) ^ (v >> 2);
    v = (v & 0x1) ^ (v >> 1);
    return v;
}

static inline uint8_t bcd_to_int(uint8_t bcd)
{
    return (bcd & 0x0f) + 10 * (bcd >> 4);
}

static inline uint8_t int_to_bcd(uint8_t v)
{
    const uint8_t hi = v / 10;
    return (uint8_t)((hi << 4) | (v - 10 * hi));
}

/** BCD increment with nibble wrap (0x09→0x10, 0x99→0x00). */
static void bcd_increment(uint8_t *bcd)
{
    uint8_t lo = *bcd & 0x0f;
    uint8_t hi = *bcd >> 4;
    if (lo < 9) {
        ++lo;
    } else {
        lo = 0;
        hi = (hi < 9) ? hi + 1 : 0;
    }
    *bcd = (uint8_t)((hi << 4) | lo);
}

// ---------------------------------------------------------------------------
// Calendar helpers for the public UTC epoch (not part of the Klein port)
// ---------------------------------------------------------------------------

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static uint8_t days_in_month_int(uint16_t y, uint8_t m)
{
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) {
        return 29;
    }
    return dim[m - 1];
}

// ---------------------------------------------------------------------------
// Generic exhaustive decoder bins (Klein's Hamming namespace)
// ---------------------------------------------------------------------------

static void kbins_init(dcf77p_kbins_t *b, uint8_t *storage, uint8_t n)
{
    b->data = storage;
    b->n    = n;
}

static void kbins_setup(dcf77p_kbins_t *b)
{
    memset(b->data, 0, b->n);
    b->tick      = 0;
    b->max       = 0;
    b->noise_max = 0;
    b->max_index = 255;
}

static void kbins_advance(dcf77p_kbins_t *b)
{
    b->tick = (b->tick < b->n - 1) ? b->tick + 1 : 0;
}

static void kbins_compute_max(dcf77p_kbins_t *b)
{
    b->noise_max = 0;
    b->max       = 0;
    b->max_index = 255;
    for (uint8_t i = 0; i < b->n; ++i) {
        const uint8_t v = b->data[i];
        if (v >= b->max) {
            b->noise_max = b->max;
            b->max       = v;
            b->max_index = i;
        } else if (v > b->noise_max) {
            b->noise_max = v;
        }
    }
}

/**
 * Score every candidate value against the received (BCD) byte by Hamming
 * distance and accumulate the evidence into the candidate's bin. The ring
 * is rotated by the current tick so each bin keeps tracking "its" value
 * as time advances.
 */
static void kbins_hamming_binning(dcf77p_kbins_t *b, uint8_t input,
                                  uint8_t significant_bits, bool with_parity)
{
    if (b->max > 255 - significant_bits) {
        // cannot raise the maximum any further — lower the noise floor instead
        for (uint8_t i = 0; i < b->n; ++i) {
            bounded_sub_u8(&b->data[i], significant_bits);
        }
        b->max -= significant_bits;
        bounded_sub_u8(&b->noise_max, significant_bits);
    }

    const uint8_t offset = b->n - 1 - b->tick;
    uint8_t       idx    = offset;
    // minutes/hours carry parity and count from 0; years count from 0;
    // days, weekdays and months count from 1
    uint8_t candidate = (with_parity || b->n == 100) ? 0x00 : 0x01;
    for (uint8_t pass = 0; pass < b->n; ++pass) {
        uint8_t cand = candidate;
        if (with_parity) {
            cand |= (uint8_t)(parity8(candidate) << 7);
        }
        const uint8_t score = significant_bits - bit_count8(input ^ cand);
        bounded_add_u8(&b->data[idx], score);

        idx = (idx < b->n - 1) ? idx + 1 : 0;
        bcd_increment(&candidate);
    }
}

/** Best candidate as BCD, or 0xFF when the evidence margin is too small. */
static uint8_t kbins_get_value(const dcf77p_kbins_t *b)
{
    const uint8_t threshold = 2;
    const uint8_t offset    = (b->n == 60 || b->n == 24 || b->n == 100) ? 0 : 1;
    if ((uint8_t)(b->max - b->noise_max) >= threshold) {
        return int_to_bcd((uint8_t)(((uint16_t)b->max_index + b->tick + 1) % b->n + offset));
    }
    return 0xFF;
}

// ---------------------------------------------------------------------------
// Time prediction (Klein's DCF77_Encoder, BCD arithmetic)
// ---------------------------------------------------------------------------

static uint8_t enc_days_per_month(const dcf77p_ktime_t *t)
{
    switch (t->month_bcd) {
    case 0x02:
        // valid till 31.12.2399; year mod 4 == year & 0x03
        return 28 + (((t->year_bcd != 0) && ((bcd_to_int(t->year_bcd) & 0x03) == 0)) ? 1 : 0);
    case 0x01: case 0x03: case 0x05: case 0x07:
    case 0x08: case 0x10: case 0x12:
        return 31;
    case 0x04: case 0x06: case 0x09: case 0x11:
        return 30;
    default:
        return 0;
    }
}

/** Gauss weekday formula on BCD data; 0 = Sunday, 0xFF = undefined input. */
static uint8_t enc_weekday(const dcf77p_ktime_t *t)
{
    if (t->day_bcd <= 0x31 && t->month_bcd <= 0x12 && t->year_bcd <= 0x99) {
        const uint8_t  d = bcd_to_int(t->day_bcd);
        const uint16_t m = (t->month_bcd <= 0x02) ? (uint16_t)t->month_bcd + 10
                                                  : (uint16_t)bcd_to_int(t->month_bcd) - 2;
        const uint8_t  y = bcd_to_int(t->year_bcd) - (t->month_bcd <= 0x02);
        uint8_t day_mod_7 = d + (26 * m - 2) / 10 + y + y / 4;
        while (day_mod_7 >= 7) {
            day_mod_7 -= 7;
            day_mod_7 = (uint8_t)((day_mod_7 >> 3) + (day_mod_7 & 7));
        }
        return day_mod_7;
    }
    return 0xFF;
}

static void enc_autoset_weekday(dcf77p_ktime_t *t)
{
    t->weekday_bcd = enc_weekday(t);
    if (t->weekday_bcd == 0) {
        t->weekday_bcd = 0x07;
    }
}

static void enc_autoset_timezone(dcf77p_ktime_t *t)
{
    // CEST is in force from the last Sunday of March 02:00 CET to the last
    // Sunday of October 03:00 CEST; the last Sunday is always in [25..31]
    if (t->month_bcd < 0x03) {
        t->uses_summertime = false;
    } else if (t->month_bcd == 0x03) {
        if (t->day_bcd < 0x25) {
            t->uses_summertime = false;
        } else {
            const uint8_t wd = enc_weekday(t);
            if (wd != 0) {
                t->uses_summertime = !(t->day_bcd - wd < 0x25);
            } else { // last Sunday of March
                t->uses_summertime = (t->hour_bcd > 2);
            }
        }
    } else if (t->month_bcd < 0x10) {
        t->uses_summertime = true;
    } else if (t->month_bcd == 0x10) {
        if (t->day_bcd < 0x25) {
            t->uses_summertime = true;
        } else {
            const uint8_t wd = enc_weekday(t);
            if (wd != 0) {
                t->uses_summertime = (t->day_bcd - wd < 0x25);
            } else { // last Sunday of October
                if (t->hour_bcd == 2) {
                    // cannot be derived from time data; keep the flag value
                } else {
                    t->uses_summertime = (t->hour_bcd < 2);
                }
            }
        }
    } else {
        t->uses_summertime = false;
    }
}

static void enc_autoset_timezone_change_scheduled(dcf77p_ktime_t *t)
{
    if (t->day_bcd < 0x25 || enc_weekday(t) != 0) {
        // changes happen only on the last Sunday of March/October;
        // undefined (0xFF) day/weekday data causes no action
        t->timezone_change_scheduled = false;
    } else {
        if (t->month_bcd == 0x03) {
            if (t->uses_summertime) {
                // preparing the first minute of summer time
                t->timezone_change_scheduled = (t->hour_bcd == 0x03 && t->minute_bcd == 0x00);
            } else {
                t->timezone_change_scheduled = (t->hour_bcd == 0x01 && t->minute_bcd != 0x00);
            }
        } else if (t->month_bcd == 0x10) {
            if (t->uses_summertime) {
                t->timezone_change_scheduled = (t->hour_bcd == 0x02 && t->minute_bcd != 0x00);
            } else {
                // preparing the first minute of winter time
                t->timezone_change_scheduled = (t->hour_bcd == 0x02 && t->minute_bcd == 0x00);
            }
        } else if (t->month_bcd <= 0x12) {
            t->timezone_change_scheduled = false;
        }
    }
}

static void enc_verify_leap_second_scheduled(dcf77p_ktime_t *t)
{
    // leap seconds happen at 00:00 UTC == 01:00 CET == 02:00 CEST,
    // and (in practice) only on the 1st of January/April/July/October
    t->leap_second_scheduled = t->leap_second_scheduled && (t->day_bcd == 0x01);

    if (t->month_bcd == 0x01) {
        t->leap_second_scheduled = t->leap_second_scheduled &&
            ((t->hour_bcd == 0x00 && t->minute_bcd != 0x00) ||
             (t->hour_bcd == 0x01 && t->minute_bcd == 0x00));
    } else if (t->month_bcd == 0x07 || t->month_bcd == 0x04 || t->month_bcd == 0x10) {
        t->leap_second_scheduled = t->leap_second_scheduled &&
            ((t->hour_bcd == 0x01 && t->minute_bcd != 0x00) ||
             (t->hour_bcd == 0x02 && t->minute_bcd == 0x00));
    } else {
        t->leap_second_scheduled = false;
    }
}

static void enc_autoset_control_bits(dcf77p_ktime_t *t)
{
    enc_autoset_weekday(t);
    enc_autoset_timezone(t);
    enc_autoset_timezone_change_scheduled(t);
    // leap seconds cannot be computed, only constrained
    enc_verify_leap_second_scheduled(t);
}

static void enc_reset(dcf77p_ktime_t *t)
{
    t->second      = 0;
    t->minute_bcd  = 0x00;
    t->hour_bcd    = 0x00;
    t->day_bcd     = 0x01;
    t->month_bcd   = 0x01;
    t->year_bcd    = 0x00;
    t->weekday_bcd = 0x01;
    t->uses_summertime           = false;
    t->uses_backup_antenna       = false;
    t->timezone_change_scheduled = false;
    t->leap_second_scheduled     = false;
}

/** Advance the predicted time by one second. Out-of-range (0xFF) fields are
 *  deliberately not advanced. */
static void enc_advance_second(dcf77p_ktime_t *t)
{
    if (t->second < 59) {
        ++t->second;
        if (t->second == 15) {
            enc_autoset_control_bits(t);
        }
    } else if (t->leap_second_scheduled && t->second == 59 && t->minute_bcd == 0x00) {
        t->second = 60;
        t->leap_second_scheduled = false;
    } else if (t->second == 59 || t->second == 60) {
        t->second = 0;
        if (t->minute_bcd < 0x59) {
            bcd_increment(&t->minute_bcd);
        } else if (t->minute_bcd == 0x59) {
            t->minute_bcd = 0x00;
            if (t->timezone_change_scheduled && !t->uses_summertime && t->hour_bcd == 0x01) {
                // CET -> CEST: clock jumps from 01:59 CET to 03:00 CEST
                bcd_increment(&t->hour_bcd);
                bcd_increment(&t->hour_bcd);
                t->uses_summertime = true;
            } else if (t->timezone_change_scheduled && t->uses_summertime && t->hour_bcd == 0x02) {
                // CEST -> CET: clock falls back from 02:59 CEST to 02:00 CET
                t->uses_summertime = false;
            } else {
                if (t->hour_bcd < 0x23) {
                    bcd_increment(&t->hour_bcd);
                } else if (t->hour_bcd == 0x23) {
                    t->hour_bcd = 0x00;
                    if (t->weekday_bcd < 0x07) {
                        bcd_increment(&t->weekday_bcd);
                    } else if (t->weekday_bcd == 0x07) {
                        t->weekday_bcd = 0x01;
                    }
                    if (bcd_to_int(t->day_bcd) < enc_days_per_month(t)) {
                        bcd_increment(&t->day_bcd);
                    } else if (bcd_to_int(t->day_bcd) == enc_days_per_month(t)) {
                        t->day_bcd = 0x01;
                        if (t->month_bcd < 0x12) {
                            bcd_increment(&t->month_bcd);
                        } else if (t->month_bcd == 0x12) {
                            t->month_bcd = 0x01;
                            if (t->year_bcd < 0x99) {
                                bcd_increment(&t->year_bcd);
                            } else if (t->year_bcd == 0x99) {
                                t->year_bcd = 0x00;
                            }
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Flag decoder (Klein's DCF77_Flag_Decoder)
// ---------------------------------------------------------------------------

static void flag_cummulate(int8_t *avg, bool count_up)
{
    if (count_up) {
        *avg += (*avg < 127);
    } else {
        *avg -= (*avg > -127);
    }
}

static void flag_process_tick(dcf77p_t *p, uint8_t second, uint8_t tick_value)
{
    switch (second) {
    case 15: p->flag_backup = tick_value != 0;                    break;
    case 16: flag_cummulate(&p->flag_tz_change, tick_value);      break;
    case 17: flag_cummulate(&p->flag_summertime, tick_value);     break;
    case 18: flag_cummulate(&p->flag_summertime, 1 - tick_value); break;
    case 19: flag_cummulate(&p->flag_leap, tick_value);           break;
    case 58: flag_cummulate(&p->flag_date_parity, tick_value);    break;
    default: break;
    }
}

static void flag_reset_after_previous_hour(dcf77p_t *p)
{
    // timezone_change_scheduled and leap_second_scheduled are set from
    // hh:01 to HH:00 only
    if (p->flag_tz_change) {
        p->flag_tz_change  = 0;
        p->flag_summertime = 0;
    }
    p->flag_leap = 0;
}

static void flag_reset_before_new_day(dcf77p_t *p)
{
    p->flag_date_parity = 0;
}

// ---------------------------------------------------------------------------
// Per-field exhaustive decoders (Klein's DCF77_*_Decoder)
// ---------------------------------------------------------------------------

static void minute_process_tick(dcf77p_t *p, uint8_t second, uint8_t tv)
{
    switch (second) {
    case 21: p->min_acc += tv;        break;
    case 22: p->min_acc += 0x02 * tv; break;
    case 23: p->min_acc += 0x04 * tv; break;
    case 24: p->min_acc += 0x08 * tv; break;
    case 25: p->min_acc += 0x10 * tv; break;
    case 26: p->min_acc += 0x20 * tv; break;
    case 27: p->min_acc += 0x40 * tv; break;
    case 28: p->min_acc += 0x80 * tv; // parity bit
             kbins_hamming_binning(&p->min_bins, p->min_acc, 8, true);
             break;
    case 29: kbins_compute_max(&p->min_bins);
             p->min_acc = 0;
             break;
    default: p->min_acc = 0; break;
    }
}

static void hour_process_tick(dcf77p_t *p, uint8_t second, uint8_t tv)
{
    switch (second) {
    case 29: p->hour_acc += tv;        break;
    case 30: p->hour_acc += 0x02 * tv; break;
    case 31: p->hour_acc += 0x04 * tv; break;
    case 32: p->hour_acc += 0x08 * tv; break;
    case 33: p->hour_acc += 0x10 * tv; break;
    case 34: p->hour_acc += 0x20 * tv; break;
    case 35: p->hour_acc += 0x80 * tv; // parity bit
             kbins_hamming_binning(&p->hour_bins, p->hour_acc, 7, true);
             break;
    case 36: kbins_compute_max(&p->hour_bins);
             p->hour_acc = 0;
             break;
    default: p->hour_acc = 0; break;
    }
}

static void day_process_tick(dcf77p_t *p, uint8_t second, uint8_t tv)
{
    switch (second) {
    case 36: p->day_acc += tv;        break;
    case 37: p->day_acc += 0x02 * tv; break;
    case 38: p->day_acc += 0x04 * tv; break;
    case 39: p->day_acc += 0x08 * tv; break;
    case 40: p->day_acc += 0x10 * tv; break;
    case 41: p->day_acc += 0x20 * tv;
             kbins_hamming_binning(&p->day_bins, p->day_acc, 6, false);
             break;
    case 42: kbins_compute_max(&p->day_bins);
             p->day_acc = 0;
             break;
    default: p->day_acc = 0; break;
    }
}

static void wday_process_tick(dcf77p_t *p, uint8_t second, uint8_t tv)
{
    switch (second) {
    case 42: p->wday_acc += tv;        break;
    case 43: p->wday_acc += 0x02 * tv; break;
    case 44: p->wday_acc += 0x04 * tv;
             kbins_hamming_binning(&p->wday_bins, p->wday_acc, 3, false);
             break;
    case 45: kbins_compute_max(&p->wday_bins);
             p->wday_acc = 0;
             break;
    default: p->wday_acc = 0; break;
    }
}

static void month_process_tick(dcf77p_t *p, uint8_t second, uint8_t tv)
{
    switch (second) {
    case 45: p->month_acc += tv;        break;
    case 46: p->month_acc += 0x02 * tv; break;
    case 47: p->month_acc += 0x04 * tv; break;
    case 48: p->month_acc += 0x08 * tv; break;
    case 49: p->month_acc += 0x10 * tv;
             kbins_hamming_binning(&p->month_bins, p->month_acc, 5, false);
             break;
    case 50: kbins_compute_max(&p->month_bins);
             p->month_acc = 0;
             break;
    default: p->month_acc = 0; break;
    }
}

static void year_process_tick(dcf77p_t *p, uint8_t second, uint8_t tv)
{
    switch (second) {
    case 50: p->year_acc += tv;        break;
    case 51: p->year_acc += 0x02 * tv; break;
    case 52: p->year_acc += 0x04 * tv; break;
    case 53: p->year_acc += 0x08 * tv; break;
    case 54: p->year_acc += 0x10 * tv; break;
    case 55: p->year_acc += 0x20 * tv; break;
    case 56: p->year_acc += 0x40 * tv; break; // 0x20 in the reference sketch — fixed
    case 57: p->year_acc += 0x80 * tv;
             kbins_hamming_binning(&p->year_bins, p->year_acc, 8, false);
             break;
    case 58: kbins_compute_max(&p->year_bins);
             p->year_acc = 0;
             break;
    default: p->year_acc = 0; break;
    }
}

// ---------------------------------------------------------------------------
// Second decoder — sync-mark binning (Klein's DCF77_Second_Decoder)
// ---------------------------------------------------------------------------

static uint8_t sec_get_second(const dcf77p_t *p)
{
    const dcf77p_kbins_t *b = &p->sec_bins;
    if ((uint8_t)(b->max - b->noise_max) >= SECOND_LOCK_THRESHOLD) {
        // subtract 2: 1 because the tick already advanced, 1 because the
        // sync mark is second 59, not second 0
        uint8_t second = (uint8_t)(2 * 60 + b->tick - 2 - b->max_index);
        while (second >= 60) {
            second -= 60;
        }
        return second;
    }
    return 0xFF;
}

/**
 * Score the sync-mark position. A sync mark earns +6 for its bin; a 0 earns
 * +1 for the previous bin, a 1 earns +1 for the bin 21 positions back (the
 * "must be a 1 at second 20" property); every observation also subtracts
 * evidence from the positions it contradicts. See the reference sketch for
 * the full derivation.
 */
static void sec_sync_mark_binning(dcf77p_t *p, uint8_t tick_data)
{
    dcf77p_kbins_t *b = &p->sec_bins;
    const uint8_t prev    = (b->tick > 0) ? b->tick - 1 : 59;
    const uint8_t prev_21 = (b->tick > 20) ? b->tick - 21 : b->tick + 60 - 21;

    switch (tick_data) {
    case DCF77P_TICK_SYNC: {
        bounded_add_u8(&b->data[b->tick], 6);
        bounded_sub_u8(&b->data[prev], 2);
        bounded_sub_u8(&b->data[prev_21], 2);
        const uint8_t next = (b->tick < 59) ? b->tick + 1 : 0;
        bounded_sub_u8(&b->data[next], 2);
        break;
    }
    case DCF77P_TICK_ZERO:
        bounded_add_u8(&b->data[prev], 1);
        bounded_sub_u8(&b->data[b->tick], 2);
        bounded_sub_u8(&b->data[prev_21], 2);
        break;
    case DCF77P_TICK_ONE:
        bounded_add_u8(&b->data[prev_21], 1);
        bounded_sub_u8(&b->data[b->tick], 2);
        bounded_sub_u8(&b->data[prev], 2);
        break;
    case DCF77P_TICK_UNDEFINED:
    default:
        bounded_sub_u8(&b->data[b->tick], 2);
        bounded_sub_u8(&b->data[prev], 2);
        bounded_sub_u8(&b->data[prev_21], 2);
        break;
    }
    b->tick = (b->tick < 59) ? b->tick + 1 : 0;

    if ((uint8_t)(b->max - b->noise_max) <= SECOND_LOCK_THRESHOLD ||
        sec_get_second(p) == 3) {
        // cheap while unlocked is irrelevant; once locked this runs only
        // once per minute (when the sync mark was just confirmed)
        kbins_compute_max(b);
    }
}

// ---------------------------------------------------------------------------
// Clock controller (Klein's DCF77_Clock_Controller)
// ---------------------------------------------------------------------------

static void ctrl_set_from_decoders(dcf77p_t *p, dcf77p_ktime_t *now)
{
    now->second      = sec_get_second(p);
    now->minute_bcd  = kbins_get_value(&p->min_bins);
    now->hour_bcd    = kbins_get_value(&p->hour_bins);
    now->weekday_bcd = kbins_get_value(&p->wday_bins);
    now->day_bcd     = kbins_get_value(&p->day_bins);
    now->month_bcd   = kbins_get_value(&p->month_bins);
    now->year_bcd    = kbins_get_value(&p->year_bins);

    now->uses_backup_antenna       = p->flag_backup;
    now->timezone_change_scheduled = p->flag_tz_change > 0;
    now->uses_summertime           = p->flag_summertime > 0;
    now->leap_second_scheduled     = p->flag_leap > 0;
}

static void build_public_time(const dcf77p_ktime_t *kt, dcf77p_time_t *out)
{
    out->second  = kt->second;
    out->minute  = (kt->minute_bcd  == 0xFF) ? 0xFF : bcd_to_int(kt->minute_bcd);
    out->hour    = (kt->hour_bcd    == 0xFF) ? 0xFF : bcd_to_int(kt->hour_bcd);
    out->day     = (kt->day_bcd     == 0xFF) ? 0xFF : bcd_to_int(kt->day_bcd);
    out->weekday = (kt->weekday_bcd == 0xFF) ? 0xFF : bcd_to_int(kt->weekday_bcd);
    out->month   = (kt->month_bcd   == 0xFF) ? 0xFF : bcd_to_int(kt->month_bcd);
    out->year2   = (kt->year_bcd    == 0xFF) ? 0xFF : bcd_to_int(kt->year_bcd);
    out->cest                  = kt->uses_summertime;
    out->dst_change_announced  = kt->timezone_change_scheduled;
    out->leap_second_announced = kt->leap_second_scheduled;
    out->backup_antenna        = kt->uses_backup_antenna;

    out->valid     = false;
    out->utc_epoch = 0;
    if (out->second <= 60 && out->minute <= 59 && out->hour <= 23 &&
        out->month >= 1 && out->month <= 12 && out->year2 <= 99 &&
        out->day >= 1 && out->day <= days_in_month_int(2000 + out->year2, out->month) &&
        out->weekday >= 1 && out->weekday <= 7) {
        const int64_t days  = days_from_civil(2000 + out->year2, out->month, out->day);
        const int64_t civil = days * 86400 + (int64_t)out->hour * 3600 +
                              (int64_t)out->minute * 60 + out->second;
        out->utc_epoch = civil - (out->cest ? 2 : 1) * 3600; // FR-DCF-009
        out->valid     = true;
    }
}

static void gather_quality(dcf77p_t *p, dcf77p_quality_t *q)
{
    q->phase_lock  = p->pmax;
    q->phase_noise = p->pnoise;
    q->second.lock  = p->sec_bins.max;   q->second.noise  = p->sec_bins.noise_max;
    q->minute.lock  = p->min_bins.max;   q->minute.noise  = p->min_bins.noise_max;
    q->hour.lock    = p->hour_bins.max;  q->hour.noise    = p->hour_bins.noise_max;
    q->day.lock     = p->day_bins.max;   q->day.noise     = p->day_bins.noise_max;
    q->weekday.lock = p->wday_bins.max;  q->weekday.noise = p->wday_bins.noise_max;
    q->month.lock   = p->month_bins.max; q->month.noise   = p->month_bins.noise_max;
    q->year.lock    = p->year_bins.max;  q->year.noise    = p->year_bins.noise_max;
    q->summertime_q = (uint8_t)(p->flag_summertime < 0 ? -p->flag_summertime : p->flag_summertime);
    q->tz_change_q  = (uint8_t)(p->flag_tz_change < 0 ? -p->flag_tz_change : p->flag_tz_change);
    q->leap_q       = (uint8_t)(p->flag_leap < 0 ? -p->flag_leap : p->flag_leap);
}

static void update_state(dcf77p_t *p, int64_t t_us)
{
    dcf77p_state_t ns;
    bool time_valid;
    STATUS_LOCK(p);
    time_valid = p->status.last_time.valid;
    STATUS_UNLOCK(p);

    if (!p->running) {
        ns = DCF77P_STATE_OFF;
    } else if (p->pmax - p->pnoise < PHASE_LOCK_THRESHOLD) {
        ns = DCF77P_STATE_ACQUIRING_PHASE;
    } else if (sec_get_second(p) == 0xFF) {
        ns = DCF77P_STATE_ACQUIRING_SECOND;
    } else if (!time_valid) {
        ns = DCF77P_STATE_DECODING;
    } else {
        ns = DCF77P_STATE_TIME_VALID;
    }

    dcf77p_state_t os;
    STATUS_LOCK(p);
    os = p->status.state;
    if (ns != os) {
        p->status.state = ns;
    }
    STATUS_UNLOCK(p);

    if (ns != os) {
        dcf77p_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type      = DCF77P_EVT_STATE_CHANGE;
        evt.t_us      = t_us;
        evt.old_state = os;
        evt.new_state = ns;
        emit(p, &evt);
    }
}

/** Ingest one classified second mark (called 220 ms into each second). */
static void ctrl_process_tick(dcf77p_t *p, uint8_t tick_data, int64_t t_us)
{
    dcf77p_ktime_t now;
    ctrl_set_from_decoders(p, &now);
    now.second += p->leap_second;
    enc_advance_second(&now);

    // leap_second == 2: second slot of the leap-second handling;
    // leap_second == 1: processing second 59 which is not the sync mark
    p->leap_second <<= 1;
    p->leap_second += (now.second == 59 &&
                       now.leap_second_scheduled && now.minute_bcd == 0x00);

    if (p->leap_second != 1) {
        sec_sync_mark_binning(p, tick_data);

        if (now.second == 0) {
            kbins_advance(&p->min_bins);
            if (now.minute_bcd == 0x00) {
                // "while" takes care of timezone changes automatically
                while (kbins_get_value(&p->hour_bins) <= 0x23 &&
                       kbins_get_value(&p->hour_bins) != now.hour_bcd) {
                    kbins_advance(&p->hour_bins);
                }
                if (now.hour_bcd == 0x00) {
                    if (kbins_get_value(&p->wday_bins) <= 0x07) {
                        kbins_advance(&p->wday_bins);
                    }
                    // "while" takes care of different month lengths
                    while (kbins_get_value(&p->day_bins) <= 0x31 &&
                           kbins_get_value(&p->day_bins) != now.day_bcd) {
                        kbins_advance(&p->day_bins);
                    }
                    if (now.day_bcd == 0x01) {
                        if (kbins_get_value(&p->month_bins) <= 0x12) {
                            kbins_advance(&p->month_bins);
                        }
                        if (now.month_bcd == 0x01) {
                            if (now.year_bcd <= 0x99) {
                                kbins_advance(&p->year_bins);
                            }
                        }
                    }
                }
            }
        }

        const uint8_t tv = (tick_data == DCF77P_TICK_ONE ||
                            tick_data == DCF77P_TICK_UNDEFINED) ? 1 : 0;
        flag_process_tick(p, now.second, tv);
        minute_process_tick(p, now.second, tv);
        hour_process_tick(p, now.second, tv);
        wday_process_tick(p, now.second, tv);
        day_process_tick(p, now.second, tv);
        month_process_tick(p, now.second, tv);
        year_process_tick(p, now.second, tv);
    }

    STATUS_LOCK(p);
    p->status.ticks[tick_data & 0x03]++;
    STATUS_UNLOCK(p);

    dcf77p_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type        = DCF77P_EVT_TICK;
    evt.t_us        = t_us;
    evt.tick        = (dcf77p_tick_class_t)tick_data;
    evt.tick_second = now.second;
    emit(p, &evt);

    update_state(p, t_us);
}

/** Second boundary: advance the predicted time, publish it (Klein's flush). */
static void ctrl_flush(dcf77p_t *p, int64_t t_us)
{
    dcf77p_ktime_t now;
    dcf77p_ktime_t now_1;

    ctrl_set_from_decoders(p, &now);
    now_1 = now;

    // leap_second offset compensates for the skipped second tick
    now.second += (p->leap_second > 0);
    enc_advance_second(&now);
    enc_autoset_control_bits(&now);

    p->decoded.second = now.second;
    if (now.second == 0) {
        // the broadcast always describes the NEXT minute, so at the minute
        // boundary the previous minute's data is the time valid right now
        p->decoded        = now_1;
        p->decoded.second = 0;

        if (now.minute_bcd == 0x01) {
            // last moment of the "old" hour: the hourly flags are complete
            flag_reset_after_previous_hour(p);

            now.uses_summertime           = p->flag_summertime > 0;
            now.timezone_change_scheduled = p->flag_tz_change > 0;
            now.leap_second_scheduled     = p->flag_leap > 0;
            enc_autoset_control_bits(&now);
            p->decoded.uses_summertime           = now.uses_summertime;
            p->decoded.timezone_change_scheduled = now.timezone_change_scheduled;
            p->decoded.leap_second_scheduled     = now.leap_second_scheduled;
            p->decoded.uses_backup_antenna       = p->flag_backup;
        }

        if (now.hour_bcd == 0x23 && now.minute_bcd == 0x59) {
            flag_reset_before_new_day(p);
        }

        p->leap_second &= (p->leap_second < 2);
    }

    dcf77p_time_t pub;
    build_public_time(&p->decoded, &pub);

    STATUS_LOCK(p);
    p->status.seconds++;
    p->status.last_time = pub;
    gather_quality(p, &p->status.quality);
    STATUS_UNLOCK(p);

    update_state(p, t_us);

    dcf77p_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = DCF77P_EVT_SECOND;
    evt.t_us = t_us;
    evt.time = pub;
    emit(p, &evt);

    // FR-DCF-012: phase-predicted second boundary as tick reference
    if (p->cfg.tick_cb) {
        p->cfg.tick_cb(t_us, pub.second <= 60 ? (int16_t)pub.second : -1, p->cfg.cb_ctx);
    }
}

// ---------------------------------------------------------------------------
// Demodulator (Klein's DCF77_Demodulator)
// ---------------------------------------------------------------------------

static uint8_t wrap100(uint16_t v)
{
    while (v >= PBIN_COUNT) {
        v -= PBIN_COUNT;
    }
    return (uint8_t)v;
}

/** Integrate one 10 ms slot into its phase bin (saturating up/down counter). */
static void phase_binning(dcf77p_t *p, uint8_t input)
{
    p->ptick = (p->ptick < PBIN_COUNT - 1) ? p->ptick + 1 : 0;
    if (input) {
        if (p->pdata[p->ptick] < PHASE_BIN_MAX) {
            ++p->pdata[p->ptick];
        }
    } else {
        if (p->pdata[p->ptick] > 0) {
            --p->pdata[p->ptick];
        }
    }
}

/**
 * Convolve the phase bins with the expected pulse shape: double weight for
 * the first 100 ms (always modulated) plus single weight for the second
 * 100 ms (modulated for 1-bits). The maximum locates the second boundary;
 * the same kernel 200 ms later estimates the noise.
 */
static void phase_detection(dcf77p_t *p)
{
    uint32_t integral = 0;

    for (uint16_t bin = 0; bin < 10; ++bin) {
        integral += (uint32_t)p->pdata[bin] << 1;
    }
    for (uint16_t bin = 10; bin < 20; ++bin) {
        integral += p->pdata[bin];
    }

    p->pmax       = 0;
    p->pmax_index = 0;
    for (uint16_t bin = 0; bin < PBIN_COUNT; ++bin) {
        if (integral > p->pmax) {
            p->pmax       = integral;
            p->pmax_index = (uint8_t)bin;
        }
        integral -= (uint32_t)p->pdata[bin] << 1;
        integral += (uint32_t)p->pdata[wrap100(bin + 10)] + p->pdata[wrap100(bin + 20)];
    }

    // noise estimate: the same kernel 200 ms out of phase
    p->pnoise = 0;
    const uint8_t noise_index = wrap100((uint16_t)p->pmax_index + 20);
    for (uint16_t bin = 0; bin < 10; ++bin) {
        p->pnoise += (uint32_t)p->pdata[wrap100((uint16_t)noise_index + bin)] << 1;
    }
    for (uint16_t bin = 10; bin < 20; ++bin) {
        p->pnoise += p->pdata[wrap100((uint16_t)noise_index + bin)];
    }
}

/** Classify the 220 ms window that starts at the second boundary. */
static void decode_220ms(dcf77p_t *p, uint8_t input, uint8_t bins_to_go, int64_t t_us)
{
    p->dec_count += input;
    if (bins_to_go >= 11) {           // first 110 ms: full-modulation window
        if (bins_to_go == 11) {
            p->dec_data  = (p->dec_count > 5) ? 2 : 0;
            p->dec_count = 0;
        }
    } else if (bins_to_go == 0) {     // second 110 ms: 1-bit extension window
        p->dec_data += (p->dec_count > 5) ? 1 : 0;
        p->dec_count = 0;
        // 3 = long tick (1), 2 = short tick (0), 1 = undefined, 0 = sync mark
        ctrl_process_tick(p, p->dec_data, t_us);
    }
}

static void detector_stage_2(dcf77p_t *p, uint8_t input, int64_t t_us)
{
    const uint8_t current_bin = p->ptick;

    if (p->pmax - p->pnoise < PHASE_LOCK_THRESHOLD ||
        wrap100((uint16_t)(PBIN_COUNT + current_bin - p->pmax_index)) == 53) {
        // while unlocked: search continuously; while locked: refresh once
        // per second, far away from anything that consumes runtime
        phase_detection(p);
    }

    if (p->bins_to_process == 0) {
        if (wrap100((uint16_t)(PBIN_COUNT + current_bin - p->pmax_index)) <= 10 ||
            wrap100((uint16_t)(PBIN_COUNT + p->pmax_index - current_bin)) <= 1) {
            // last 10 ms of the current second
            ctrl_flush(p, t_us);
            // start processing the new second's 220 ms window
            p->bins_to_process = 22;
        }
    }

    if (p->bins_to_process > 0) {
        --p->bins_to_process;
        decode_220ms(p, input, p->bins_to_process, t_us);
    }
}

// ---------------------------------------------------------------------------
// Sampler (1 kHz esp_timer callback — Klein's Timer2 ISR, stage 0)
// ---------------------------------------------------------------------------

static void sampler_cb(void *arg)
{
    dcf77p_t *p = (dcf77p_t *)arg;

    const uint8_t s = (uint8_t)gpio_get_level((gpio_num_t)p->cfg.signal_gpio) ^
                      (p->cfg.signal_inverted ? 1 : 0);
    p->sample_acc += s;
    if (++p->sample_cnt >= SAMPLES_PER_BIN) {
        slot_msg_t m;
        m.t_us = esp_timer_get_time();
        m.bit  = (p->sample_acc > SAMPLES_PER_BIN / 2) ? 1 : 0;
        if (xQueueSend(p->slot_queue, &m, 0) != pdTRUE) {
            STATUS_LOCK(p);
            p->status.slots_dropped++;
            STATUS_UNLOCK(p);
        }
        p->sample_acc = 0;
        p->sample_cnt = 0;
    }
}

// ---------------------------------------------------------------------------
// Decoder task
// ---------------------------------------------------------------------------

static void dcf77p_task_fn(void *arg)
{
    dcf77p_t *p = (dcf77p_t *)arg;

    while (p->running) {
        slot_msg_t m;
        if (xQueueReceive(p->slot_queue, &m, pdMS_TO_TICKS(TASK_POLL_MS)) == pdTRUE) {
            phase_binning(p, m.bit);
            detector_stage_2(p, m.bit, m.t_us);
        }
    }

    p->task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Reset of all decoder stages (fresh acquisition)
// ---------------------------------------------------------------------------

static void decoder_reset(dcf77p_t *p)
{
    memset(p->pdata, 0, sizeof(p->pdata));
    p->ptick           = 0;
    p->pmax            = 0;
    p->pnoise          = 0;
    p->pmax_index      = 255;
    p->bins_to_process = 0;
    p->dec_count       = 0;
    p->dec_data        = 0;

    kbins_setup(&p->sec_bins);
    kbins_setup(&p->min_bins);
    kbins_setup(&p->hour_bins);
    kbins_setup(&p->day_bins);
    kbins_setup(&p->wday_bins);
    kbins_setup(&p->month_bins);
    kbins_setup(&p->year_bins);
    p->min_acc = p->hour_acc = p->day_acc = 0;
    p->wday_acc = p->month_acc = p->year_acc = 0;

    p->flag_summertime  = 0;
    p->flag_tz_change   = 0;
    p->flag_leap        = 0;
    p->flag_date_parity = 0;
    p->flag_backup      = false;

    enc_reset(&p->decoded);
    p->leap_second = 0;

    p->sample_acc = 0;
    p->sample_cnt = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

dcf77p_config_t dcf77p_config_default(void)
{
    dcf77p_config_t c = {0};
    c.signal_gpio     = 11;    // IC-HW-005
    c.pon_gpio        = 12;    // IC-HW-006
    c.pon_on_level    = 0;     // module PDN/P1: low = receiver on
    c.signal_inverted = false; // mark (carrier reduction) = HIGH
    c.internal_pullup = true;  // open-collector time-code output

    c.task_stack    = 4096;
    c.task_priority = 7;       // PMC-STD-001 §4
    c.task_core     = 0;       // PRO core, PMC-STD-001 §4
    return c;
}

bool dcf77p_init(dcf77p_t *p, const dcf77p_config_t *cfg)
{
    if (p == NULL || cfg == NULL) {
        return false;
    }
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;

    const portMUX_TYPE mux_init = portMUX_INITIALIZER_UNLOCKED;
    p->status_mux   = mux_init;
    p->status.state = DCF77P_STATE_OFF;

    kbins_init(&p->sec_bins,   p->sec_data,   60);
    kbins_init(&p->min_bins,   p->min_data,   60);
    kbins_init(&p->hour_bins,  p->hour_data,  24);
    kbins_init(&p->day_bins,   p->day_data,   31);
    kbins_init(&p->wday_bins,  p->wday_data,  7);
    kbins_init(&p->month_bins, p->month_data, 12);
    kbins_init(&p->year_bins,  p->year_data,  100);
    decoder_reset(p);

    p->slot_queue = xQueueCreate(SLOT_QUEUE_LEN, sizeof(slot_msg_t));
    if (p->slot_queue == NULL) {
        return false;
    }

    const esp_timer_create_args_t targs = {
        .callback              = sampler_cb,
        .arg                   = p,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "dcf77p_smp",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&targs, &p->sampler) != ESP_OK) {
        return false;
    }
    return true;
}

bool dcf77p_start(dcf77p_t *p)
{
    if (p == NULL || p->slot_queue == NULL || p->sampler == NULL || p->task != NULL) {
        return false;
    }

    const gpio_config_t sig_cfg = {
        .pin_bit_mask = 1ULL << p->cfg.signal_gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = p->cfg.internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE, // sampled, not edge-driven
    };
    if (gpio_config(&sig_cfg) != ESP_OK) {
        return false;
    }

    if (p->cfg.pon_gpio >= 0) {
        const gpio_config_t pon_cfg = {
            .pin_bit_mask = 1ULL << p->cfg.pon_gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&pon_cfg) != ESP_OK) {
            return false;
        }
        // pulse off→on so the receiver AGC restarts from a defined state
        gpio_set_level((gpio_num_t)p->cfg.pon_gpio, p->cfg.pon_on_level ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level((gpio_num_t)p->cfg.pon_gpio, p->cfg.pon_on_level ? 1 : 0);
    }

    decoder_reset(p);
    xQueueReset(p->slot_queue);
    STATUS_LOCK(p);
    memset(&p->status, 0, sizeof(p->status));
    p->status.state = DCF77P_STATE_ACQUIRING_PHASE;
    STATUS_UNLOCK(p);

    p->running = true;
    TaskHandle_t th = NULL;
    if (xTaskCreatePinnedToCore(dcf77p_task_fn, "dcf77p", p->cfg.task_stack, p,
                                p->cfg.task_priority, &th, p->cfg.task_core) != pdPASS) {
        p->running = false;
        return false;
    }
    p->task = th;

    if (esp_timer_start_periodic(p->sampler, 1000) != ESP_OK) { // 1 kHz
        dcf77p_stop(p);
        return false;
    }
    return true;
}

void dcf77p_stop(dcf77p_t *p)
{
    if (p == NULL || !p->running) {
        return;
    }
    esp_timer_stop(p->sampler);
    p->running = false;
    while (p->task != NULL) { // task self-deletes within one poll interval
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (p->cfg.pon_gpio >= 0) {
        // hold the receiver off while disabled (FR-DCF-002)
        gpio_set_level((gpio_num_t)p->cfg.pon_gpio, p->cfg.pon_on_level ? 0 : 1);
    }
    STATUS_LOCK(p);
    p->status.state = DCF77P_STATE_OFF;
    STATUS_UNLOCK(p);
}

void dcf77p_get_status(dcf77p_t *p, dcf77p_status_t *out)
{
    if (p == NULL || out == NULL) {
        return;
    }
    STATUS_LOCK(p);
    *out = p->status;
    STATUS_UNLOCK(p);
}
