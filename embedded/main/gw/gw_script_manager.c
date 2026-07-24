#include "gw_script_manager.h"

#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "gw_defaults.h"
#include "gw_event.h"
#include "gw_log.h"
#include "gw_param.h"
#include "gw_result.h"
#include "gw_state.h"
#include "gw_storage.h"
#include "gw_time.h"
#include "py_runtime.h"

static portMUX_TYPE s_script_mgr_lock = portMUX_INITIALIZER_UNLOCKED;
static gw_script_manager_status_t s_status;

static const char *normalize_reason(const char *reason)
{
    return reason != NULL && reason[0] != '\0' ? reason : "script_stop";
}

static bool is_valid_exception_action(const char *action)
{
    return strcmp(action, GW_SCRIPT_EXCEPTION_ACTION_RESTART_IMMEDIATELY) == 0 ||
           strcmp(action, GW_SCRIPT_EXCEPTION_ACTION_RESTART_BY_WATCHDOG) == 0 ||
           strcmp(action, GW_SCRIPT_EXCEPTION_ACTION_IGNORE) == 0;
}

static void set_exception_action_locked(const char *action)
{
    strlcpy(s_status.exception_action,
            is_valid_exception_action(action) ? action : GW_SCRIPT_EXCEPTION_ACTION_DEFAULT,
            sizeof(s_status.exception_action));
}

static void load_exception_action_from_nvs(void)
{
    char action[sizeof(s_status.exception_action)];
    esp_err_t err = gw_storage_secure_get_string(GW_NVS_NAMESPACE_SYSTEM,
                                                 GW_PARAM_KEY_SCRIPT_EXCEPTION_ACTION,
                                                 action,
                                                 sizeof(action));
    if (err != ESP_OK || !is_valid_exception_action(action)) {
        strlcpy(action, GW_SCRIPT_EXCEPTION_ACTION_DEFAULT, sizeof(action));
        (void)gw_storage_secure_set_string(GW_NVS_NAMESPACE_SYSTEM,
                                           GW_PARAM_KEY_SCRIPT_EXCEPTION_ACTION,
                                           action);
    }

    portENTER_CRITICAL(&s_script_mgr_lock);
    set_exception_action_locked(action);
    portEXIT_CRITICAL(&s_script_mgr_lock);
}

static void post_script_event(gw_event_id_t event_id,
                              const char *reason,
                              int32_t code,
                              const char *detail)
{
    gw_time_point_t now = gw_time_now();
    gw_event_t event = {
        .event_id = event_id,
        .priority = event_id == GW_EVT_SCRIPT_STOP_TIMEOUT ?
                    GW_EVENT_PRIORITY_HIGH :
                    GW_EVENT_PRIORITY_NORMAL,
        .source = "script_manager",
        .target = "dev_mgr_task",
        .request_id = NULL,
        .method = NULL,
        .uptime_ms = now.uptime_ms,
        .timestamp = now.utc_trusted ? now.utc_ms : 0,
        .result = gw_result_error(code, normalize_reason(reason), detail),
        .payload = NULL,
        .payload_len = 0,
        .release_cb = NULL,
    };

    (void)gw_event_post(&event);
}

esp_err_t gw_script_manager_init(void)
{
    portENTER_CRITICAL(&s_script_mgr_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    set_exception_action_locked(GW_SCRIPT_EXCEPTION_ACTION_DEFAULT);
    portEXIT_CRITICAL(&s_script_mgr_lock);
    load_exception_action_from_nvs();
    return ESP_OK;
}

esp_err_t gw_script_manager_request_stop(const char *reason, uint32_t timeout_ms, bool *out_stopped)
{
    const char *stable_reason = normalize_reason(reason);
    uint32_t wait_ms = timeout_ms != 0 ? timeout_ms : GW_SCRIPT_STOP_TIMEOUT_MS;

    if (out_stopped != NULL) {
        *out_stopped = false;
    }

    portENTER_CRITICAL(&s_script_mgr_lock);
    ++s_status.request_count;
    strlcpy(s_status.last_reason, stable_reason, sizeof(s_status.last_reason));
    s_status.last_stopped = false;
    portEXIT_CRITICAL(&s_script_mgr_lock);

    if (!py_runtime_is_started()) {
        portENTER_CRITICAL(&s_script_mgr_lock);
        ++s_status.stopped_count;
        s_status.last_stopped = true;
        portEXIT_CRITICAL(&s_script_mgr_lock);
        if (out_stopped != NULL) {
            *out_stopped = true;
        }
        return ESP_OK;
    }

    post_script_event(GW_EVT_SCRIPT_STOP_REQUEST,
                      stable_reason,
                      GW_RESULT_OK,
                      "script stop requested");
    esp_err_t stop_err = py_runtime_request_stop(stable_reason);
    if (stop_err != ESP_OK) {
        return stop_err;
    }

    esp_err_t err = py_runtime_wait_stopped(wait_ms);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_script_mgr_lock);
        ++s_status.stopped_count;
        s_status.last_stopped = true;
        portEXIT_CRITICAL(&s_script_mgr_lock);
        if (out_stopped != NULL) {
            *out_stopped = true;
        }
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_script_mgr_lock);
    ++s_status.timeout_count;
    portEXIT_CRITICAL(&s_script_mgr_lock);

    gw_result_t timeout_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                                 stable_reason,
                                                 "script stop timeout");
    timeout_result.esp_err = err;
    gw_log_write(GW_LOG_LEVEL_WARN,
                 "script",
                 "stop_timeout",
                 "script did not stop before timeout",
                 &timeout_result);
    post_script_event(GW_EVT_SCRIPT_STOP_TIMEOUT,
                      stable_reason,
                      GW_RESULT_INTERNAL_ERROR,
                      "script stop timeout");
    return ESP_ERR_TIMEOUT;
}

esp_err_t gw_script_manager_restart(const char *active_slot)
{
    esp_err_t err = py_runtime_start(active_slot);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_script_mgr_lock);
        ++s_status.restart_count;
        portEXIT_CRITICAL(&s_script_mgr_lock);
    }
    return err;
}

esp_err_t gw_script_manager_set_exception_action(const char *action)
{
    if (action == NULL || !is_valid_exception_action(action)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(gw_storage_secure_set_string(GW_NVS_NAMESPACE_SYSTEM,
                                                     GW_PARAM_KEY_SCRIPT_EXCEPTION_ACTION,
                                                     action),
                        "gw_script_mgr",
                        "save script exception action failed");

    portENTER_CRITICAL(&s_script_mgr_lock);
    set_exception_action_locked(action);
    portEXIT_CRITICAL(&s_script_mgr_lock);
    return ESP_OK;
}

esp_err_t gw_script_manager_get_exception_action(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_script_mgr_lock);
    strlcpy(out, s_status.exception_action, out_size);
    portEXIT_CRITICAL(&s_script_mgr_lock);
    return ESP_OK;
}

esp_err_t gw_script_manager_get_status(gw_script_manager_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_script_mgr_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_script_mgr_lock);
    return ESP_OK;
}
