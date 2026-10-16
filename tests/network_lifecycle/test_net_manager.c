#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../embedded/components/gw_app/src/net/gw_net_manager.c"

static TaskFunction_t host_worker;
static void *host_argument;
static gw_network_status_t host_public_status;
static gw_event_id_t host_events[16];
static size_t host_event_count;
static unsigned host_deleted;
static unsigned host_mode;

#if GW_NETWORK_TARGET_4G
typedef gw_4g_ecm_start_config_t host_config_t;
typedef gw_4g_ecm_status_t host_status_t;
#define host_driver_init gw_4g_ecm_init
#define host_driver_prepare gw_4g_ecm_prepare_usb
#define host_driver_start gw_4g_ecm_start
#define host_driver_stop gw_4g_ecm_stop
#define host_driver_status gw_4g_ecm_get_status
#define HOST_READY GW_4G_ECM_STAGE_READY
#define HOST_LOST GW_4G_ECM_STAGE_DISCONNECTED
#define HOST_FAILED GW_4G_ECM_STAGE_FAILED
#else
typedef gw_ethernet_start_config_t host_config_t;
typedef gw_ethernet_status_t host_status_t;
#define host_driver_init gw_ethernet_init
#define host_driver_prepare gw_ethernet_prepare_usb
#define host_driver_start gw_ethernet_start
#define host_driver_stop gw_ethernet_stop
#define host_driver_status gw_ethernet_get_status
#define HOST_READY GW_ETH_STAGE_READY
#define HOST_LOST GW_ETH_STAGE_DISCONNECTED
#define HOST_FAILED GW_ETH_STAGE_FAILED
#endif

static host_status_t host_status;

size_t strlcpy(char *target, const char *source, size_t size)
{
    size_t length = strlen(source);
    if (size != 0U) {
        size_t copied = length < size - 1U ? length : size - 1U;
        memcpy(target, source, copied);
        target[copied] = '\0';
    }
    return length;
}

void host_enter_critical(portMUX_TYPE *lock) { assert(*lock == 0U); *lock = 1U; }
void host_exit_critical(portMUX_TYPE *lock) { assert(*lock == 1U); *lock = 0U; }
const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "ESP_ERR_TIMEOUT"; }
void gw_state_set_network_status(gw_network_status_t status) { host_public_status = status; }
gw_time_point_t gw_time_now(void) { return (gw_time_point_t){.uptime_ms = 100U}; }
const char *gw_event_id_to_string(gw_event_id_t event) { (void)event; return "network"; }
esp_err_t gw_event_post(const gw_event_t *event)
{
    assert(host_event_count < sizeof(host_events) / sizeof(host_events[0]));
    host_events[host_event_count++] = event->event_id;
    return ESP_OK;
}
esp_err_t gw_log_write(gw_log_level_t level, const char *module, const char *event, const char *message, const gw_result_t *result)
{ (void)level; (void)module; (void)event; (void)message; (void)result; return ESP_OK; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority;
    host_worker = function;
    host_argument = argument;
    *handle = (TaskHandle_t)1;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(task == NULL); ++host_deleted; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 4096U; }
esp_err_t host_driver_init(void) { return ESP_OK; }
esp_err_t host_driver_prepare(void) { return ESP_OK; }
esp_err_t host_driver_stop(void) { return ESP_OK; }
esp_err_t host_driver_status(host_status_t *status) { *status = host_status; return ESP_OK; }
esp_err_t host_driver_start(const host_config_t *config)
{
    assert(s_net_status.worker_running);
    if (host_mode == 2U) {
        host_status.stage = HOST_FAILED;
        strlcpy(host_status.reason, "start timeout", sizeof(host_status.reason));
        config->stage_cb(HOST_FAILED, host_status.reason, config->stage_ctx);
        return ESP_ERR_TIMEOUT;
    }
    host_status.stage = HOST_READY;
    host_status.ready = true;
    config->stage_cb(HOST_READY, "ready", config->stage_ctx);
    if (host_mode == 1U) {
        /* Reproduce loss during the 4G post-READY AT refresh, before start returns. */
        host_status.stage = HOST_LOST;
        host_status.ready = false;
        strlcpy(host_status.reason, "link lost during startup", sizeof(host_status.reason));
        config->stage_cb(HOST_LOST, host_status.reason, config->stage_ctx);
    }
    return ESP_OK;
}
#if GW_NETWORK_TARGET_4G
esp_err_t gw_4g_ecm_refresh_modem_info(uint32_t timeout) { (void)timeout; return ESP_OK; }
esp_err_t gw_4g_ecm_at(const char *command, uint32_t timeout, char *response, size_t size)
{ (void)command; (void)timeout; (void)response; (void)size; return ESP_OK; }
const char *gw_4g_ecm_stage_to_string(gw_4g_ecm_stage_t stage) { (void)stage; return "stage"; }
#endif

static gw_net_status_snapshot_t run_start(unsigned mode)
{
    host_mode = mode;
    host_event_count = 0U;
    host_deleted = 0U;
    memset(&host_status, 0, sizeof(host_status));
    assert(gw_net_init() == ESP_OK);
    assert(gw_net_start(gw_net_default_profile()) == ESP_OK);
    assert(s_net_worker_handle != NULL);
    host_worker(host_argument);
    assert(host_deleted == 1U);
    gw_net_status_snapshot_t status = {0};
    assert(gw_net_get_status(&status) == ESP_OK);
    assert(!status.worker_running);
    assert(status.start_count == 1U);
    assert(host_events[0] == GW_EVT_NET_STARTING);
    return status;
}

int main(void)
{
    gw_net_status_snapshot_t status = run_start(0U);
    assert(status.state == GW_NET_STATE_CONNECTED);
    assert(host_public_status == GW_NETWORK_STATUS_CONNECTED);
    assert(host_event_count == 2U && host_events[1] == GW_EVT_NET_READY);

    status = run_start(1U);
    assert(status.state == GW_NET_STATE_DISCONNECTED);
    assert(host_public_status == GW_NETWORK_STATUS_DISCONNECTED);
    assert(strcmp(status.last_reason, "link lost during startup") == 0);
    assert(host_event_count == 2U && host_events[1] == GW_EVT_NET_LOST);
    assert(status.fail_count == 0U);
    /* The supervisor can now retry after observing the truthful lost state. */
    assert(gw_net_start(gw_net_default_profile()) == ESP_OK);
    assert(s_net_worker_handle != NULL);
    host_mode = 0U;
    host_worker(host_argument);
    assert(gw_net_get_status(&status) == ESP_OK);
    assert(status.state == GW_NET_STATE_CONNECTED && status.start_count == 2U);
    assert(host_events[host_event_count - 1U] == GW_EVT_NET_READY);

    status = run_start(2U);
    assert(status.state == GW_NET_STATE_FAILED && status.fail_count == 1U);
    assert(host_event_count == 2U && host_events[1] == GW_EVT_NET_FAILED);
    printf("network manager (%s): ready, loss during startup, retry, and failed start passed\n", gw_net_default_profile_name());
    return 0;
}
