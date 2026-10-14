#include "gw_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_event.h"
#include "gw_log.h"
#include "gw_result.h"
#include "gw_state.h"
#include "gw_time.h"
#include "gw_network_build.h"

#if GW_NETWORK_TARGET_4G
#include "gw_4g_ecm.h"
#elif GW_NETWORK_TARGET_ETHERNET
#include "gw_ethernet.h"
#else
#error "A gateway network target must be selected by CMake"
#endif

static const char *TAG = "gw_net";
static portMUX_TYPE s_net_lock = portMUX_INITIALIZER_UNLOCKED;
static gw_net_status_snapshot_t s_net_status;
static TaskHandle_t s_net_worker_handle;

static gw_network_status_t public_status_from_state(gw_net_state_t state)
{
    switch (state) {
    case GW_NET_STATE_STARTING:
    case GW_NET_STATE_SIM_CHECKING:
    case GW_NET_STATE_REGISTERING_NETWORK:
    case GW_NET_STATE_ATTACHING:
    case GW_NET_STATE_GETTING_IP:
        return GW_NETWORK_STATUS_CONNECTING;
    case GW_NET_STATE_CONNECTED:
        return GW_NETWORK_STATUS_CONNECTED;
    case GW_NET_STATE_DEGRADED:
        return GW_NETWORK_STATUS_DEGRADED;
    case GW_NET_STATE_STOPPED:
    case GW_NET_STATE_DISCONNECTED:
    case GW_NET_STATE_FAILED:
    default:
        return GW_NETWORK_STATUS_DISCONNECTED;
    }
}

static void net_set_state(gw_net_state_t state, const char *reason)
{
    gw_network_status_t public_status = public_status_from_state(state);
    portENTER_CRITICAL(&s_net_lock);
    s_net_status.state = state;
    s_net_status.public_status = public_status;
    if (reason != NULL) {
        strlcpy(s_net_status.last_reason, reason, sizeof(s_net_status.last_reason));
    }
    portEXIT_CRITICAL(&s_net_lock);
    gw_state_set_network_status(public_status);
}

static void net_post_event(gw_event_id_t event_id, gw_result_t result)
{
    gw_time_point_t now = gw_time_now();
    gw_event_t event = {
        .event_id = event_id,
        .priority = GW_EVENT_PRIORITY_HIGH,
        .source = "net_worker",
        .target = "dev_mgr_task",
        .uptime_ms = now.uptime_ms,
        .timestamp = now.utc_trusted ? now.utc_ms : 0,
        .result = result,
    };
    esp_err_t err = gw_event_post(&event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "post %s failed: %s",
                 gw_event_id_to_string(event_id),
                 esp_err_to_name(err));
    }
}

static void net_apply_driver_stage(gw_net_state_t state, const char *reason)
{
    bool was_connected;
    portENTER_CRITICAL(&s_net_lock);
    was_connected = s_net_status.public_status == GW_NETWORK_STATUS_CONNECTED;
    portEXIT_CRITICAL(&s_net_lock);

    net_set_state(state, reason);
    if (was_connected && state == GW_NET_STATE_DISCONNECTED) {
        net_post_event(GW_EVT_NET_LOST,
                       gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                       GW_REASON_NETWORK_UNAVAILABLE,
                                       reason));
    }
}

#if GW_NETWORK_TARGET_4G

static gw_net_state_t net_state_from_driver_stage(gw_4g_ecm_stage_t stage)
{
    switch (stage) {
    case GW_4G_ECM_STAGE_STARTING:
    case GW_4G_ECM_STAGE_WAIT_USB:
        return GW_NET_STATE_STARTING;
    case GW_4G_ECM_STAGE_PROBING:
    case GW_4G_ECM_STAGE_SIM_CHECKING:
        return GW_NET_STATE_SIM_CHECKING;
    case GW_4G_ECM_STAGE_REGISTERING:
        return GW_NET_STATE_REGISTERING_NETWORK;
    case GW_4G_ECM_STAGE_CONFIGURING:
        return GW_NET_STATE_ATTACHING;
    case GW_4G_ECM_STAGE_GETTING_IP:
        return GW_NET_STATE_GETTING_IP;
    case GW_4G_ECM_STAGE_READY:
        return GW_NET_STATE_CONNECTED;
    case GW_4G_ECM_STAGE_DISCONNECTED:
        return GW_NET_STATE_DISCONNECTED;
    case GW_4G_ECM_STAGE_FAILED:
        return GW_NET_STATE_FAILED;
    case GW_4G_ECM_STAGE_STOPPED:
    case GW_4G_ECM_STAGE_IDLE:
    default:
        return GW_NET_STATE_STOPPED;
    }
}

