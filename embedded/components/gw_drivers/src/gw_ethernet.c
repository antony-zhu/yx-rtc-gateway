#include "gw_ethernet.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_ecm.h"
#include "usb/usb_host.h"

#include "gw_hw_config.h"

#define GW_ETH_EVENT_GOT_IP  BIT0
#define GW_ETH_EVENT_LOST_IP BIT1

static const char *TAG = "gw_ethernet";
static portMUX_TYPE s_eth_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    bool initialized;
    bool idf_ready;
    bool handlers_registered;
    bool stopping;
    bool usb_host_ready;
    bool usb_host_stop;
    bool cdc_ready;
    bool eth_started;
    EventGroupHandle_t events;
    TaskHandle_t usb_host_task;
    esp_netif_t *netif;
    iot_eth_driver_t *driver;
    iot_eth_handle_t eth;
    iot_eth_netif_glue_handle_t glue;
    gw_ethernet_stage_cb_t stage_cb;
    void *stage_ctx;
    gw_ethernet_status_t status;
} gw_ethernet_ctx_t;

static gw_ethernet_ctx_t s_eth;

static void copy_text(char *out, size_t out_size, const char *text)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    strlcpy(out, text != NULL ? text : "", out_size);
}

static void set_stage(gw_ethernet_stage_t stage, const char *reason)
{
    gw_ethernet_stage_cb_t callback;
    void *callback_ctx;

    portENTER_CRITICAL(&s_eth_lock);
    s_eth.status.stage = stage;
    s_eth.status.ready = stage == GW_ETH_STAGE_READY;
    copy_text(s_eth.status.reason, sizeof(s_eth.status.reason), reason);
    callback = s_eth.stage_cb;
    callback_ctx = s_eth.stage_ctx;
    portEXIT_CRITICAL(&s_eth_lock);

    if (callback != NULL) {
        callback(stage, reason != NULL ? reason : "", callback_ctx);
    }
}

static void clear_ip_status(void)
{
    portENTER_CRITICAL(&s_eth_lock);
    s_eth.status.ip[0] = '\0';
    s_eth.status.netmask[0] = '\0';
    s_eth.status.gateway[0] = '\0';
    s_eth.status.dns0[0] = '\0';
    s_eth.status.dns1[0] = '\0';
    portEXIT_CRITICAL(&s_eth_lock);
}

static void update_ip_status(const esp_netif_ip_info_t *ip_info)
{
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns0[16] = "0.0.0.0";
    char dns1[16] = "0.0.0.0";
    esp_netif_dns_info_t dns = {0};

    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info->ip));
    snprintf(netmask, sizeof(netmask), IPSTR, IP2STR(&ip_info->netmask));
    snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&ip_info->gw));
    if (esp_netif_get_dns_info(s_eth.netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        snprintf(dns0, sizeof(dns0), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    if (esp_netif_get_dns_info(s_eth.netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
        snprintf(dns1, sizeof(dns1), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }

    portENTER_CRITICAL(&s_eth_lock);
    copy_text(s_eth.status.ip, sizeof(s_eth.status.ip), ip);
    copy_text(s_eth.status.netmask, sizeof(s_eth.status.netmask), netmask);
    copy_text(s_eth.status.gateway, sizeof(s_eth.status.gateway), gateway);
    copy_text(s_eth.status.dns0, sizeof(s_eth.status.dns0), dns0);
    copy_text(s_eth.status.dns1, sizeof(s_eth.status.dns1), dns1);
    portEXIT_CRITICAL(&s_eth_lock);
}

static bool event_is_ours(void *event_data)
{
    return event_data == NULL ||
           *(iot_eth_handle_t *)event_data == s_eth.eth;
}

static void ethernet_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)arg;
    (void)event_base;
    if (!event_is_ours(event_data)) {
        return;
    }

    if (event_id == IOT_ETH_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "CH397 ethernet link up");
        return;
    }
    if (event_id != IOT_ETH_EVENT_DISCONNECTED) {
        return;
    }

    clear_ip_status();
    if (!s_eth.stopping && s_eth.status.ready) {
        set_stage(GW_ETH_STAGE_DISCONNECTED, "ethernet link down");
        if (s_eth.events != NULL) {
            xEventGroupSetBits(s_eth.events, GW_ETH_EVENT_LOST_IP);
        }
    }
}

static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        if (got_ip == NULL || got_ip->esp_netif != s_eth.netif) {
            return;
        }
        update_ip_status(&got_ip->ip_info);
        set_stage(GW_ETH_STAGE_READY, "ethernet ready");
        xEventGroupSetBits(s_eth.events, GW_ETH_EVENT_GOT_IP);
        ESP_LOGI(TAG,
                 "ethernet got ip=%s netmask=%s gateway=%s",
                 s_eth.status.ip,
                 s_eth.status.netmask,
                 s_eth.status.gateway);
        return;
    }

    if (event_id == IP_EVENT_ETH_LOST_IP && s_eth.status.ready) {
        clear_ip_status();
        set_stage(GW_ETH_STAGE_DISCONNECTED, "ethernet lost ip");
        xEventGroupSetBits(s_eth.events, GW_ETH_EVENT_LOST_IP);
    }
}

