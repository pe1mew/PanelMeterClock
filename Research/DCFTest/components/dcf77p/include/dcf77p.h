/**
 * @file dcf77p.h
 * @brief DCF77 phase-locked exhaustive decoder, ESP-IDF component, FreeRTOS based.
 *
 * C port of Udo Klein's "blinkenlight" DCF77 decoder
 * (www.blinkenlight.net, https://github.com/udoklein/dcf77; reference:
 * Documentation/src/blinkenLightDC77DecoderAll.c) for the ESP32-S3.
 *
 * Unlike the simple edge-timing decoder (components/dcf77), this decoder
 * never looks at individual edges. It samples the demodulated signal at
 * 1 kHz, integrates it into 100 phase bins of 10 ms, and convolves the bins
 * with the expected pulse shape to find the second boundary (phase lock).
 * Each second's 220 ms window is then classified into sync-mark / 0 / 1 /
 * undefined; a 60-bin scoring scheme localises the sync mark (second lock),
 * and every calendar field (minute, hour, day, weekday, month, year) is
 * decoded *exhaustively*: every candidate value is scored each minute by
 * Hamming distance against the received bits, with the per-field bin rings
 * advanced in lock-step with the predicted time. The maximum-scoring bin
 * wins; lock quality = max − runner-up. This decodes through noise levels
 * where individual pulses are unreadable.
 *
 * RTOS structure (mirrors components/dcf77): a 1 kHz esp_timer callback
 * does sampling only and forwards one majority bit per 10 ms through a
 * FreeRTOS queue to a dedicated decoder task (default: PRO core, priority
 * 7 per PMC-STD-001 §4). All callbacks run in the decoder task context.
 *
 * Default pins per PMC-HTD-001 §3/§7: time-code GPIO 11 (input, internal
 * pull-up, IC-HW-005), receiver enable (PON) GPIO 12 (output, low = on,
 * IC-HW-006). Signal polarity configurable (FR-DCF-004).
 *
 * Copyright 2013 Udo Klein (original decoder, GPL-3.0-or-later)
 * Copyright 2026 Remko Welling (ESP-IDF port)
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
 * NOTE: linking this component makes the combined firmware subject to the
 * GPL-3.0. The sibling component `dcf77` (edge-timing decoder) carries no
 * such obligation.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-second tick classification (encoding as in Klein's decoder).
 */
typedef enum {
    DCF77P_TICK_SYNC      = 0, /**< No pulse — the missing 59th-second mark. */
    DCF77P_TICK_UNDEFINED = 1, /**< Pulse shape matches neither bit. */
    DCF77P_TICK_ZERO      = 2, /**< ~100 ms pulse. */
    DCF77P_TICK_ONE       = 3, /**< ~200 ms pulse. */
} dcf77p_tick_class_t;

/**
 * @brief Decoded time snapshot.
 *
 * Field values are plain integers; 0xFF means the exhaustive decoder has
 * not (yet) accumulated enough evidence for that field. @ref valid is set
 * once every field is determined and plausible; only then is @ref utc_epoch
 * meaningful.
 */
typedef struct {
    uint8_t second;   /**< 0–60 (60 only during a leap second); 0xFF = no second lock. */
    uint8_t minute;   /**< 0–59 or 0xFF. */
    uint8_t hour;     /**< 0–23 or 0xFF. */
    uint8_t day;      /**< 1–31 or 0xFF. */
    uint8_t weekday;  /**< 1 = Monday … 7 = Sunday, or 0xFF. */
    uint8_t month;    /**< 1–12 or 0xFF. */
    uint8_t year2;    /**< 0–99 (year − 2000) or 0xFF. */
    bool    cest;                  /**< true = CEST (UTC+2), false = CET (UTC+1). */
    bool    dst_change_announced;  /**< A1 flag (majority-integrated). */
    bool    leap_second_announced; /**< A2 flag (majority-integrated). */
    bool    backup_antenna;        /**< R flag. */
    bool    valid;     /**< All fields determined and mutually plausible. */
    int64_t utc_epoch; /**< Seconds since 1970-01-01 UTC; only when valid (FR-DCF-009). */
} dcf77p_time_t;

