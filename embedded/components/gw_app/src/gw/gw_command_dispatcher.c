#include "gw_command_dispatcher.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "gw_board.h"
#include "gw_dongle_mqtt_bridge.h"
#include "gw_reboot_manager.h"
#include "gw_result.h"
#include "gw_ymzl_protocol.h"

typedef esp_err_t (*command_validate_fn)(const cJSON *args);
typedef esp_err_t (*command_execute_fn)(const cJSON *args);

typedef struct {
    gw_command_capability_t capability;
    command_validate_fn validate;
    command_execute_fn execute;
} command_handler_t;

static esp_err_t validate_object(const cJSON *args)
{
    return cJSON_IsObject(args) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t execute_notify(const cJSON *args)
{
    (void)args;
    return ESP_OK;
}

static esp_err_t validate_board_led(const cJSON *args)
{
    const cJSON *enabled = cJSON_IsObject(args)
                               ? cJSON_GetObjectItemCaseSensitive(args, "enabled")
                               : NULL;
    return cJSON_IsBool(enabled) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t execute_board_led(const cJSON *args)
{
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    return gw_board_task_led_set(cJSON_IsTrue(enabled));
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static esp_err_t decode_frame(const cJSON *args, uint8_t *frame, size_t *out_len)
{
    const cJSON *hex = cJSON_IsObject(args)
                           ? cJSON_GetObjectItemCaseSensitive(args, "hex")
                           : NULL;
    if (!cJSON_IsString(hex) || hex->valuestring == NULL || frame == NULL ||
        out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = strlen(hex->valuestring);
    if (length < 12U || length > GW_SCHEDULER_ACTION_DATA_MAX_BYTES * 2U ||
        (length & 1U) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < length / 2U; ++i) {
        int high = hex_nibble(hex->valuestring[i * 2U]);
        int low = hex_nibble(hex->valuestring[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        frame[i] = (uint8_t)((high << 4) | low);
    }
    *out_len = length / 2U;
    return gw_ymzl_frame_validate(frame, *out_len);
}

static esp_err_t validate_frame_send(const cJSON *args)
{
    uint8_t frame[GW_SCHEDULER_ACTION_DATA_MAX_BYTES];
    size_t frame_len = 0;
    return decode_frame(args, frame, &frame_len);
}

static esp_err_t execute_frame_send(const cJSON *args)
{
    uint8_t frame[GW_SCHEDULER_ACTION_DATA_MAX_BYTES];
    size_t frame_len = 0;
    esp_err_t err = decode_frame(args, frame, &frame_len);
    return err == ESP_OK
               ? gw_dongle_mqtt_bridge_send_frame(frame, frame_len, 1000U)
               : err;
}

static esp_err_t validate_reboot(const cJSON *args)
{
    if (!cJSON_IsObject(args)) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *delay = cJSON_GetObjectItemCaseSensitive(args, "delay_ms");
    if (delay != NULL && (!cJSON_IsNumber(delay) || delay->valuedouble < 0 ||
                          delay->valuedouble > UINT32_MAX ||
                          delay->valuedouble != (double)(uint32_t)delay->valuedouble)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t execute_reboot(const cJSON *args)
{
    const cJSON *delay = cJSON_GetObjectItemCaseSensitive(args, "delay_ms");
    gw_reboot_request_t request = {
        .reason = GW_REBOOT_REASON_REMOTE_REQUEST,
        .delay_ms = cJSON_IsNumber(delay) ? (uint32_t)delay->valuedouble : 0U,
        .execute = true,
    };
    gw_result_t result = gw_result_ok();
    return gw_reboot_manager_request(&request, &result);
}

static const command_handler_t HANDLERS[] = {
    {{"event.notify", 1U}, validate_object, execute_notify},
    {{"board.led.set", 1U}, validate_board_led, execute_board_led},
    {{"gs200.frame.send", 1U}, validate_frame_send, execute_frame_send},
    {{"device.reboot", 1U}, validate_reboot, execute_reboot},
};

static const command_handler_t *find_handler(const char *name, uint16_t version)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(HANDLERS) / sizeof(HANDLERS[0]); ++i) {
        if (HANDLERS[i].capability.version == version &&
            strcmp(HANDLERS[i].capability.name, name) == 0) {
            return &HANDLERS[i];
        }
    }
    return NULL;
}

static cJSON *parse_args(const char *args_json, size_t args_len)
{
    if (args_json == NULL || args_len == 0U ||
        args_len > GW_SCHEDULER_COMMAND_ARGS_MAX_BYTES) {
        return NULL;
    }
    char copy[GW_SCHEDULER_COMMAND_ARGS_MAX_BYTES + 1U];
    memcpy(copy, args_json, args_len);
    copy[args_len] = '\0';
    cJSON *args = cJSON_Parse(copy);
    if (!cJSON_IsObject(args)) {
        cJSON_Delete(args);
        return NULL;
    }
    return args;
}

esp_err_t gw_command_validate(const char *name,
                              uint16_t version,
                              const char *args_json,
                              size_t args_len)
{
    const command_handler_t *handler = find_handler(name, version);
    if (handler == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    cJSON *args = parse_args(args_json, args_len);
    if (args == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = handler->validate(args);
    cJSON_Delete(args);
    return err;
}

esp_err_t gw_command_execute_schedule(const gw_scheduler_due_payload_t *due)
{
    if (due == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (due->action_type == GW_SCHEDULER_ACTION_NOTIFY_ONLY) {
        return ESP_OK;
    }
    if (due->action_type == GW_SCHEDULER_ACTION_BOARD_LED_ON ||
        due->action_type == GW_SCHEDULER_ACTION_BOARD_LED_OFF) {
        return gw_board_task_led_set(due->action_type == GW_SCHEDULER_ACTION_BOARD_LED_ON);
    }
    if (due->action_type == GW_SCHEDULER_ACTION_DONGLE_RAW ||
        due->action_type == GW_SCHEDULER_ACTION_LIGHT_ON ||
        due->action_type == GW_SCHEDULER_ACTION_LIGHT_OFF ||
        due->action_type == GW_SCHEDULER_ACTION_SCENE_SWITCH) {
        if (due->action_len == 0U || due->action_len > sizeof(due->action_data)) {
            return ESP_ERR_INVALID_SIZE;
        }
        return gw_dongle_mqtt_bridge_send_frame(due->action_data,
                                                due->action_len,
                                                1000U);
    }
    if (due->action_type != GW_SCHEDULER_ACTION_COMMAND) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const command_handler_t *handler = find_handler(due->command_name,
                                                     due->command_version);
    cJSON *args = parse_args(due->command_args_json, due->command_args_len);
    if (handler == NULL || args == NULL) {
        cJSON_Delete(args);
        return handler == NULL ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = handler->execute(args);
    cJSON_Delete(args);
    return err;
}

size_t gw_command_get_capabilities(const gw_command_capability_t **out)
{
    static gw_command_capability_t capabilities[sizeof(HANDLERS) / sizeof(HANDLERS[0])];
    for (size_t i = 0; i < sizeof(HANDLERS) / sizeof(HANDLERS[0]); ++i) {
        capabilities[i] = HANDLERS[i].capability;
    }
    if (out != NULL) {
        *out = capabilities;
    }
    return sizeof(HANDLERS) / sizeof(HANDLERS[0]);
}