static void net_driver_stage_cb(gw_4g_ecm_stage_t stage,
                                const char *reason,
                                void *user_ctx)
{
    (void)user_ctx;
    net_apply_driver_stage(net_state_from_driver_stage(stage), reason);
}

static esp_err_t net_driver_init(void)
{
    return gw_4g_ecm_init();
}

static esp_err_t net_driver_prepare(void)
{
    return gw_4g_ecm_prepare_usb();
}

static esp_err_t net_driver_start(void)
{
    gw_4g_ecm_start_config_t config = {
        .apn = s_net_status.apn,
        .timeout_ms = 0,
        .stage_cb = net_driver_stage_cb,
        .stage_ctx = NULL,
    };
    return gw_4g_ecm_start(&config);
}

static esp_err_t net_driver_stop(void)
{
    return gw_4g_ecm_stop();
}

static void net_copy_driver_status(void)
{
    gw_4g_ecm_status_t status = {0};
    if (gw_4g_ecm_get_status(&status) != ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&s_net_lock);
    strlcpy(s_net_status.apn, status.apn, sizeof(s_net_status.apn));
    strlcpy(s_net_status.ip, status.ip, sizeof(s_net_status.ip));
    strlcpy(s_net_status.netmask, status.netmask, sizeof(s_net_status.netmask));
    strlcpy(s_net_status.gateway, status.gateway, sizeof(s_net_status.gateway));
    strlcpy(s_net_status.dns0, status.dns0, sizeof(s_net_status.dns0));
    strlcpy(s_net_status.dns1, status.dns1, sizeof(s_net_status.dns1));
    strlcpy(s_net_status.imei, status.modem.imei, sizeof(s_net_status.imei));
    strlcpy(s_net_status.iccid, status.modem.iccid, sizeof(s_net_status.iccid));
    strlcpy(s_net_status.last_reason, status.reason, sizeof(s_net_status.last_reason));
    s_net_status.rssi = status.modem.rssi;
    portEXIT_CRITICAL(&s_net_lock);
}

#else

static gw_net_state_t net_state_from_driver_stage(gw_ethernet_stage_t stage)
{
    switch (stage) {
    case GW_ETH_STAGE_STARTING:
    case GW_ETH_STAGE_WAIT_USB:
        return GW_NET_STATE_STARTING;
    case GW_ETH_STAGE_GETTING_IP:
        return GW_NET_STATE_GETTING_IP;
    case GW_ETH_STAGE_READY:
        return GW_NET_STATE_CONNECTED;
    case GW_ETH_STAGE_DISCONNECTED:
        return GW_NET_STATE_DISCONNECTED;
    case GW_ETH_STAGE_FAILED:
        return GW_NET_STATE_FAILED;
    case GW_ETH_STAGE_STOPPED:
    case GW_ETH_STAGE_IDLE:
    default:
        return GW_NET_STATE_STOPPED;
    }
}

static void net_driver_stage_cb(gw_ethernet_stage_t stage,
                                const char *reason,
                                void *user_ctx)
{
    (void)user_ctx;
    net_apply_driver_stage(net_state_from_driver_stage(stage), reason);
}

static esp_err_t net_driver_init(void)
{
    return gw_ethernet_init();
}

static esp_err_t net_driver_prepare(void)
{
    return gw_ethernet_prepare_usb();
}

static esp_err_t net_driver_start(void)
{
    gw_ethernet_start_config_t config = {
        .timeout_ms = 0,
        .stage_cb = net_driver_stage_cb,
        .stage_ctx = NULL,
    };
    return gw_ethernet_start(&config);
}

static esp_err_t net_driver_stop(void)
{
    return gw_ethernet_stop();
}