/** @brief Lock quality of one decoder stage: winner vs. runner-up score. */
typedef struct {
    uint8_t lock;  /**< Score of the best candidate. */
    uint8_t noise; /**< Score of the second-best candidate. */
} dcf77p_lq_t;

/** @brief Quality of all decoder stages (Klein's clock_quality_t). */
typedef struct {
    uint32_t    phase_lock;  /**< Phase convolution maximum. */
    uint32_t    phase_noise; /**< Convolution at 200 ms phase offset. */
    dcf77p_lq_t second;      /**< Sync-mark binning. */
    dcf77p_lq_t minute, hour, day, weekday, month, year;
    uint8_t     summertime_q, tz_change_q, leap_q; /**< Flag integrator magnitudes. */
} dcf77p_quality_t;

/** @brief Acquisition state, in increasing order of lock depth. */
typedef enum {
    DCF77P_STATE_OFF,             /**< Not started or stopped (receiver held off). */
    DCF77P_STATE_ACQUIRING_PHASE, /**< Sampling; no usable phase lock yet. */
    DCF77P_STATE_ACQUIRING_SECOND,/**< Phase locked; sync mark not yet localised. */
    DCF77P_STATE_DECODING,        /**< Second locked; calendar fields incomplete. */
    DCF77P_STATE_TIME_VALID,      /**< Full date/time determined and plausible. */
} dcf77p_state_t;

typedef enum {
    DCF77P_EVT_SECOND,       /**< Detected second boundary: time snapshot valid. */
    DCF77P_EVT_TICK,         /**< A second's mark was classified: tick/tick_second valid. */
    DCF77P_EVT_STATE_CHANGE, /**< old_state/new_state valid. */
} dcf77p_event_type_t;

/** @brief Event passed to the event callback (decoder task context). */
typedef struct {
    dcf77p_event_type_t type;
    int64_t             t_us;        /**< Sample timestamp that triggered the event. */
    dcf77p_time_t       time;        /**< EVT_SECOND: time at this boundary. */
    dcf77p_tick_class_t tick;        /**< EVT_TICK: classification result. */
    uint8_t             tick_second; /**< EVT_TICK: second this mark belongs to, 0xFF unknown. */
    dcf77p_state_t      old_state;   /**< EVT_STATE_CHANGE. */
    dcf77p_state_t      new_state;   /**< EVT_STATE_CHANGE. */
} dcf77p_event_t;

typedef void (*dcf77p_event_cb_t)(const dcf77p_event_t *evt, void *ctx);

/**
 * @brief Per-second callback (FR-DCF-012), decoder task context.
 *
 * Fired at every phase-predicted second boundary — derived from the phase
 * lock, not from individual edges, so it keeps ticking through noise.
 *
 * @param t_us   Timestamp of the boundary sample (esp_timer time base).
 * @param second Second within the minute (0–60), or -1 without second lock.
 * @param ctx    User context pointer from the configuration.
 */
typedef void (*dcf77p_tick_cb_t)(int64_t t_us, int16_t second, void *ctx);

/** @brief Snapshot of decoder state and statistics. */
typedef struct {
    dcf77p_state_t   state;
    dcf77p_quality_t quality;
    dcf77p_time_t    last_time;     /**< Time at the most recent second boundary. */
    uint32_t         seconds;       /**< Second boundaries detected. */
    uint32_t         ticks[4];      /**< Classified marks, indexed by dcf77p_tick_class_t. */
    uint32_t         slots_dropped; /**< 10 ms slots lost on queue overflow. */
} dcf77p_status_t;

