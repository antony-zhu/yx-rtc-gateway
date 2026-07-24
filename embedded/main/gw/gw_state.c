#include "gw_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "gw_event.h"
#include "gw_log.h"
#include "gw_time.h"

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static gw_state_snapshot_t s_state;

esp_err_t gw_state_init(void)
{
    gw_time_point_t now = gw_time_now();

    portENTER_CRITICAL(&s_state_lock);
    memset(&s_state, 0, sizeof(s_state));
    s_state.device_status = GW_DEVICE_STATUS_BOOTING;
    s_state.network_status = GW_NETWORK_STATUS_DISCONNECTED;
    s_state.mqtt_status = GW_MQTT_STATUS_DISCONNECTED;
    s_state.script_status = GW_SCRIPT_STATUS_STOPPED;
    s_state.task_status = GW_TASK_STATUS_IDLE;
    s_state.health_status = GW_HEALTH_STATUS_OK;
    s_state.boot_uptime_ms = now.uptime_ms;
    portEXIT_CRITICAL(&s_state_lock);

    return ESP_OK;
}

esp_err_t gw_state_get_snapshot(gw_state_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_time_point_t now = gw_time_now();

    portENTER_CRITICAL(&s_state_lock);
    *out = s_state;
    out->boot_uptime_ms = now.uptime_ms;
    portEXIT_CRITICAL(&s_state_lock);

    out->event_pending_count = gw_event_get_pending_count();
    out->event_dropped_count = gw_event_get_dropped_count();
    out->log_written_count = gw_log_get_written_count();
    out->log_dropped_count = gw_log_get_dropped_count();

    return ESP_OK;
}

void gw_state_set_device_status(gw_device_status_t status)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.device_status = status;
    portEXIT_CRITICAL(&s_state_lock);
}

void gw_state_set_network_status(gw_network_status_t status)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.network_status = status;
    portEXIT_CRITICAL(&s_state_lock);
}

void gw_state_set_mqtt_status(gw_mqtt_status_t status)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.mqtt_status = status;
    portEXIT_CRITICAL(&s_state_lock);
}

void gw_state_set_script_status(gw_script_status_t status)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.script_status = status;
    portEXIT_CRITICAL(&s_state_lock);
}

void gw_state_set_task_status(gw_task_status_t status)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.task_status = status;
    portEXIT_CRITICAL(&s_state_lock);
}

void gw_state_set_health_status(gw_health_status_t status)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.health_status = status;
    portEXIT_CRITICAL(&s_state_lock);
}

void gw_state_update_stack_high_watermark(const char *task_name, uint32_t free_words)
{
    if (task_name == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    if (strcmp(task_name, "dev_mgr_task") == 0) {
        s_state.dev_mgr_stack_free_words = free_words;
    } else if (strcmp(task_name, "script_task") == 0) {
        s_state.script_stack_free_words = free_words;
    } else if (strcmp(task_name, "log_task") == 0) {
        s_state.log_stack_free_words = free_words;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

const char *gw_device_status_to_string(gw_device_status_t status)
{
    switch (status) {
    case GW_DEVICE_STATUS_BOOTING:
        return "booting";
    case GW_DEVICE_STATUS_OFFLINE:
        return "offline";
    case GW_DEVICE_STATUS_ONLINE:
        return "online";
    case GW_DEVICE_STATUS_CONFIG_CHECKING:
        return "config_checking";
    case GW_DEVICE_STATUS_CONFIG_UPDATING:
        return "config_updating";
    case GW_DEVICE_STATUS_OTA_CHECKING:
        return "ota_checking";
    case GW_DEVICE_STATUS_OTA_UPGRADING:
        return "ota_upgrading";
    case GW_DEVICE_STATUS_DEGRADED:
        return "degraded";
    case GW_DEVICE_STATUS_BUSY:
        return "busy";
    case GW_DEVICE_STATUS_FACTORY_RESETTING:
        return "factory_resetting";
    case GW_DEVICE_STATUS_REBOOTING:
        return "rebooting";
    default:
        return "unknown";
    }
}

const char *gw_network_status_to_string(gw_network_status_t status)
{
    switch (status) {
    case GW_NETWORK_STATUS_DISCONNECTED:
        return "disconnected";
    case GW_NETWORK_STATUS_CONNECTING:
        return "connecting";
    case GW_NETWORK_STATUS_CONNECTED:
        return "connected";
    case GW_NETWORK_STATUS_DEGRADED:
        return "degraded";
    default:
        return "unknown";
    }
}

const char *gw_mqtt_status_to_string(gw_mqtt_status_t status)
{
    switch (status) {
    case GW_MQTT_STATUS_DISCONNECTED:
        return "disconnected";
    case GW_MQTT_STATUS_NOT_CONFIGURED:
        return "not_configured";
    case GW_MQTT_STATUS_CONNECTING:
        return "connecting";
    case GW_MQTT_STATUS_CONNECTED:
        return "connected";
    case GW_MQTT_STATUS_RECONNECTING:
        return "reconnecting";
    case GW_MQTT_STATUS_AUTH_FAILED:
        return "auth_failed";
    default:
        return "unknown";
    }
}

const char *gw_script_status_to_string(gw_script_status_t status)
{
    switch (status) {
    case GW_SCRIPT_STATUS_DISABLED:
        return "disabled";
    case GW_SCRIPT_STATUS_DISABLED_BY_FAULT:
        return "disabled_by_fault";
    case GW_SCRIPT_STATUS_STARTING:
        return "starting";
    case GW_SCRIPT_STATUS_RUNNING:
        return "running";
    case GW_SCRIPT_STATUS_STOPPED:
        return "stopped";
    case GW_SCRIPT_STATUS_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

const char *gw_task_status_to_string(gw_task_status_t status)
{
    switch (status) {
    case GW_TASK_STATUS_IDLE:
        return "idle";
    case GW_TASK_STATUS_BUSY:
        return "busy";
    case GW_TASK_STATUS_CONFIG_CHECKING:
        return "config_checking";
    case GW_TASK_STATUS_CONFIG_UPDATING:
        return "config_updating";
    case GW_TASK_STATUS_OTA_CHECKING:
        return "ota_checking";
    case GW_TASK_STATUS_OTA_UPGRADING:
        return "ota_upgrading";
    case GW_TASK_STATUS_LOG_UPLOADING:
        return "log_uploading";
    case GW_TASK_STATUS_FACTORY_RESETTING:
        return "factory_resetting";
    case GW_TASK_STATUS_REBOOTING:
        return "rebooting";
    default:
        return "unknown";
    }
}

const char *gw_health_status_to_string(gw_health_status_t status)
{
    switch (status) {
    case GW_HEALTH_STATUS_OK:
        return "ok";
    case GW_HEALTH_STATUS_WARN:
        return "warn";
    case GW_HEALTH_STATUS_DEGRADED:
        return "degraded";
    case GW_HEALTH_STATUS_FATAL_PENDING:
        return "fatal_pending";
    default:
        return "unknown";
    }
}
