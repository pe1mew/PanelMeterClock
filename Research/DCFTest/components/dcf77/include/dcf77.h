/**
 * @file dcf77.h
 * @brief DCF77 time-code receiver/decoder, ESP-IDF component, FreeRTOS based.
 *
 * Simple edge-timing decoder per PMC-STD-001 §5.10: a GPIO edge interrupt
 * timestamps every transition of the demodulated time-code signal; a
 * dedicated FreeRTOS task filters glitches, measures pulse widths,
 * classifies bits (≈100 ms = 0, ≈200 ms = 1), assembles the 59-bit minute
 * frame and validates it (fixed bits, zone bits, even parity, BCD range,
 * day-in-month and weekday cross-checks — FR-DCF-006).
 *
 * Decoded frames are reported through an event callback; the leading edge
 * of every second mark is reported through a tick callback (FR-DCF-012).
 * A decoded time is reported as confirmed once two consecutive frames are
 * consistent (FR-DCF-007). Both callbacks run in the decoder task context.
 *
 * Frame layout and tick classification after Udo Klein's blinkenlight
 * decoder (www.blinkenlight.net; see
 * Documentation/src/blinkenLightDC77DecoderAll.c).
 *
 * Default pin assignment per PMC-HTD-001 §3/§7: time-code GPIO 11 (input,
 * internal pull-up, IC-HW-005), receiver enable (PON) GPIO 12 (output,
 * low = receiver on, IC-HW-006). Signal polarity is configurable
 * (FR-DCF-004).
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of data bits in a DCF77 minute frame (seconds 0–58). */
#define DCF77_FRAME_BITS 59

/**
 * @brief Decoded DCF77 time (FR-DCF-005).
 *
 * The broadcast time is the civil CET/CEST time valid at the minute mark
 * that *ends* the frame — i.e. at the leading edge of the second-0 mark on
 * which DCF77_EVT_FRAME_OK is emitted.
 */
typedef struct {
    uint16_t year;     /**< Full year, 2000–2099. */
    uint8_t  month;    /**< 1–12. */
    uint8_t  day;      /**< 1–31. */
    uint8_t  weekday;  /**< 1 = Monday … 7 = Sunday. */
    uint8_t  hour;     /**< 0–23, civil CET/CEST time. */
    uint8_t  minute;   /**< 0–59. */
    bool     cest;     /**< true = CEST (UTC+2), false = CET (UTC+1). */
    bool     dst_change_announced;  /**< A1: CET↔CEST change within the next hour. */
    bool     leap_second_announced; /**< A2: leap second within the next hour. */
    bool     call_bit;              /**< R: transmitter abnormality / backup antenna. */
    int64_t  utc_epoch; /**< Seconds since 1970-01-01 00:00:00 UTC (FR-DCF-009). */
} dcf77_time_t;

/** @brief Decoder events, in rough order of appearance during acquisition. */
typedef enum {
    DCF77_EVT_NO_SIGNAL,     /**< No edges for cfg.signal_lost_ms. */
    DCF77_EVT_BIT,           /**< Second mark classified; bit_*, measured_ms valid. */
    DCF77_EVT_PULSE_REJECT,  /**< Mark width outside both bit windows; measured_ms valid. */
    DCF77_EVT_RESYNC,        /**< Mark period implausible; bit alignment lost; reason valid. */
    DCF77_EVT_MINUTE_MARK,   /**< Missing 59th mark detected; this edge is second 0. */
    DCF77_EVT_FRAME_OK,      /**< Frame decoded and valid; time, bits valid. */
    DCF77_EVT_FRAME_REJECT,  /**< Frame failed validation; reason, bits valid. */
    DCF77_EVT_TIME_CONFIRMED,/**< Two consecutive consistent frames; time valid (FR-DCF-007). */
} dcf77_event_type_t;

/** @brief Event data passed to the event callback (decoder task context). */
typedef struct {
    dcf77_event_type_t type;
    int64_t       t_us;        /**< esp_timer timestamp of the causing edge. */
    int16_t       bit_index;   /**< BIT: 0–58, or -1 while not minute-aligned. */
    int8_t        bit_value;   /**< BIT: 0 or 1. */
    uint16_t      measured_ms; /**< Mark width (BIT/PULSE_REJECT) or mark period (RESYNC/MINUTE_MARK). */
    uint8_t       bit_count;   /**< FRAME_*: bits collected this minute. */
    const uint8_t *bits;       /**< FRAME_*: bit values [0..bit_count); 0xFF = unreadable. Valid only during the callback. */
    const char    *reason;     /**< FRAME_REJECT/RESYNC: human-readable cause. */
    dcf77_time_t  time;        /**< FRAME_OK / TIME_CONFIRMED. */
} dcf77_event_t;

typedef void (*dcf77_event_cb_t)(const dcf77_event_t *evt, void *ctx);

/**
 * @brief Per-second tick callback (FR-DCF-012), decoder task context.
 *
 * Fired on the accepted leading edge of every second mark — the on-time
 * marker. Acceptance is delayed by the glitch filter, so the call arrives
 * up to ~(glitch_ms + poll interval) after the physical edge; use @p t_us
 * to back-date.
 *
 * @param t_us   esp_timer timestamp of the mark's leading edge.
 * @param second Second within the minute (0–58), or -1 while not aligned.
 * @param ctx    User context pointer from the configuration.
 */
typedef void (*dcf77_tick_cb_t)(int64_t t_us, int16_t second, void *ctx);