/** @brief Configuration. Obtain defaults with dcf77p_config_default(). */
typedef struct {
    int8_t  signal_gpio;     /**< Time-code input. Default 11 (IC-HW-005). */
    int8_t  pon_gpio;        /**< Receiver enable output, -1 = not wired. Default 12 (IC-HW-006). */
    uint8_t pon_on_level;    /**< GPIO level that powers the receiver ON. Default 0. */
    bool    signal_inverted; /**< false: carrier-reduction mark = HIGH (FR-DCF-004). */
    bool    internal_pullup; /**< Enable internal pull-up (IC-HW-005). */

    uint32_t    task_stack;    /**< Decoder task stack in bytes. */
    UBaseType_t task_priority; /**< Decoder task priority. Default 7 (PMC-STD-001 §4). */
    BaseType_t  task_core;     /**< Decoder task core. Default 0 = PRO (PMC-STD-001 §4). */

    dcf77p_event_cb_t event_cb; /**< Optional event callback. */
    dcf77p_tick_cb_t  tick_cb;  /**< Optional per-second tick callback. */
    void             *cb_ctx;   /**< Passed to both callbacks. */
} dcf77p_config_t;

/** @brief Internal: working time representation (BCD fields, Klein's time_data). */
typedef struct {
    uint8_t second;      /* 0..60, 0xFF unknown */
    uint8_t minute_bcd;  /* BCD or 0xFF */
    uint8_t hour_bcd;
    uint8_t day_bcd;
    uint8_t weekday_bcd;
    uint8_t month_bcd;
    uint8_t year_bcd;
    bool    uses_summertime;
    bool    uses_backup_antenna;
    bool    timezone_change_scheduled;
    bool    leap_second_scheduled;
} dcf77p_ktime_t;

/** @brief Internal: generic exhaustive-decoder bin ring (Klein's Hamming bins). */
typedef struct {
    uint8_t *data;
    uint8_t  n;
    uint8_t  tick;
    uint8_t  max, noise_max, max_index;
} dcf77p_kbins_t;

/**
 * @brief Decoder instance. Allocate one per receiver (statically — it is
 * a few hundred bytes). All members are managed by the module — treat as
 * opaque.
 */
typedef struct {
    dcf77p_config_t cfg;
    QueueHandle_t   slot_queue;
    volatile TaskHandle_t task;
    volatile bool   running;
    esp_timer_handle_t sampler;
    portMUX_TYPE    status_mux;
    dcf77p_status_t status;

    /* sampler state (esp_timer callback context only) */
    uint8_t sample_acc, sample_cnt;

    /* demodulator: 100 phase bins of 10 ms */
    uint16_t pdata[100];
    uint8_t  ptick;
    uint32_t pmax, pnoise;
    uint8_t  pmax_index;
    uint8_t  bins_to_process;
    uint8_t  dec_count, dec_data;

    /* exhaustive field decoders */
    uint8_t        sec_data[60], min_data[60], hour_data[24], day_data[31],
                   wday_data[7], month_data[12], year_data[100];
    dcf77p_kbins_t sec_bins, min_bins, hour_bins, day_bins,
                   wday_bins, month_bins, year_bins;
    uint8_t        min_acc, hour_acc, day_acc, wday_acc, month_acc, year_acc;

    /* flag decoder (signed integrators) */
    int8_t flag_summertime, flag_tz_change, flag_leap, flag_date_parity;
    bool   flag_backup;

    /* clock controller */
    dcf77p_ktime_t decoded;
    uint8_t        leap_second;
} dcf77p_t;

/** @brief Default configuration: PMC-HTD-001 pinning, STD task placement. */
dcf77p_config_t dcf77p_config_default(void);

/**
 * @brief Initialise an instance. Call once before any other function.
 *
 * Allocates the slot queue and the 1 kHz sampler timer; touches no GPIO.
 *
 * @return true on success.
 */
bool dcf77p_init(dcf77p_t *p, const dcf77p_config_t *cfg);

/**
 * @brief Power the receiver, start the 1 kHz sampler and the decoder task.
 *
 * The PON line is pulsed off→on for a clean AGC restart. All decoder bins
 * are reset, so a restart begins a fresh acquisition.
 *
 * @return true on success.
 */
bool dcf77p_start(dcf77p_t *p);

/**
 * @brief Stop sampling and decoding; hold the receiver off (FR-DCF-002).
 *
 * The instance may be restarted with dcf77p_start().
 */
void dcf77p_stop(dcf77p_t *p);

/** @brief Copy a consistent snapshot of state, quality and statistics. */
void dcf77p_get_status(dcf77p_t *p, dcf77p_status_t *out);

#ifdef __cplusplus
}
#endif
