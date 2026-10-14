/**
 * @file gw_dongle_filter.c
 * @brief Bounded YMZL/GS200 application-layer filter and report policy engine.
 */

#include "gw_dongle_filter.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw_ymzl_protocol.h"

#define GW_DONGLE_FILTER_CMD12             0x12U
#define GW_DONGLE_FILTER_CMD12_FIXED_LEN   15U
#define GW_DONGLE_FILTER_OPERATION_OFFSET  (4U + GW_DONGLE_FILTER_CMD12_FIXED_LEN)
#define GW_DONGLE_FILTER_MAX_KEY_BYTES     GW_YMZL_MAX_FRAME_LEN
#define GW_DONGLE_FILTER_DEFAULT_CMD_SLOT  ((uint16_t)GW_DONGLE_FILTER_MAX_RULES)
#define GW_DONGLE_FILTER_DEFAULT_OTHER_SLOT \
    ((uint16_t)(GW_DONGLE_FILTER_MAX_RULES + 1U))

typedef struct {
    const uint8_t *frame;
    size_t frame_len;
    uint8_t cmd;
    uint8_t data_len;
    const uint8_t *data;
    bool cmd12;
    uint8_t device_id[GW_DONGLE_FILTER_DEVICE_ID_LEN];
    uint16_t source_addr;
    uint16_t publish_addr;
    uint16_t area;
    const uint8_t *operation;
    size_t operation_len;
    uint8_t opcode;
    bool has_command_code;
    uint8_t command_code;
    const uint8_t *value;
    size_t value_len;
} gw_dongle_filter_frame_info_t;

typedef struct {
    bool valid;
    bool has_value;
    bool pending;
    uint16_t slot;
    uint8_t key_len;
    uint8_t key[GW_DONGLE_FILTER_MAX_KEY_BYTES];
    uint8_t value_len;
    uint8_t value[GW_YMZL_MAX_DATA_LEN];
    uint64_t last_ms;
    uint64_t deadline_ms;
    uint8_t pending_len;
    uint8_t pending_frame[GW_YMZL_MAX_FRAME_LEN];
    uint32_t lru;
} gw_dongle_filter_state_t;

static gw_dongle_filter_config_t s_config;
static gw_dongle_filter_state_t s_states[GW_DONGLE_FILTER_STATE_CAPACITY];
static gw_dongle_filter_status_t s_status;
static uint32_t s_lru_sequence;
static bool s_initialized;
static portMUX_TYPE s_filter_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_filter_mutex;
static StaticSemaphore_t s_filter_mutex_buffer;

