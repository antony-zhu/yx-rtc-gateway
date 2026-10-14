#pragma once

/*
 * YMZL/GS200 application-layer frame filtering.
 *
 * The filter deliberately deals in protocol fields rather than a concatenated
 * action string.  For a CMD=0x12 frame the operation area is:
 *
 *     opcode | command_code (GS200 DPID) | operation payload
 *
 * `opcode` and `command_code` are therefore independent match fields.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_ymzl_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of rules retained by the application-layer filter. */
#define GW_DONGLE_FILTER_MAX_RULES              64U

/** Maximum number of independent key/value runtime states. */
#define GW_DONGLE_FILTER_STATE_CAPACITY         51U

/** Length of a GS200 device identifier in a CMD=0x12 frame. */
#define GW_DONGLE_FILTER_DEVICE_ID_LEN          6U

/** Largest operation payload in a valid YMZL frame. */
#define GW_DONGLE_FILTER_MAX_OPERATION_PAYLOAD  \
    (GW_YMZL_MAX_DATA_LEN - 16U)

/** Default compatibility window used by the Lua implementation. */
#define GW_DONGLE_FILTER_DEFAULT_DEDUP_WINDOW_MS 9000U

/** Retry interval for a delayed report when the publish callback fails. */
#define GW_DONGLE_FILTER_RETRY_INTERVAL_MS      1000U

/** Maximum accepted debounce delay. */
#define GW_DONGLE_FILTER_MAX_DELAY_MS           (24U * 60U * 60U * 1000U)

/** Report behavior after a rule matches. */
typedef enum {
    GW_DONGLE_FILTER_DROP = 0,
    GW_DONGLE_FILTER_REALTIME,
    GW_DONGLE_FILTER_DEDUP,
    GW_DONGLE_FILTER_CHANGE,
    GW_DONGLE_FILTER_CHANGE_DELAY,
} gw_dongle_filter_type_t;

/** Match fields for one rule. A field is ignored when its `has_*` flag is false. */
typedef struct {
    bool has_cmd;                         /*!< Outer YMZL CMD byte. */
    uint8_t cmd;
    bool has_data_len;                    /*!< YMZL DATA length byte. */
    uint8_t data_len;

    bool has_device_id;                   /*!< CMD=0x12 DATA[1..6]. */
    uint8_t device_id[GW_DONGLE_FILTER_DEVICE_ID_LEN];
    bool has_source_addr;                 /*!< CMD=0x12 source address, big endian. */
    uint16_t source_addr;
    bool has_publish_addr;                /*!< CMD=0x12 publish/target address. */
    uint16_t publish_addr;
    bool has_area;                        /*!< CMD=0x12 area/network ID. */
    uint16_t area;

    bool has_opcode;                      /*!< Operation first byte. */
    uint8_t opcode;
    bool has_opcode_mask;                 /*!< Optional mask; false means exact byte. */
    uint8_t opcode_mask;
    bool has_command_code;                /*!< Operation second byte (GS200 DPID). */
    uint8_t command_code;
    bool require_single_byte_opcode;      /*!< Match by opcode and ignore command_code. */
} gw_dongle_filter_match_t;

/** Report parameters attached to a rule or a default policy. */
typedef struct {
    gw_dongle_filter_type_t type;
    uint32_t window_ms;                   /*!< DEDUP window; zero uses the 9 s default. */
    uint32_t delay_ms;                    /*!< CHANGE_DELAY debounce delay. */
} gw_dongle_filter_policy_t;

/** One ordered, first-match-wins filter rule. */
typedef struct {
    bool enabled;
    gw_dongle_filter_match_t match;
    gw_dongle_filter_policy_t policy;
} gw_dongle_filter_rule_t;

/** Complete bounded filter configuration. */
typedef struct {
    gw_dongle_filter_rule_t rules[GW_DONGLE_FILTER_MAX_RULES];
    size_t rule_count;
    gw_dongle_filter_policy_t cmd12_default;
    gw_dongle_filter_policy_t other_cmd_default;
} gw_dongle_filter_config_t;

/** Snapshot counters for diagnostics. */
typedef struct {
    uint32_t matched_count;
    uint32_t dropped_count;
    uint32_t realtime_count;
    uint32_t dedup_suppressed_count;
    uint32_t unchanged_count;
    uint32_t delayed_pending_count;
    uint32_t delayed_emit_count;
    uint32_t state_eviction_count;
} gw_dongle_filter_status_t;

/** Callback used to publish a frame selected by the filter. */
typedef esp_err_t (*gw_dongle_filter_emit_cb_t)(const uint8_t *frame,
                                                size_t frame_len,
                                                void *ctx);

/**
 * @brief Initialize the singleton filter with the built-in Lua-compatible table.
 *
 * The call is idempotent. Runtime states are cleared only on the first call or
 * when `gw_dongle_filter_reset_runtime()` is explicitly requested.
 */
esp_err_t gw_dongle_filter_init(void);

/**
 * @brief Produce the built-in table without changing the active configuration.
 */
esp_err_t gw_dongle_filter_get_default_config(gw_dongle_filter_config_t *out);

/**
 * @brief Atomically replace the active bounded configuration.
 *
 * The caller owns its input buffer; the filter copies it. This API is intended
 * to be called by the application worker (or while the bridge is stopped), so
 * frame processing and configuration replacement are not concurrent.
 */
esp_err_t gw_dongle_filter_set_config(const gw_dongle_filter_config_t *config);

/** @brief Copy the active configuration to `out`. */
esp_err_t gw_dongle_filter_get_config(gw_dongle_filter_config_t *out);

/** @brief Clear dedup/change/debounce state while keeping the active table. */
esp_err_t gw_dongle_filter_reset_runtime(void);

/**
 * @brief Evaluate one complete YMZL frame.
 *
 * `now_ms` must be a monotonic millisecond timestamp. Immediate policies call
 * `emit` synchronously. CHANGE_DELAY stores the latest frame and emits it from
 * `gw_dongle_filter_poll()` after the quiet interval; no FreeRTOS task is made
 * per rule.
 */
esp_err_t gw_dongle_filter_process(const uint8_t *frame,
                                   size_t frame_len,
                                   uint64_t now_ms,
                                   gw_dongle_filter_emit_cb_t emit,
                                   void *ctx);

/**
 * @brief Emit due CHANGE_DELAY frames from the singleton worker context.
 */
esp_err_t gw_dongle_filter_poll(uint64_t now_ms,
                                gw_dongle_filter_emit_cb_t emit,
                                void *ctx);

/** @brief Get filter counters. */
esp_err_t gw_dongle_filter_get_status(gw_dongle_filter_status_t *out);

#ifdef __cplusplus
}
#endif
