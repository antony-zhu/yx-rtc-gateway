#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../embedded/components/gw_drivers/src/gw_ethernet.c"

static EventBits_t host_bits;
static esp_netif_t host_netif;
static iot_eth_driver_t host_driver;
static unsigned host_ip_timing;
static unsigned host_pump_count;
static unsigned host_free_count;
static unsigned host_uninstall_count;
static unsigned host_uninstall_failures;
static unsigned host_deleted;
static bool host_async_free;
static bool host_all_free;
static bool host_seen_ready;
static int64_t host_time;

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
const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "ERROR"; }

static void deliver_ip(void)
{
    ip_event_got_ip_t event = {
        .esp_netif = s_eth.netif,
        .ip_info = {.ip = {{192, 168, 1, 10}}, .netmask = {{255, 255, 255, 0}}, .gw = {{192, 168, 1, 1}}},
    };
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_ETH_GOT_IP, &event);
}

EventGroupHandle_t xEventGroupCreate(void) { host_bits = 0U; return &host_bits; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { *group |= bits; return *group; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { EventBits_t previous = *group; *group &= ~bits; return previous; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear, BaseType_t all, TickType_t ticks)
{
    (void)all; (void)ticks;
    if (host_ip_timing == 3U) {
        deliver_ip();
    }
    EventBits_t result = *group;
    if (clear) { *group &= ~bits; }
    return result;
}
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t gpio, int level) { (void)gpio; (void)level; return ESP_OK; }
int64_t esp_timer_get_time(void) { return host_time; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t event, esp_event_handler_t callback, void *arg)
{ (void)base; (void)event; (void)callback; (void)arg; return ESP_OK; }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *dns)
{ (void)netif; (void)type; memset(dns, 0, sizeof(*dns)); return ESP_OK; }
esp_netif_t *esp_netif_new(const esp_netif_config_t *config) { (void)config; return &host_netif; }
void esp_netif_destroy(esp_netif_t *netif) { (void)netif; }
esp_err_t esp_netif_attach(esp_netif_t *netif, void *glue) { (void)netif; (void)glue; return ESP_OK; }
esp_err_t iot_eth_new_usb_ecm(const iot_usbh_ecm_config_t *config, iot_eth_driver_t **driver)
{ (void)config; *driver = &host_driver; return ESP_OK; }
esp_err_t iot_eth_install(const iot_eth_config_t *config, iot_eth_handle_t *handle)
{ (void)config; *handle = (iot_eth_handle_t)1; return ESP_OK; }
esp_err_t iot_eth_start(iot_eth_handle_t handle)
{
    ethernet_event_handler(NULL, IOT_ETH_EVENT, IOT_ETH_EVENT_CONNECTED, &handle);
    assert(s_eth.status.stage == GW_ETH_STAGE_GETTING_IP);
    if (host_ip_timing == 1U) { deliver_ip(); }
    return ESP_OK;
}
esp_err_t iot_eth_stop(iot_eth_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t iot_eth_uninstall(iot_eth_handle_t handle) { (void)handle; return ESP_OK; }
iot_eth_netif_glue_handle_t iot_eth_new_netif_glue(iot_eth_handle_t handle) { return handle; }
esp_err_t iot_eth_del_netif_glue(iot_eth_netif_glue_handle_t glue) { (void)glue; return ESP_OK; }
void *usb_ecm_get_cdc_port_handle(iot_eth_driver_t *driver) { (void)driver; return (void *)1; }
esp_err_t usb_ecm_get_last_connect_error(iot_eth_driver_t *driver) { (void)driver; return ESP_OK; }
esp_err_t usbh_cdc_driver_install(const usbh_cdc_driver_config_t *config) { (void)config; return ESP_OK; }
esp_err_t usbh_cdc_driver_uninstall(void) { return ESP_OK; }
void vTaskDelay(TickType_t ticks)
{
    host_time += (int64_t)ticks * 1000;
    if (host_ip_timing == 2U && ticks == pdMS_TO_TICKS(GW_ETH_POWER_STABLE_MS)) { deliver_ip(); }
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core)
{
    (void)function; (void)name; (void)stack; (void)argument; (void)priority; (void)handle; (void)core;
    assert(!"existing host task must never be replaced during shutdown");
    return 0;
}
void vTaskDelete(TaskHandle_t task) { assert(task == NULL); ++host_deleted; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)2; }
void xTaskNotifyGive(TaskHandle_t task) { (void)task; s_eth.usb_host_stop = true; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) { (void)clear; (void)ticks; return 1U; }
esp_err_t usb_host_install(const usb_host_config_t *config) { (void)config; return ESP_OK; }
esp_err_t usb_host_lib_handle_events(TickType_t ticks, uint32_t *flags)
{
    (void)ticks;
    assert(s_eth.usb_host_ready && s_eth.usb_host_task != NULL);
    assert(++host_pump_count <= 6U);
    *flags = 0U;
    if (host_async_free && host_pump_count == 3U) {
        assert(host_free_count > 0U);
        host_all_free = true;
        *flags = USB_HOST_LIB_EVENT_FLAGS_ALL_FREE;
    }
    return ESP_OK;
}
esp_err_t usb_host_device_free_all(void)
{
    ++host_free_count;
    if (host_async_free && !host_all_free) { return ESP_ERR_NOT_FINISHED; }
    host_all_free = true;
    return ESP_OK;
}
esp_err_t usb_host_uninstall(void)
{
    assert(host_all_free);
    assert(s_eth.usb_host_ready && s_eth.usb_host_task != NULL);
    ++host_uninstall_count;
    if (host_uninstall_failures != 0U) {
        --host_uninstall_failures;
        /* A timed-out stop must leave truthful ownership and reject reuse. */
        assert(start_usb_host() == ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void stage_callback(gw_ethernet_stage_t stage, const char *reason, void *context)
{
    (void)reason; (void)context;
    if (host_seen_ready) { assert(stage == GW_ETH_STAGE_READY); }
    host_seen_ready = stage == GW_ETH_STAGE_READY;
}

static void test_dhcp_timing(unsigned timing)
{
    memset(&s_eth, 0, sizeof(s_eth));
    host_seen_ready = false;
    host_ip_timing = timing;
    host_time = 0;
    assert(gw_ethernet_init() == ESP_OK);
    /* Keep the actual host task separate from the synchronous startup test. */
    s_eth.usb_host_ready = true;
    s_eth.usb_host_task = (TaskHandle_t)1;
    s_eth.cdc_ready = true;
    gw_ethernet_start_config_t config = {.timeout_ms = 100U, .stage_cb = stage_callback};
    assert(gw_ethernet_start(&config) == ESP_OK);
    gw_ethernet_status_t status;
    assert(gw_ethernet_get_status(&status) == ESP_OK);
    assert(status.ready && status.stage == GW_ETH_STAGE_READY);
    assert(strcmp(status.ip, "192.168.1.10") == 0);
    assert(host_seen_ready);
}

static void test_host_release(bool asynchronous, unsigned failures)
{
    memset(&s_eth, 0, sizeof(s_eth));
    s_eth.usb_host_task = (TaskHandle_t)1;
    host_async_free = asynchronous;
    host_all_free = false;
    host_pump_count = host_free_count = host_uninstall_count = host_deleted = 0U;
    host_uninstall_failures = failures;
    usb_host_task((void *)2);
    assert(host_deleted == 1U);
    assert(!s_eth.usb_host_ready && s_eth.usb_host_task == NULL);
    assert(host_uninstall_count == failures + 1U);
    assert(host_pump_count >= (asynchronous ? 3U : 1U));
}

int main(void)
{
    test_dhcp_timing(1U);
    test_dhcp_timing(2U);
    test_dhcp_timing(3U);
    test_host_release(false, 0U);
    test_host_release(true, 0U);
    test_host_release(true, 1U);
    puts("Ethernet: early DHCP, DHCP during power stabilization, waited DHCP, empty/asynchronous USB release, and failed uninstall retry passed");
    return 0;
}