static esp_err_t ensure_filter_mutex(void)
{
    if (s_filter_mutex == NULL) {
        s_filter_mutex = xSemaphoreCreateMutexStatic(&s_filter_mutex_buffer);
    }
    return s_filter_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t lock_filter(void)
{
    esp_err_t err = ensure_filter_mutex();
    if (err != ESP_OK) {
        return err;
    }
    return xSemaphoreTake(s_filter_mutex, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

static void unlock_filter(void)
{
    if (s_filter_mutex != NULL) {
        (void)xSemaphoreGive(s_filter_mutex);
    }
}

static void status_increment(uint32_t *counter)
{
    portENTER_CRITICAL(&s_filter_lock);
    ++(*counter);
    portEXIT_CRITICAL(&s_filter_lock);
}

static void status_set_pending_count(uint32_t value)
{
    portENTER_CRITICAL(&s_filter_lock);
    s_status.delayed_pending_count = value;
    portEXIT_CRITICAL(&s_filter_lock);
}

static bool policy_type_valid(gw_dongle_filter_type_t type)
{
    return type >= GW_DONGLE_FILTER_DROP &&
           type <= GW_DONGLE_FILTER_CHANGE_DELAY;
}

static esp_err_t validate_policy(const gw_dongle_filter_policy_t *policy)
{
    if (policy == NULL || !policy_type_valid(policy->type)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (policy->type == GW_DONGLE_FILTER_DEDUP &&
        policy->window_ms > GW_DONGLE_FILTER_MAX_DELAY_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (policy->type == GW_DONGLE_FILTER_CHANGE_DELAY &&
        policy->delay_ms > GW_DONGLE_FILTER_MAX_DELAY_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t validate_match(const gw_dongle_filter_match_t *match)
{
    if (match == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (match->require_single_byte_opcode &&
        (!match->has_opcode || match->has_command_code)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (match->has_opcode_mask && (!match->has_opcode || match->opcode_mask == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (match->has_cmd && match->cmd != GW_DONGLE_FILTER_CMD12 &&
        (match->has_device_id || match->has_source_addr || match->has_publish_addr ||
         match->has_area || match->has_opcode || match->has_command_code ||
         match->require_single_byte_opcode)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t validate_config(const gw_dongle_filter_config_t *config)
{
    if (config == NULL || config->rule_count > GW_DONGLE_FILTER_MAX_RULES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (validate_policy(&config->cmd12_default) != ESP_OK ||
        validate_policy(&config->other_cmd_default) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < config->rule_count; ++i) {
        if (validate_match(&config->rules[i].match) != ESP_OK ||
            validate_policy(&config->rules[i].policy) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static gw_dongle_filter_policy_t normalized_policy(gw_dongle_filter_policy_t policy)
{
    if (policy.type == GW_DONGLE_FILTER_DEDUP && policy.window_ms == 0U) {
        policy.window_ms = GW_DONGLE_FILTER_DEFAULT_DEDUP_WINDOW_MS;
    }
    return policy;
}

static void add_default_rule(gw_dongle_filter_config_t *config,
                             uint8_t opcode,
                             bool has_command_code,
                             uint8_t command_code)
{
    if (config == NULL || config->rule_count >= GW_DONGLE_FILTER_MAX_RULES) {
        return;
    }

    gw_dongle_filter_rule_t *rule = &config->rules[config->rule_count++];
    memset(rule, 0, sizeof(*rule));
    rule->enabled = true;
    rule->match.has_cmd = true;
    rule->match.cmd = GW_DONGLE_FILTER_CMD12;
    rule->match.has_opcode = true;
    rule->match.opcode = opcode;
    rule->match.has_command_code = has_command_code;
    rule->match.command_code = command_code;
    rule->match.require_single_byte_opcode = !has_command_code;
    rule->policy.type = GW_DONGLE_FILTER_DEDUP;
    rule->policy.window_ms = GW_DONGLE_FILTER_DEFAULT_DEDUP_WINDOW_MS;
}

static void build_default_config(gw_dongle_filter_config_t *config)
{
    static const uint8_t status_dpid[] = {
        0x10U, 0x20U, 0x30U, 0x40U, 0x15U,
        0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U,
        0x58U, 0x59U, 0x5AU, 0x5BU, 0x5CU, 0x5DU, 0x5EU, 0x5FU, 0x60U,
    };
    static const uint8_t other_pairs[][2] = {
        {0x47U, 0x00U}, {0x47U, 0x46U}, {0x47U, 0x23U},
        {0x3BU, 0x00U}, {0x3FU, 0x00U}, {0x4BU, 0x00U}, {0x55U, 0x10U},
    };

    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->cmd12_default.type = GW_DONGLE_FILTER_DROP;
    config->other_cmd_default.type = GW_DONGLE_FILTER_REALTIME;

    /* Lua's three one-byte actions: 0A, 0B and 01. */
    add_default_rule(config, 0x0AU, false, 0U);
    add_default_rule(config, 0x0BU, false, 0U);
    add_default_rule(config, 0x01U, false, 0U);

    for (size_t i = 0U; i < sizeof(status_dpid); ++i) {
        add_default_rule(config, 0x4FU, true, status_dpid[i]);
    }
    for (size_t i = 0U; i < sizeof(other_pairs) / sizeof(other_pairs[0]); ++i) {
        add_default_rule(config, other_pairs[i][0], true, other_pairs[i][1]);
    }
}

static void clear_runtime_states(void)
{
    memset(s_states, 0, sizeof(s_states));
    s_lru_sequence = 0U;
    status_set_pending_count(0U);
}

static esp_err_t ensure_initialized(void)
{
    return s_initialized ? ESP_OK : gw_dongle_filter_init();
}

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static esp_err_t parse_frame(const uint8_t *frame,
                             size_t frame_len,
                             gw_dongle_filter_frame_info_t *out)
{
    if (frame == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = gw_ymzl_frame_validate(frame, frame_len);
    if (err != ESP_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));
    out->frame = frame;
    out->frame_len = frame_len;
    out->cmd = frame[2];
    out->data_len = frame[3];
    out->data = frame + 4U;
    out->cmd12 = out->cmd == GW_DONGLE_FILTER_CMD12;

    if (!out->cmd12) {
        out->value = out->data;
        out->value_len = out->data_len;
        return ESP_OK;
    }

    /* CMD=0x12 has a 15-byte network header and at least one opcode byte. */
    if (out->data_len < GW_DONGLE_FILTER_CMD12_FIXED_LEN + 1U) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out->device_id,
           out->data + 1U,
           GW_DONGLE_FILTER_DEVICE_ID_LEN);
    out->source_addr = read_be16(out->data + 9U);
    out->publish_addr = read_be16(out->data + 11U);
    out->area = read_be16(out->data + 13U);
    out->operation = out->data + GW_DONGLE_FILTER_CMD12_FIXED_LEN;
    out->operation_len = out->data_len - GW_DONGLE_FILTER_CMD12_FIXED_LEN;
    out->opcode = out->operation[0];
    if (out->operation_len >= 2U) {
        out->has_command_code = true;
        out->command_code = out->operation[1];
        out->value = out->operation + 2U;
        out->value_len = out->operation_len - 2U;
    } else {
        out->value = out->operation + 1U;
        out->value_len = 0U;
    }
    return ESP_OK;
}

static bool match_requires_cmd12(const gw_dongle_filter_match_t *match)
{
    return match->has_device_id || match->has_source_addr || match->has_publish_addr ||
           match->has_area || match->has_opcode || match->has_command_code ||
           match->require_single_byte_opcode;
}

static bool match_frame(const gw_dongle_filter_match_t *match,
                        const gw_dongle_filter_frame_info_t *frame)
{
    if (match == NULL || frame == NULL) {
        return false;
    }
    if (match->has_cmd && frame->cmd != match->cmd) {
        return false;
    }
    if (match->has_data_len && frame->data_len != match->data_len) {
        return false;
    }
    if (match_requires_cmd12(match) && !frame->cmd12) {
        return false;
    }
    if (!frame->cmd12) {
        return true;
    }
    if (match->has_device_id &&
        memcmp(match->device_id,
               frame->device_id,
               GW_DONGLE_FILTER_DEVICE_ID_LEN) != 0) {
        return false;
    }
    if (match->has_source_addr && match->source_addr != frame->source_addr) {
        return false;
    }
    if (match->has_publish_addr && match->publish_addr != frame->publish_addr) {
        return false;
    }
    if (match->has_area && match->area != frame->area) {
        return false;
    }
    if (match->has_opcode) {
        uint8_t mask = match->has_opcode_mask ? match->opcode_mask : 0xFFU;
        if ((frame->opcode & mask) != (match->opcode & mask)) {
            return false;
        }
    }
    if (match->has_command_code &&
        (!frame->has_command_code || frame->command_code != match->command_code)) {
        return false;
    }
    return true;
}

static const gw_dongle_filter_policy_t *select_policy(
    const gw_dongle_filter_frame_info_t *frame,
    uint16_t *out_slot)
{
    for (size_t i = 0U; i < s_config.rule_count; ++i) {
        const gw_dongle_filter_rule_t *rule = &s_config.rules[i];
        if (rule->enabled && match_frame(&rule->match, frame)) {
            if (out_slot != NULL) {
                *out_slot = (uint16_t)i;
            }
            return &rule->policy;
        }
    }

    if (out_slot != NULL) {
        *out_slot = frame->cmd12 ? GW_DONGLE_FILTER_DEFAULT_CMD_SLOT :
                                   GW_DONGLE_FILTER_DEFAULT_OTHER_SLOT;
    }
    return frame->cmd12 ? &s_config.cmd12_default : &s_config.other_cmd_default;
}

static void append_key_byte(uint8_t *key, size_t *key_len, uint8_t value)
{
    if (*key_len < GW_DONGLE_FILTER_MAX_KEY_BYTES) {
        key[(*key_len)++] = value;
    }
}

static size_t build_state_key(const gw_dongle_filter_frame_info_t *frame,
                              const gw_dongle_filter_match_t *match,
                              gw_dongle_filter_type_t type,
                              uint16_t slot,
                              uint8_t key[GW_DONGLE_FILTER_MAX_KEY_BYTES])
{
    size_t key_len = 0U;
    append_key_byte(key, &key_len, (uint8_t)(slot & 0xFFU));
    append_key_byte(key, &key_len, (uint8_t)(slot >> 8U));
    append_key_byte(key, &key_len, frame->cmd);

    if (frame->cmd12) {
        /* Lua compatibility is exactly device-id + operation (not addresses). */
        for (size_t i = 0U; i < GW_DONGLE_FILTER_DEVICE_ID_LEN; ++i) {
            append_key_byte(key, &key_len, frame->device_id[i]);
        }
        if (type != GW_DONGLE_FILTER_DEDUP) {
            append_key_byte(key, &key_len, (uint8_t)(frame->source_addr >> 8U));
            append_key_byte(key, &key_len, (uint8_t)frame->source_addr);
            append_key_byte(key, &key_len, (uint8_t)(frame->publish_addr >> 8U));
            append_key_byte(key, &key_len, (uint8_t)frame->publish_addr);
            append_key_byte(key, &key_len, (uint8_t)(frame->area >> 8U));
            append_key_byte(key, &key_len, (uint8_t)frame->area);
        }
        bool include_command_code = frame->has_command_code &&
                                    (match == NULL || !match->require_single_byte_opcode);
        append_key_byte(key, &key_len, frame->opcode);
        append_key_byte(key, &key_len, include_command_code ? 1U : 0U);
        if (include_command_code) {
            append_key_byte(key, &key_len, frame->command_code);
        }
    } else {
        /* For non-CMD12 custom state policies, identical DATA is the identity. */
        for (size_t i = 0U; i < frame->data_len; ++i) {
            append_key_byte(key, &key_len, frame->data[i]);
        }
    }
    return key_len;
}

static void select_value(const gw_dongle_filter_frame_info_t *frame,
                         const gw_dongle_filter_match_t *match,
                         const uint8_t **out_value,
                         size_t *out_value_len)
{
    if (frame == NULL || out_value == NULL || out_value_len == NULL) {
        return;
    }
    *out_value = frame->value;
    *out_value_len = frame->value_len;
    if (frame->cmd12 && match != NULL && match->require_single_byte_opcode) {
        *out_value = frame->operation + 1U;
        *out_value_len = frame->operation_len - 1U;
    }
}

static uint32_t next_lru(void)
{
    ++s_lru_sequence;
    if (s_lru_sequence == 0U) {
        s_lru_sequence = 1U;
    }
    return s_lru_sequence;
}

static bool state_key_matches(const gw_dongle_filter_state_t *state,
                              uint16_t slot,
                              const uint8_t *key,
                              size_t key_len)
{
    return state->valid && state->slot == slot && state->key_len == key_len &&
           memcmp(state->key, key, key_len) == 0;
}

static size_t find_or_allocate_state(uint16_t slot,
                                     const uint8_t *key,
                                     size_t key_len)
{
    size_t oldest = 0U;
    uint32_t oldest_lru = UINT32_MAX;
    for (size_t i = 0U; i < GW_DONGLE_FILTER_STATE_CAPACITY; ++i) {
        if (state_key_matches(&s_states[i], slot, key, key_len)) {
            s_states[i].lru = next_lru();
            return i;
        }
        if (!s_states[i].valid) {
            oldest = i;
            oldest_lru = 0U;
            break;
        }
        if (s_states[i].lru < oldest_lru) {
            oldest_lru = s_states[i].lru;
            oldest = i;
        }
    }

    if (s_states[oldest].valid) {
        if (s_states[oldest].pending) {
            portENTER_CRITICAL(&s_filter_lock);
            if (s_status.delayed_pending_count > 0U) {
                --s_status.delayed_pending_count;
            }
            portEXIT_CRITICAL(&s_filter_lock);
        }
        status_increment(&s_status.state_eviction_count);
    }
    memset(&s_states[oldest], 0, sizeof(s_states[oldest]));
    s_states[oldest].valid = true;
    s_states[oldest].slot = slot;
    s_states[oldest].key_len = (uint8_t)key_len;
    memcpy(s_states[oldest].key, key, key_len);
    s_states[oldest].lru = next_lru();
    return oldest;
}

static bool values_equal(const gw_dongle_filter_state_t *state,
                         const uint8_t *value,
                         size_t value_len)
{
    return state->has_value && state->value_len == value_len &&
           memcmp(state->value, value, value_len) == 0;
}

static void save_value(gw_dongle_filter_state_t *state,
                       const uint8_t *value,
                       size_t value_len)
{
    state->value_len = (uint8_t)value_len;
    if (value_len > 0U) {
        memcpy(state->value, value, value_len);
    }
    state->has_value = true;
    state->last_ms = 0U;
}

static esp_err_t emit_due_internal(uint64_t now_ms,
                                   gw_dongle_filter_emit_cb_t emit,
                                   void *ctx)
{
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0U; i < GW_DONGLE_FILTER_STATE_CAPACITY; ++i) {
        gw_dongle_filter_state_t *state = &s_states[i];
        if (!state->valid || !state->pending || now_ms < state->deadline_ms) {
            continue;
        }

        esp_err_t err = emit(state->pending_frame, state->pending_len, ctx);
        if (err == ESP_OK) {
            state->pending = false;
            state->pending_len = 0U;
            portENTER_CRITICAL(&s_filter_lock);
            if (s_status.delayed_pending_count > 0U) {
                --s_status.delayed_pending_count;
            }
            ++s_status.delayed_emit_count;
            portEXIT_CRITICAL(&s_filter_lock);
        } else {
            state->deadline_ms = now_ms + GW_DONGLE_FILTER_RETRY_INTERVAL_MS;
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
        state->lru = next_lru();
    }
    return first_error;
}

esp_err_t gw_dongle_filter_init(void)
{
    esp_err_t mutex_err = ensure_filter_mutex();
    if (mutex_err != ESP_OK) {
        return mutex_err;
    }
    if (s_initialized) {
        return ESP_OK;
    }
    build_default_config(&s_config);
    clear_runtime_states();
    memset(&s_status, 0, sizeof(s_status));
    s_initialized = true;
    return ESP_OK;
}

esp_err_t gw_dongle_filter_get_default_config(gw_dongle_filter_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    build_default_config(out);
    return ESP_OK;
}

esp_err_t gw_dongle_filter_set_config(const gw_dongle_filter_config_t *config)
{
    esp_err_t err = validate_config(config);
    if (err != ESP_OK) {
        return err;
    }
    err = ensure_initialized();
    if (err != ESP_OK) {
        return err;
    }
    err = lock_filter();
    if (err != ESP_OK) {
        return err;
    }
    clear_runtime_states();
    s_config = *config;
    s_config.cmd12_default = normalized_policy(s_config.cmd12_default);
    s_config.other_cmd_default = normalized_policy(s_config.other_cmd_default);
    for (size_t i = 0U; i < s_config.rule_count; ++i) {
        s_config.rules[i].policy = normalized_policy(s_config.rules[i].policy);
    }
    unlock_filter();
    return ESP_OK;
}

esp_err_t gw_dongle_filter_get_config(gw_dongle_filter_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        return err;
    }
    err = lock_filter();
    if (err != ESP_OK) {
        return err;
    }
    *out = s_config;
    unlock_filter();
    return ESP_OK;
}

esp_err_t gw_dongle_filter_reset_runtime(void)
{
    esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        return err;
    }
    err = lock_filter();
    if (err != ESP_OK) {
        return err;
    }
    clear_runtime_states();
    unlock_filter();
    return ESP_OK;
}

esp_err_t gw_dongle_filter_process(const uint8_t *frame,
                                   size_t frame_len,
                                   uint64_t now_ms,
                                   gw_dongle_filter_emit_cb_t emit,
                                   void *ctx)
{
    if (emit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        return err;
    }
    err = lock_filter();
    if (err != ESP_OK) {
        return err;
    }

    gw_dongle_filter_frame_info_t info;
    err = parse_frame(frame, frame_len, &info);
    if (err != ESP_OK) {
        unlock_filter();
        return err;
    }
    /* Preserve ordering when a busy input queue crosses a debounce deadline. */
    (void)emit_due_internal(now_ms, emit, ctx);

    uint16_t slot = 0U;
    const gw_dongle_filter_policy_t *policy = select_policy(&info, &slot);
    status_increment(&s_status.matched_count);

    switch (policy->type) {
    case GW_DONGLE_FILTER_DROP:
        status_increment(&s_status.dropped_count);
        unlock_filter();
        return ESP_OK;

    case GW_DONGLE_FILTER_REALTIME:
        status_increment(&s_status.realtime_count);
        err = emit(info.frame, info.frame_len, ctx);
        unlock_filter();
        return err;

    case GW_DONGLE_FILTER_DEDUP: {
        uint8_t key[GW_DONGLE_FILTER_MAX_KEY_BYTES] = {0};
        const gw_dongle_filter_match_t *match = slot < s_config.rule_count ?
                                                &s_config.rules[slot].match : NULL;
        size_t key_len = build_state_key(&info, match, policy->type, slot, key);
        size_t state_index = find_or_allocate_state(slot, key, key_len);
        gw_dongle_filter_state_t *state = &s_states[state_index];
        uint32_t window_ms = policy->window_ms != 0U ? policy->window_ms :
                             GW_DONGLE_FILTER_DEFAULT_DEDUP_WINDOW_MS;
        if (state->has_value && now_ms - state->last_ms <= window_ms) {
            status_increment(&s_status.dedup_suppressed_count);
            unlock_filter();
            return ESP_OK;
        }
        state->has_value = true;
        state->last_ms = now_ms;
        err = emit(info.frame, info.frame_len, ctx);
        unlock_filter();
        return err;
    }

    case GW_DONGLE_FILTER_CHANGE:
    case GW_DONGLE_FILTER_CHANGE_DELAY: {
        uint8_t key[GW_DONGLE_FILTER_MAX_KEY_BYTES] = {0};
        const gw_dongle_filter_match_t *match = slot < s_config.rule_count ?
                                                &s_config.rules[slot].match : NULL;
        const uint8_t *value = NULL;
        size_t value_len = 0U;
        select_value(&info, match, &value, &value_len);
        size_t key_len = build_state_key(&info, match, policy->type, slot, key);
        size_t state_index = find_or_allocate_state(slot, key, key_len);
        gw_dongle_filter_state_t *state = &s_states[state_index];
        if (values_equal(state, value, value_len)) {
            status_increment(&s_status.unchanged_count);
            unlock_filter();
            return ESP_OK;
        }
        save_value(state, value, value_len);
        if (policy->type == GW_DONGLE_FILTER_CHANGE) {
            err = emit(info.frame, info.frame_len, ctx);
            unlock_filter();
            return err;
        }

        if (policy->delay_ms == 0U) {
            err = emit(info.frame, info.frame_len, ctx);
            unlock_filter();
            return err;
        }
        memcpy(state->pending_frame, info.frame, info.frame_len);
        state->pending_len = (uint8_t)info.frame_len;
        state->deadline_ms = now_ms + policy->delay_ms;
        if (!state->pending) {
            state->pending = true;
            portENTER_CRITICAL(&s_filter_lock);
            ++s_status.delayed_pending_count;
            portEXIT_CRITICAL(&s_filter_lock);
        }
        unlock_filter();
        return ESP_OK;
    }

    default:
        unlock_filter();
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t gw_dongle_filter_poll(uint64_t now_ms,
                                gw_dongle_filter_emit_cb_t emit,
                                void *ctx)
{
    if (emit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        return err;
    }
    err = lock_filter();
    if (err != ESP_OK) {
        return err;
    }
    err = emit_due_internal(now_ms, emit, ctx);
    unlock_filter();
    return err;
}

esp_err_t gw_dongle_filter_get_status(gw_dongle_filter_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = lock_filter();
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&s_filter_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_filter_lock);
    unlock_filter();
    return ESP_OK;
}