static esp_err_t ensure_idf(void)
{
    if (s_eth.idf_ready) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_eth.idf_ready = true;
    return ESP_OK;
}

static esp_err_t register_handlers(void)
{
    if (s_eth.handlers_registered) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IOT_ETH_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   ethernet_event_handler,
                                                   NULL),
                        TAG,
                        "register ethernet events failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   ip_event_handler,
                                                   NULL),
                        TAG,
                        "register ethernet IP events failed");
    s_eth.handlers_registered = true;
    return ESP_OK;
}

static esp_err_t configure_power_gpio(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << GW_ETH_PIN_POWER_CTRL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure ethernet power GPIO failed");
    return gpio_set_level((gpio_num_t)GW_ETH_PIN_POWER_CTRL,
                          !GW_ETH_POWER_CTRL_ACTIVE_LEVEL);
}

/*
 * CH397 exposes the Vendor, CDC-ECM and NCM interfaces as separate USB
 * configurations.  The CDC/ECM driver only sees the configuration selected by
 * the USB host during enumeration, so keep the same selection policy as the
 * verified Ethernet implementation: use configuration 2 for multi-config
 * devices and the only configuration for single-config devices.
 */
static bool ethernet_usb_enum_filter_cb(const usb_device_desc_t *dev_desc,
                                       uint8_t *configuration_value)
{
    if (dev_desc == NULL || configuration_value == NULL) {
        return true;
    }

    *configuration_value = dev_desc->bNumConfigurations > 1U ? 2U : 1U;
    ESP_LOGI(TAG,
             "USB enumerate select cfg=%u (vid=%04X pid=%04X num_cfg=%u)",
             (unsigned)*configuration_value,
             (unsigned)dev_desc->idVendor,
             (unsigned)dev_desc->idProduct,
             (unsigned)dev_desc->bNumConfigurations);
    return true;
}

static void usb_host_task(void *arg)
{
    TaskHandle_t waiter = (TaskHandle_t)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = ethernet_usb_enum_filter_cb,
    };
    esp_err_t err = usb_host_install(&config);
    s_eth.usb_host_ready = err == ESP_OK;
    xTaskNotifyGive(waiter);
    if (err != ESP_OK) {
        s_eth.usb_host_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (!s_eth.usb_host_stop) {
        uint32_t flags = 0;
        err = usb_host_lib_handle_events(pdMS_TO_TICKS(50), &flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB host event failed: %s", esp_err_to_name(err));
        }
    }

    (void)usb_host_device_free_all();
    err = usb_host_uninstall();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB host uninstall failed: %s", esp_err_to_name(err));
    }
    s_eth.usb_host_ready = false;
    s_eth.usb_host_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_usb_host(void)
{
    if (s_eth.usb_host_ready) {
        return ESP_OK;
    }
    s_eth.usb_host_stop = false;
    BaseType_t created = xTaskCreatePinnedToCore(usb_host_task,
                                                 "eth_usb_host",
                                                 4096,
                                                 xTaskGetCurrentTaskHandle(),
                                                 GW_ETH_CDC_TASK_PRIORITY,
                                                 &s_eth.usb_host_task,
                                                 0);
    ESP_RETURN_ON_FALSE(created == pdPASS,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "create ethernet USB host task failed");
    ESP_RETURN_ON_FALSE(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000)) != 0U,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "wait ethernet USB host failed");
    return s_eth.usb_host_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t install_cdc_driver(void)
{
    if (s_eth.cdc_ready) {
        return ESP_OK;
    }
    const usbh_cdc_driver_config_t config = {
        .task_stack_size = GW_ETH_CDC_TASK_STACK_BYTES,
        .task_priority = GW_ETH_CDC_TASK_PRIORITY,
        .task_coreid = 0,
        .skip_init_usb_host_driver = true,
    };
    ESP_RETURN_ON_ERROR(usbh_cdc_driver_install(&config),
                        TAG,
                        "install ethernet CDC driver failed");
    s_eth.cdc_ready = true;
    return ESP_OK;
}