/** @brief Acquisition state for status reporting. */
typedef enum {
    DCF77_STATE_OFF,        /**< Not started, or stopped (receiver held off, FR-DCF-002). */
    DCF77_STATE_NO_SIGNAL,  /**< Running but no edges seen recently. */
    DCF77_STATE_SYNCING,    /**< Pulses arriving; waiting for a minute mark. */
    DCF77_STATE_COLLECTING, /**< Bit-aligned; filling the frame. */
    DCF77_STATE_LOCKED,     /**< Consecutive frames confirming (FR-DCF-007). */
} dcf77_state_t;

/** @brief Snapshot of decoder state and statistics. */
typedef struct {
    dcf77_state_t state;
    int16_t  second;          /**< Current second within the minute, -1 unknown. */
    uint32_t edges;           /**< Raw edges received from the ISR. */
    uint32_t glitches;        /**< Spikes removed by the glitch filter. */
    uint32_t bits_ok;         /**< Marks classified as a valid 0 or 1. */
    uint32_t pulses_rejected; /**< Marks with implausible width. */
    uint32_t frames_ok;
    uint32_t frames_rejected;
    uint32_t confirmations;   /**< TIME_CONFIRMED events so far. */
    bool     time_valid;      /**< last_time holds a confirmed time. */
    dcf77_time_t last_time;   /**< Last confirmed time (valid at its minute mark). */
} dcf77_status_t;

/** @brief Configuration. Obtain defaults with dcf77_config_default(). */
typedef struct {
    int8_t  signal_gpio;     /**< Time-code input. Default 11 (IC-HW-005). */
    int8_t  pon_gpio;        /**< Receiver enable output, -1 = not wired. Default 12 (IC-HW-006). */
    uint8_t pon_on_level;    /**< GPIO level that powers the receiver ON. Default 0 (module PDN/P1: low = on). */
    bool    signal_inverted; /**< false: carrier-reduction mark = HIGH; true: mark = LOW (FR-DCF-004). */
    bool    internal_pullup; /**< Enable internal pull-up for the open-collector output (IC-HW-005). */

    uint16_t glitch_ms;      /**< Level changes shorter than this are filtered as noise. */
    uint16_t zero_min_ms;    /**< Bit-0 mark width window (nominal 100 ms). */
    uint16_t zero_max_ms;
    uint16_t one_min_ms;     /**< Bit-1 mark width window (nominal 200 ms). */
    uint16_t one_max_ms;
    uint16_t period_min_ms;  /**< Mark-to-mark window for a normal second (nominal 1000 ms). */
    uint16_t period_max_ms;
    uint16_t minute_min_ms;  /**< Mark-to-mark window across the missing 59th mark (nominal 2000 ms). */
    uint16_t minute_max_ms;
    uint32_t signal_lost_ms; /**< Watchdog delay for DCF77_EVT_NO_SIGNAL. */

    uint32_t    task_stack;    /**< Decoder task stack in bytes. */
    UBaseType_t task_priority; /**< Decoder task priority. Default 7 (PMC-STD-001 §4). */
    BaseType_t  task_core;     /**< Decoder task core. Default 0 = PRO (PMC-STD-001 §4). */

    dcf77_event_cb_t event_cb; /**< Optional event callback. */
    dcf77_tick_cb_t  tick_cb;  /**< Optional per-second tick callback. */
    void            *cb_ctx;   /**< Passed to both callbacks. */
} dcf77_config_t;

/**
 * @brief Decoder instance. Allocate one per receiver.
 *
 * All members are managed by the module — treat as opaque.
 */
typedef struct {
    dcf77_config_t cfg;
    QueueHandle_t  edge_queue;
    volatile TaskHandle_t task;
    volatile bool  running;
    portMUX_TYPE   status_mux;
    dcf77_status_t status;
} dcf77_t;

/** @brief Default configuration: PMC-HTD-001 pinning and PTB nominal timing. */
dcf77_config_t dcf77_config_default(void);

/**
 * @brief Initialise an instance. Call once before any other function.
 *
 * Allocates the edge queue; does not touch any GPIO yet.
 *
 * @param dcf Pointer to an uninitialised dcf77_t instance.
 * @param cfg Configuration to copy; must not be NULL.
 * @return true on success, false if the queue could not be created.
 */
bool dcf77_init(dcf77_t *dcf, const dcf77_config_t *cfg);

/**
 * @brief Power the receiver, attach the edge interrupt, start the decoder task.
 *
 * Installs the shared GPIO ISR service if not installed yet (flags 0), then
 * registers the per-pin handler. The PON line is pulsed off→on to give the
 * receiver AGC a clean restart.
 *
 * @param dcf Initialised instance.
 * @return true on success, false if GPIO setup or task creation failed.
 */
bool dcf77_start(dcf77_t *dcf);

/**
 * @brief Stop decoding and hold the receiver off (FR-DCF-002).
 *
 * Disables the pin interrupt, removes the ISR handler, terminates the
 * decoder task and drives the PON line to the off level. The instance may
 * be restarted with dcf77_start().
 *
 * @param dcf Started instance.
 */
void dcf77_stop(dcf77_t *dcf);

/**
 * @brief Copy a consistent snapshot of decoder state and statistics.
 *
 * Thread-safe; may be called from any task.
 *
 * @param dcf Initialised instance.
 * @param out Destination for the snapshot.
 */
void dcf77_get_status(dcf77_t *dcf, dcf77_status_t *out);

#ifdef __cplusplus
}
#endif