static void net_copy_driver_status(void)
{
    gw_ethernet_status_t status = {0};
    if (gw_ethernet_get_status(&status) != ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&s_net_lock);
    s_net_status.apn[0] = '\0';
    strlcpy(s_net_status.ip, status.ip, sizeof(s_net_status.ip));
    strlcpy(s_net_status.netmask, status.netmask, sizeof(s_net_status.netmask));
    strlcpy(s_net_status.gateway, status.gateway, sizeof(s_net_status.gateway));
    strlcpy(s_net_status.dns0, status.dns0, sizeof(s_net_status.dns0));
    strlcpy(s_net_status.dns1, status.dns1, sizeof(s_net_status.dns1));
    s_net_status.imei[0] = '\0';
    s_net_status.iccid[0] = '\0';
    strlcpy(s_net_status.last_reason, status.reason, sizeof(s_net_status.last_reason));
    s_net_status.rssi = 99;
    portEXIT_CRITICAL(&s_net_lock);
}

#endif

static void net_worker_task(void *arg)
{
    gw_net_profile_t profile = (gw_net_profile_t)(uintptr_t)arg;
    const char *profile_name = gw_net_default_profile_name();
    portENTER_CRITICAL(&s_net_lock);
    s_net_status.worker_running = true;
    s_net_status.profile = profile;
    ++s_net_status.start_count;
    portEXIT_CRITICAL(&s_net_lock);

    net_set_state(GW_NET_STATE_STARTING, "");
    net_post_event(GW_EVT_NET_STARTING, gw_result_ok());
    gw_log_write(GW_LOG_LEVEL_INFO,
                 "net",
                 "start",
                 profile == GW_NET_PROFILE_4G
                     ? "default 4g network start requested"
                     : "default ethernet network start requested",
                 NULL);

    esp_err_t err = net_driver_start();
    ESP_LOGI(TAG,
             "net_worker stack high water: %lu bytes",
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    net_copy_driver_status();

    portENTER_CRITICAL(&s_net_lock);
    s_net_status.worker_running = false;
    s_net_worker_handle = NULL;
    if (err != ESP_OK) {
        ++s_net_status.fail_count;
    }
    portEXIT_CRITICAL(&s_net_lock);

    if (err == ESP_OK) {
        char detail[32];
        snprintf(detail, sizeof(detail), "%s ready", profile_name);
        net_set_state(GW_NET_STATE_CONNECTED, detail);
        net_post_event(GW_EVT_NET_READY, gw_result_ok());
        gw_log_write(GW_LOG_LEVEL_INFO, "net", "ready", detail, NULL);
        vTaskDelete(NULL);
        return;
    }

    const char *reason = err == ESP_ERR_TIMEOUT
                             ? GW_REASON_TIMEOUT
                             : GW_REASON_INTERNAL_ERROR;
    gw_result_t result = gw_result_error(
        GW_RESULT_INTERNAL_ERROR,
        reason,
        s_net_status.last_reason[0] != '\0'
            ? s_net_status.last_reason
            : esp_err_to_name(err));
    net_set_state(GW_NET_STATE_FAILED, result.detail);
    net_post_event(GW_EVT_NET_FAILED, result);
    gw_log_write(GW_LOG_LEVEL_WARN,
                 "net",
                 "failed",
                 profile == GW_NET_PROFILE_4G
                     ? "4g network start failed"
                     : "ethernet network start failed",
                 &result);
    vTaskDelete(NULL);
}

gw_net_profile_t gw_net_default_profile(void)
{
#if GW_NETWORK_TARGET_4G
    return GW_NET_PROFILE_4G;
#else
    return GW_NET_PROFILE_ETHERNET;
#endif
}

const char *gw_net_default_profile_name(void)
{
#if GW_NETWORK_TARGET_4G
    return "4g";
#else
    return "ethernet";
#endif
}

esp_err_t gw_net_init(void)
{
    portENTER_CRITICAL(&s_net_lock);
    memset(&s_net_status, 0, sizeof(s_net_status));
    s_net_status.initialized = true;
    s_net_status.profile = gw_net_default_profile();
    s_net_status.state = GW_NET_STATE_STOPPED;
    s_net_status.public_status = GW_NETWORK_STATUS_DISCONNECTED;
    s_net_status.rssi = 99;
    portEXIT_CRITICAL(&s_net_lock);

    ESP_RETURN_ON_ERROR(net_driver_init(), TAG, "network driver init failed");
    gw_state_set_network_status(GW_NETWORK_STATUS_DISCONNECTED);
    return ESP_OK;
}

esp_err_t gw_net_prepare(void)
{
    return net_driver_prepare();
}

esp_err_t gw_net_start(gw_net_profile_t profile)
{
    if (profile != gw_net_default_profile()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&s_net_lock);
    bool initialized = s_net_status.initialized;
    bool already_running = s_net_worker_handle != NULL;
    bool already_connected = s_net_status.state == GW_NET_STATE_CONNECTED ||
                             s_net_status.state == GW_NET_STATE_DEGRADED;
    portEXIT_CRITICAL(&s_net_lock);

    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (already_running || already_connected) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(net_worker_task,
                                "net_worker",
                                GW_HTTP_WORKER_STACK_BYTES,
                                (void *)(uintptr_t)profile,
                                5,
                                &s_net_worker_handle);
    if (ok != pdPASS) {
        portENTER_CRITICAL(&s_net_lock);
        s_net_worker_handle = NULL;
        portEXIT_CRITICAL(&s_net_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t gw_net_start_by_name(const char *profile_name)
{
    const char *default_name = gw_net_default_profile_name();
    if (profile_name == NULL || profile_name[0] == '\0' ||
        strcmp(profile_name, default_name) == 0 ||
        (gw_net_default_profile() == GW_NET_PROFILE_ETHERNET &&
         strcmp(profile_name, "eth") == 0)) {
        return gw_net_start(gw_net_default_profile());
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_net_stop(const char *reason)
{
    esp_err_t err = net_driver_stop();
    net_copy_driver_status();
    net_set_state(GW_NET_STATE_STOPPED, reason != NULL ? reason : "");
    return err;
}

esp_err_t gw_net_refresh_modem_info(uint32_t timeout_ms)
{
#if GW_NETWORK_TARGET_4G
    return gw_4g_ecm_refresh_modem_info(timeout_ms);
#else
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gw_net_at(const char *command,
                    uint32_t timeout_ms,
                    char *out_response,
                    size_t response_size)
{
#if GW_NETWORK_TARGET_4G
    return gw_4g_ecm_at(command, timeout_ms, out_response, response_size);
#else
    (void)command;
    (void)timeout_ms;
    (void)out_response;
    (void)response_size;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gw_net_get_4g_status(gw_4g_ecm_status_t *out)
{
#if GW_NETWORK_TARGET_4G
    return gw_4g_ecm_get_status(out);
#else
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const char *gw_net_4g_stage_to_string(gw_4g_ecm_stage_t stage)
{
#if GW_NETWORK_TARGET_4G
    return gw_4g_ecm_stage_to_string(stage);
#else
    (void)stage;
    return "unsupported";
#endif
}

esp_err_t gw_net_get_status(gw_net_status_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    net_copy_driver_status();
    portENTER_CRITICAL(&s_net_lock);
    *out = s_net_status;
    out->worker_running = s_net_worker_handle != NULL ||
                          s_net_status.worker_running;
    portEXIT_CRITICAL(&s_net_lock);
    return ESP_OK;
}

const char *gw_net_state_to_string(gw_net_state_t state)
{
    switch (state) {
    case GW_NET_STATE_STOPPED: return "stopped";
    case GW_NET_STATE_STARTING: return "starting";
    case GW_NET_STATE_SIM_CHECKING: return "sim_checking";
    case GW_NET_STATE_REGISTERING_NETWORK: return "registering_network";
    case GW_NET_STATE_ATTACHING: return "attaching";
    case GW_NET_STATE_GETTING_IP: return "getting_ip";
    case GW_NET_STATE_CONNECTED: return "connected";
    case GW_NET_STATE_DEGRADED: return "degraded";
    case GW_NET_STATE_DISCONNECTED: return "disconnected";
    case GW_NET_STATE_FAILED: return "failed";
    default: return "unknown";
    }
}