static esp_err_t install_ethernet_stack(void)
{
    if (s_eth.eth != NULL) {
        return ESP_OK;
    }
    const iot_usbh_ecm_config_t ecm_config = {
        .match_id_list = ESP_USB_DEVICE_MATCH_ID_ANY,
    };
    iot_eth_config_t eth_config = {0};
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();

    ESP_RETURN_ON_ERROR(iot_eth_new_usb_ecm(&ecm_config, &s_eth.driver),
                        TAG,
                        "create CH397 USB ECM driver failed");
    eth_config.driver = s_eth.driver;
    ESP_RETURN_ON_ERROR(iot_eth_install(&eth_config, &s_eth.eth),
                        TAG,
                        "install CH397 ethernet failed");
    s_eth.netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_eth.netif != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "create ethernet netif failed");
    s_eth.glue = iot_eth_new_netif_glue(s_eth.eth);
    ESP_RETURN_ON_FALSE(s_eth.glue != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "create ethernet netif glue failed");
    return esp_netif_attach(s_eth.netif, s_eth.glue);
}

static esp_err_t cleanup_stack(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_eth.eth_started && s_eth.eth != NULL) {
        esp_err_t err = iot_eth_stop(s_eth.eth);
        if (err != ESP_OK) {
            first_error = err;
        }
        s_eth.eth_started = false;
    }
    if (s_eth.glue != NULL) {
        esp_err_t err = iot_eth_del_netif_glue(s_eth.glue);
        if (first_error == ESP_OK && err != ESP_OK) {
            first_error = err;
        }
        s_eth.glue = NULL;
    }
    if (s_eth.netif != NULL) {
        esp_netif_destroy(s_eth.netif);
        s_eth.netif = NULL;
    }
    if (s_eth.eth != NULL) {
        esp_err_t err = iot_eth_uninstall(s_eth.eth);
        if (first_error == ESP_OK && err != ESP_OK) {
            first_error = err;
        }
        if (err == ESP_OK) {
            s_eth.eth = NULL;
            s_eth.driver = NULL;
        }
    }
    if (s_eth.cdc_ready && s_eth.eth == NULL) {
        esp_err_t err = usbh_cdc_driver_uninstall();
        if (first_error == ESP_OK && err != ESP_OK) {
            first_error = err;
        }
        if (err == ESP_OK) {
            s_eth.cdc_ready = false;
        }
    }
    if (s_eth.usb_host_task != NULL && !s_eth.cdc_ready) {
        s_eth.usb_host_stop = true;
        uint32_t waited_ms = 0U;
        while (s_eth.usb_host_task != NULL && waited_ms < GW_ETH_USB_HOST_RELEASE_MS) {
            vTaskDelay(pdMS_TO_TICKS(GW_ETH_USB_HOST_POLL_MS));
            waited_ms += GW_ETH_USB_HOST_POLL_MS;
        }
        if (s_eth.usb_host_task != NULL && first_error == ESP_OK) {
            first_error = ESP_ERR_TIMEOUT;
        }
    }
    return first_error;
}

esp_err_t gw_ethernet_init(void)
{
    if (s_eth.initialized) {
        return ESP_OK;
    }
    memset(&s_eth, 0, sizeof(s_eth));
    s_eth.events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_eth.events != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "create ethernet event group failed");
    s_eth.status.initialized = true;
    s_eth.status.stage = GW_ETH_STAGE_IDLE;
    copy_text(s_eth.status.reason, sizeof(s_eth.status.reason), "idle");
    ESP_RETURN_ON_ERROR(configure_power_gpio(), TAG, "prepare ethernet power failed");
    s_eth.initialized = true;
    return ESP_OK;
}

esp_err_t gw_ethernet_prepare_usb(void)
{
    ESP_RETURN_ON_ERROR(gw_ethernet_init(), TAG, "ethernet init failed");
    ESP_RETURN_ON_ERROR(ensure_idf(), TAG, "esp netif init failed");
    ESP_RETURN_ON_ERROR(register_handlers(), TAG, "register ethernet handlers failed");
    ESP_RETURN_ON_ERROR(start_usb_host(), TAG, "start ethernet USB host failed");
    return install_cdc_driver();
}

esp_err_t gw_ethernet_start(const gw_ethernet_start_config_t *config)
{
    uint32_t timeout_ms = config != NULL && config->timeout_ms > 0U
                              ? config->timeout_ms
                              : GW_ETH_WAIT_IP_TIMEOUT_MS;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(gw_ethernet_init(), TAG, "ethernet init failed");
    s_eth.stage_cb = config != NULL ? config->stage_cb : NULL;
    s_eth.stage_ctx = config != NULL ? config->stage_ctx : NULL;
    s_eth.stopping = false;
    clear_ip_status();
    xEventGroupClearBits(s_eth.events, GW_ETH_EVENT_GOT_IP | GW_ETH_EVENT_LOST_IP);
    set_stage(GW_ETH_STAGE_STARTING, "ethernet starting");

    err = gw_ethernet_prepare_usb();
    if (err != ESP_OK) {
        goto fail;
    }
    err = install_ethernet_stack();
    if (err != ESP_OK) {
        goto fail;
    }
    err = iot_eth_start(s_eth.eth);
    if (err != ESP_OK) {
        goto fail;
    }
    s_eth.eth_started = true;
    set_stage(GW_ETH_STAGE_WAIT_USB, "ethernet wait usb");

    err = gpio_set_level((gpio_num_t)GW_ETH_PIN_POWER_CTRL,
                         GW_ETH_POWER_CTRL_ACTIVE_LEVEL);
    if (err != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(GW_ETH_POWER_STABLE_MS));

    int64_t usb_deadline = esp_timer_get_time() +
                           (int64_t)GW_ETH_WAIT_USB_TIMEOUT_MS * 1000;
    while (usb_ecm_get_cdc_port_handle(s_eth.driver) == NULL &&
           esp_timer_get_time() < usb_deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (usb_ecm_get_cdc_port_handle(s_eth.driver) == NULL) {
        err = usb_ecm_get_last_connect_error(s_eth.driver);
        if (err == ESP_OK) {
            err = ESP_ERR_NOT_FOUND;
        }
        goto fail;
    }

    set_stage(GW_ETH_STAGE_GETTING_IP, "ethernet getting ip");
    EventBits_t bits = xEventGroupWaitBits(s_eth.events,
                                           GW_ETH_EVENT_GOT_IP |
                                               GW_ETH_EVENT_LOST_IP,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & GW_ETH_EVENT_GOT_IP) == 0U || !s_eth.status.ready) {
        err = ESP_ERR_TIMEOUT;
        goto fail;
    }
    ESP_LOGI(TAG, "CH397 USB ethernet ready");
    return ESP_OK;

fail:
    s_eth.stopping = true;
    (void)gpio_set_level((gpio_num_t)GW_ETH_PIN_POWER_CTRL,
                         !GW_ETH_POWER_CTRL_ACTIVE_LEVEL);
    esp_err_t cleanup_error = cleanup_stack();
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(TAG, "ethernet cleanup failed: %s", esp_err_to_name(cleanup_error));
    }
    clear_ip_status();
    char reason[64];
    snprintf(reason, sizeof(reason), "ethernet:%s", esp_err_to_name(err));
    set_stage(GW_ETH_STAGE_FAILED, reason);
    return err;
}

esp_err_t gw_ethernet_stop(void)
{
    ESP_RETURN_ON_ERROR(gw_ethernet_init(), TAG, "ethernet init failed");
    s_eth.stopping = true;
    (void)gpio_set_level((gpio_num_t)GW_ETH_PIN_POWER_CTRL,
                         !GW_ETH_POWER_CTRL_ACTIVE_LEVEL);
    esp_err_t err = cleanup_stack();
    clear_ip_status();
    set_stage(GW_ETH_STAGE_STOPPED, "stopped");
    return err;
}

esp_err_t gw_ethernet_get_status(gw_ethernet_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(gw_ethernet_init(), TAG, "ethernet init failed");
    portENTER_CRITICAL(&s_eth_lock);
    *out = s_eth.status;
    portEXIT_CRITICAL(&s_eth_lock);
    return ESP_OK;
}

const char *gw_ethernet_stage_to_string(gw_ethernet_stage_t stage)
{
    switch (stage) {
    case GW_ETH_STAGE_IDLE: return "idle";
    case GW_ETH_STAGE_STARTING: return "starting";
    case GW_ETH_STAGE_WAIT_USB: return "wait_usb";
    case GW_ETH_STAGE_GETTING_IP: return "getting_ip";
    case GW_ETH_STAGE_READY: return "ready";
    case GW_ETH_STAGE_DISCONNECTED: return "disconnected";
    case GW_ETH_STAGE_FAILED: return "failed";
    case GW_ETH_STAGE_STOPPED: return "stopped";
    default: return "unknown";
    }
}
