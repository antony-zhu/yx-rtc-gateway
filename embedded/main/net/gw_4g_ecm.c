#include "gw_4g_ecm.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_ecm.h"
#include "lwip/ip4_addr.h"
#include "usb/usb_host.h"

#include "gw_defaults.h"
#include "gw_event.h"
#include "gw_result.h"

static const char *TAG = "gw_4g_ecm";

#define GW_4G_EVENT_USB_CONNECTED     BIT0
#define GW_4G_EVENT_USB_DISCONNECTED  BIT1
#define GW_4G_EVENT_GOT_IP            BIT2
#define GW_4G_EVENT_LOST_IP           BIT3
#define GW_4G_EVENT_USB_OPEN_FAILED   BIT4
#define GW_4G_NET_LOST_RETRY_US       (250ULL * 1000ULL)

typedef struct {
    bool initialized;
    bool idf_ready;
    bool event_handlers_registered;

    EventGroupHandle_t events;
    SemaphoreHandle_t at_lock;

    gw_4g_ecm_status_t status;

    esp_netif_t *ecm_netif;
    iot_eth_driver_t *ecm_driver;
    iot_eth_handle_t ecm_eth;
    iot_eth_netif_glue_handle_t ecm_glue;
    usb_device_match_id_t ecm_match[2];
    bool ecm_stack_ready;
    bool ecm_started;

    usbh_cdc_port_handle_t cdc_port;
    usb_device_match_id_t cdc_match[2];
    TaskHandle_t usb_lib_task;
    volatile bool usb_host_ready;
    volatile bool usb_stop_requested;
    bool cdc_driver_ready;
    bool cdc_callback_registered;
    volatile bool cdc_connected;
    volatile esp_err_t cdc_open_error;
    uint8_t cdc_dev_addr;
    uint8_t cdc_itf_num;

    esp_timer_handle_t net_lost_retry_timer;
    bool net_lost_latched;
    bool net_lost_posting;
    bool net_lost_delivered;
    bool net_lost_suppressed;
    bool power_baseline_ready;
    uint32_t consecutive_start_failures;
} gw_4g_ecm_ctx_t;

static gw_4g_ecm_ctx_t s_4g;
static portMUX_TYPE s_net_lost_lock = portMUX_INITIALIZER_UNLOCKED;

static void clear_net_loss_latch(void);
static esp_err_t ecm_stop(void);
static esp_err_t ecm_stack_destroy(void);

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void copy_text(char *out, size_t out_size, const char *text)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    (void)snprintf(out, out_size, "%s", text != NULL ? text : "");
}

static void ip4_to_text(const esp_ip4_addr_t *ip, char *out, size_t out_size)
{
    if (ip == NULL || out == NULL || out_size == 0U) {
        return;
    }
    (void)snprintf(out, out_size, IPSTR, IP2STR(ip));
}

static void set_stage(gw_4g_ecm_stage_t stage,
                      const char *reason,
                      const gw_4g_ecm_start_config_t *config)
{
    s_4g.status.stage = stage;
    if (stage == GW_4G_ECM_STAGE_READY) {
        s_4g.status.ready = true;
    } else if (stage == GW_4G_ECM_STAGE_FAILED ||
               stage == GW_4G_ECM_STAGE_STOPPED ||
               stage == GW_4G_ECM_STAGE_IDLE) {
        s_4g.status.ready = false;
    }
    if (reason != NULL) {
        copy_text(s_4g.status.reason, sizeof(s_4g.status.reason), reason);
    }
    if (config != NULL && config->stage_cb != NULL) {
        config->stage_cb(stage, s_4g.status.reason, config->stage_ctx);
    }
}

static void clear_ip_status(void)
{
    s_4g.status.ready = false;
    s_4g.status.ip[0] = '\0';
    s_4g.status.netmask[0] = '\0';
    s_4g.status.gateway[0] = '\0';
    s_4g.status.dns0[0] = '\0';
    s_4g.status.dns1[0] = '\0';
}

static void update_dns(esp_netif_t *netif)
{
    esp_netif_dns_info_t dns = {0};

    if (netif == NULL) {
        return;
    }
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4) {
        ip4_to_text(&dns.ip.u_addr.ip4, s_4g.status.dns0, sizeof(s_4g.status.dns0));
    }
    memset(&dns, 0, sizeof(dns));
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4) {
        ip4_to_text(&dns.ip.u_addr.ip4, s_4g.status.dns1, sizeof(s_4g.status.dns1));
    }
}

static void update_ip_status(esp_netif_t *netif, const esp_netif_ip_info_t *ip_info)
{
    if (netif == NULL || ip_info == NULL) {
        return;
    }
    ip4_to_text(&ip_info->ip, s_4g.status.ip, sizeof(s_4g.status.ip));
    ip4_to_text(&ip_info->netmask, s_4g.status.netmask, sizeof(s_4g.status.netmask));
    ip4_to_text(&ip_info->gw, s_4g.status.gateway, sizeof(s_4g.status.gateway));
    update_dns(netif);
    s_4g.status.ready = true;
    clear_net_loss_latch();
}

/** @brief 清除本轮断网锁存；新 IP 到达或显式重新启动时调用。 */
static void clear_net_loss_latch(void)
{
    portENTER_CRITICAL(&s_net_lost_lock);
    s_4g.net_lost_latched = false;
    s_4g.net_lost_delivered = false;
    portEXIT_CRITICAL(&s_net_lost_lock);

    if (s_4g.net_lost_retry_timer != NULL) {
        (void)esp_timer_stop(s_4g.net_lost_retry_timer);
    }
}

/** @brief 开关主动停网期间的 NET_LOST 抑制，避免 stop 产生伪断网重试。 */
static void suppress_net_loss(bool suppressed)
{
    portENTER_CRITICAL(&s_net_lost_lock);
    s_4g.net_lost_suppressed = suppressed;
    s_4g.net_lost_latched = false;
    s_4g.net_lost_delivered = false;
    portEXIT_CRITICAL(&s_net_lost_lock);

    if (s_4g.net_lost_retry_timer != NULL) {
        (void)esp_timer_stop(s_4g.net_lost_retry_timer);
    }
}

/** @brief 尝试把已锁存的 NET_LOST 投递给 supervisor；队满时由定时器继续补投。 */
static void try_post_latched_net_lost(void)
{
    bool should_post = false;

    portENTER_CRITICAL(&s_net_lost_lock);
    if (s_4g.net_lost_latched &&
        !s_4g.net_lost_delivered &&
        !s_4g.net_lost_posting) {
        s_4g.net_lost_posting = true;
        should_post = true;
    }
    portEXIT_CRITICAL(&s_net_lost_lock);
    if (!should_post) {
        return;
    }

    gw_event_t event = {
        .event_id = GW_EVT_NET_LOST,
        /*
         * 公共队列为 HIGH/CRITICAL 预留容量；断网事件使用 HIGH，降低首次投递
         * 被普通业务突发占满的概率，锁存定时器仍负责极端情况下的补投。
         */
        .priority = GW_EVENT_PRIORITY_HIGH,
        .source = "4g_ecm",
        .target = "dev_mgr_task",
        .request_id = NULL,
        .method = NULL,
        .uptime_ms = (uint64_t)now_ms(),
        .timestamp = 0,
        .result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                  GW_REASON_NETWORK_UNAVAILABLE,
                                  "4g network lost"),
        .payload = NULL,
        .payload_len = 0,
        .release_cb = NULL,
    };

    esp_err_t err = gw_event_post(&event);
    bool retry_needed;
    portENTER_CRITICAL(&s_net_lost_lock);
    s_4g.net_lost_posting = false;
    if (err == ESP_OK && s_4g.net_lost_latched) {
        s_4g.net_lost_delivered = true;
    }
    retry_needed = s_4g.net_lost_latched && !s_4g.net_lost_delivered;
    portEXIT_CRITICAL(&s_net_lost_lock);

    if (!retry_needed) {
        return;
    }

    ESP_LOGW(TAG, "post net lost failed, retry scheduled: %s", esp_err_to_name(err));
    if (s_4g.net_lost_retry_timer != NULL) {
        (void)esp_timer_stop(s_4g.net_lost_retry_timer);
        esp_err_t timer_err = esp_timer_start_once(s_4g.net_lost_retry_timer,
                                                   GW_4G_NET_LOST_RETRY_US);
        if (timer_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "schedule net lost retry failed: %s",
                     esp_err_to_name(timer_err));
        }
    }
}

/** @brief esp_timer 任务回调，只负责重试一次非阻塞事件投递。 */
static void net_lost_retry_timer_cb(void *arg)
{
    (void)arg;
    try_post_latched_net_lost();
}

/**
 * @brief 锁存一次真实断网并触发可靠投递。
 *
 * Ethernet disconnect 与 lost-IP 通常会成对到达；锁存位保证同一轮只向 supervisor
 * 交付一条 NET_LOST，避免重复增加指数退避次数。
 */
static void post_net_lost_event(const char *detail)
{
    (void)detail;
    bool first_loss = false;
    portENTER_CRITICAL(&s_net_lost_lock);
    if (!s_4g.net_lost_suppressed && !s_4g.net_lost_latched) {
        s_4g.net_lost_latched = true;
        s_4g.net_lost_delivered = false;
        first_loss = true;
    }
    portEXIT_CRITICAL(&s_net_lost_lock);

    if (first_loss) {
        try_post_latched_net_lost();
    }
}

static esp_err_t ensure_idf(void)
{
    esp_err_t err = ESP_OK;

    if (s_4g.idf_ready) {
        return ESP_OK;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_4g.idf_ready = true;
    return ESP_OK;
}

static void iot_eth_event_handler(void *arg,
                                  esp_event_base_t event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == IOT_ETH_EVENT_CONNECTED) {
        copy_text(s_4g.status.reason, sizeof(s_4g.status.reason), "ecm link up");
    } else if (event_id == IOT_ETH_EVENT_DISCONNECTED) {
        bool was_ready = s_4g.status.ready || s_4g.status.stage == GW_4G_ECM_STAGE_READY;
        clear_ip_status();
        copy_text(s_4g.status.reason, sizeof(s_4g.status.reason), "ecm link down");
        if (s_4g.events != NULL) {
            xEventGroupSetBits(s_4g.events, GW_4G_EVENT_LOST_IP);
        }
        if (was_ready) {
            post_net_lost_event("ecm link down");
        }
    } else if (event_id == IOT_ETH_EVENT_STOP) {
        /*
         * 启动失败后的清理会主动停止 ECM。此时不能用清理事件覆盖真正的失败阶段，
         * 否则上层只能看到“ecm stopped”，无法区分 USB 枚举、SIM 或 IP 超时。
         */
        if (s_4g.status.stage != GW_4G_ECM_STAGE_FAILED) {
            copy_text(s_4g.status.reason, sizeof(s_4g.status.reason), "ecm stopped");
        }
    } else if (event_id == IOT_ETH_EVENT_START) {
        if (s_4g.status.stage != GW_4G_ECM_STAGE_FAILED) {
            copy_text(s_4g.status.reason, sizeof(s_4g.status.reason), "ecm started");
        }
    }
}

static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;

    (void)arg;
    (void)event_base;

    if (event_id == IP_EVENT_ETH_GOT_IP &&
        got_ip != NULL &&
        got_ip->esp_netif == s_4g.ecm_netif) {
        update_ip_status(s_4g.ecm_netif, &got_ip->ip_info);
        copy_text(s_4g.status.reason, sizeof(s_4g.status.reason), "ecm got ip");
        if (s_4g.events != NULL) {
            xEventGroupSetBits(s_4g.events, GW_4G_EVENT_GOT_IP);
        }
        ESP_LOGI(TAG,
                 "4g got ip=%s netmask=%s gateway=%s dns0=%s dns1=%s",
                 s_4g.status.ip,
                 s_4g.status.netmask,
                 s_4g.status.gateway,
                 s_4g.status.dns0[0] != '\0' ? s_4g.status.dns0 : "-",
                 s_4g.status.dns1[0] != '\0' ? s_4g.status.dns1 : "-");
        return;
    }

    if (event_id == IP_EVENT_ETH_LOST_IP) {
        bool was_ready = s_4g.status.ready || s_4g.status.stage == GW_4G_ECM_STAGE_READY;
        clear_ip_status();
        copy_text(s_4g.status.reason, sizeof(s_4g.status.reason), "ecm lost ip");
        if (s_4g.events != NULL) {
            xEventGroupSetBits(s_4g.events, GW_4G_EVENT_LOST_IP);
        }
        if (was_ready) {
            post_net_lost_event("ecm lost ip");
        }
    }
}

static esp_err_t register_event_handlers(void)
{
    if (s_4g.event_handlers_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IOT_ETH_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   iot_eth_event_handler,
                                                   NULL),
                        TAG,
                        "register iot eth event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_ETH_GOT_IP,
                                                   ip_event_handler,
                                                   NULL),
                        TAG,
                        "register got ip event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_ETH_LOST_IP,
                                                   ip_event_handler,
                                                   NULL),
                        TAG,
                        "register lost ip event failed");
    s_4g.event_handlers_registered = true;
    return ESP_OK;
}

static esp_err_t configure_gpio(gpio_num_t pin,
                                gpio_mode_t mode,
                                gpio_pullup_t pullup,
                                gpio_pulldown_t pulldown)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = mode,
        .pull_up_en = pullup,
        .pull_down_en = pulldown,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&io_conf);
}

static esp_err_t port_init(void)
{
    ESP_RETURN_ON_ERROR(configure_gpio((gpio_num_t)GW_4G_PIN_RESET,
                                       GPIO_MODE_OUTPUT,
                                       GPIO_PULLUP_DISABLE,
                                       GPIO_PULLDOWN_ENABLE),
                        TAG,
                        "config 4g reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)GW_4G_PIN_RESET, 0),
                        TAG,
                        "set 4g reset failed");
    ESP_RETURN_ON_ERROR(configure_gpio((gpio_num_t)GW_4G_PIN_POWER_CTRL,
                                       GPIO_MODE_OUTPUT,
                                       GPIO_PULLUP_DISABLE,
                                       GPIO_PULLDOWN_ENABLE),
                        TAG,
                        "config 4g power ctrl failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_CTRL, 0),
                        TAG,
                        "set 4g power ctrl failed");
    ESP_RETURN_ON_ERROR(configure_gpio((gpio_num_t)GW_4G_PIN_DETECT,
                                       GPIO_MODE_INPUT,
                                       GPIO_PULLUP_ENABLE,
                                       GPIO_PULLDOWN_DISABLE),
                        TAG,
                        "config 4g detect failed");
    ESP_RETURN_ON_ERROR(configure_gpio((gpio_num_t)GW_4G_PIN_POWER_EN,
                                       GPIO_MODE_OUTPUT,
                                       GPIO_PULLUP_DISABLE,
                                       GPIO_PULLDOWN_ENABLE),
                        TAG,
                        "config 4g power en failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_EN, 0),
                        TAG,
                        "set 4g power en failed");
    return ESP_OK;
}

static bool is_detected(void)
{
    return gpio_get_level((gpio_num_t)GW_4G_PIN_DETECT) == 0;
}

static void power_on(void)
{
    ESP_LOGI(TAG, "4g power on");
    s_4g.power_baseline_ready = false;
    (void)gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_CTRL, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /*
     * 当前硬件需要 POWER_CTRL 先拉高，再给 POWER_EN 一个固定宽度脉冲。
     * 该时序来自旧工程已验证实现，避免 4G 模组偶发不上电。
     */
    (void)gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(GW_4G_POWER_ON_DELAY_MS));
    (void)gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_EN, 0);
}

static void power_off(void)
{
    ESP_LOGI(TAG, "4g power off");
    (void)gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_EN, 1);
    (void)gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_CTRL, 0);

    /*
     * POWER_CTRL 拉低后必须给模组和 USB 端点足够的放电、离线时间。旧实现立即
     * 开始下一轮，CDC/ECM client 会继续保留上一代设备对象，最终形成无限枚举
     * 超时。等待结束后恢复 POWER_EN 的空闲电平，确保下一轮得到完整上电脉冲。
     */
    vTaskDelay(pdMS_TO_TICKS(GW_4G_POWER_OFF_SETTLE_MS));
    (void)gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_EN, 0);
    s_4g.power_baseline_ready = true;
}

/** @brief 连续普通恢复仍失败时，对已上电模组执行一次硬件复位。 */
static void hardware_reset(void)
{
    ESP_LOGW(TAG,
             "4g consecutive failures=%" PRIu32 ", pulse hardware reset",
             s_4g.consecutive_start_failures);
    (void)gpio_set_level((gpio_num_t)GW_4G_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(GW_4G_RESET_PULSE_MS));
    (void)gpio_set_level((gpio_num_t)GW_4G_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(GW_4G_RESET_DELAY_MS));
}

static bool usb_enum_filter_cb(const usb_device_desc_t *dev_desc, uint8_t *configuration_value)
{
    if (dev_desc == NULL || configuration_value == NULL) {
        return true;
    }

    /*
     * 4G 模组有时把完整 ECM 配置放在第二个配置描述符。
     * 枚举阶段优先选择最后一个配置，和旧工程已验证行为保持一致。
     */
    *configuration_value = (dev_desc->bNumConfigurations > 1) ? 2 : 1;
    ESP_LOGI(TAG,
             "usb enum select cfg=%u (num_cfg=%u)",
             (unsigned)*configuration_value,
             (unsigned)dev_desc->bNumConfigurations);
    return true;
}

static void usb_lib_task(void *arg)
{
    TaskHandle_t notify_task = (TaskHandle_t)arg;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = usb_enum_filter_cb,
    };
    bool has_clients = true;
    bool has_devices = false;
    esp_err_t ret = usb_host_install(&host_config);

    s_4g.usb_host_ready = (ret == ESP_OK);
    xTaskNotifyGive(notify_task);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(ret));
        s_4g.usb_lib_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (has_clients) {
        uint32_t event_flags = 0;

        ret = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(ret));
            continue;
        }
        if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0U) {
            if (usb_host_device_free_all() == ESP_OK) {
                has_clients = false;
            } else {
                has_devices = true;
            }
        }
        if (has_devices && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0U) {
            has_clients = false;
        }

        /*
         * CDC 可能在注册 USB client 之前就创建失败，此时不会产生 NO_CLIENTS
         * 边沿。显式释放请求覆盖该分支，确保空 Host 也能退出。
         */
        if (s_4g.usb_stop_requested) {
            usb_host_lib_info_t info = {0};
            if (usb_host_lib_info(&info) == ESP_OK && info.num_clients == 0) {
                if (usb_host_device_free_all() == ESP_OK) {
                    has_clients = false;
                } else {
                    has_devices = true;
                }
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    (void)usb_host_uninstall();
    s_4g.usb_host_ready = false;
    s_4g.usb_stop_requested = false;
    s_4g.usb_lib_task = NULL;
    ESP_LOGI(TAG, "usb host library uninstalled");
    vTaskDelete(NULL);
}

static bool wait_usb_host_release(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    if (s_4g.usb_lib_task != NULL) {
        s_4g.usb_stop_requested = true;
    }
    while (s_4g.usb_lib_task != NULL && waited_ms < timeout_ms) {
        (void)usb_host_lib_unblock();
        vTaskDelay(pdMS_TO_TICKS(GW_4G_USB_HOST_POLL_MS));
        waited_ms += GW_4G_USB_HOST_POLL_MS;
    }
    return s_4g.usb_lib_task == NULL;
}

static esp_err_t install_usb_host_library(void)
{
    uint32_t notify_value = 0;
    BaseType_t ok = pdFAIL;

    if (s_4g.usb_lib_task != NULL) {
        if (s_4g.usb_host_ready && !s_4g.usb_stop_requested) {
            return ESP_OK;
        }
        if (!wait_usb_host_release(GW_4G_USB_HOST_RELEASE_MS)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_4g.usb_host_ready = false;
    s_4g.usb_stop_requested = false;
    ok = xTaskCreatePinnedToCore(usb_lib_task,
                                 "gw_4g_usb",
                                 4096,
                                 xTaskGetCurrentTaskHandle(),
                                 GW_4G_CDC_TASK_PRIORITY,
                                 &s_4g.usb_lib_task,
                                 0);
    if (ok != pdPASS) {
        s_4g.usb_lib_task = NULL;
        s_4g.usb_stop_requested = false;
        return ESP_ERR_NO_MEM;
    }

    notify_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
    if (notify_value == 0U || !s_4g.usb_host_ready) {
        (void)wait_usb_host_release(GW_4G_USB_HOST_RELEASE_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void cdc_port_closed_cb(usbh_cdc_port_handle_t cdc_port_handle, void *user_data)
{
    (void)user_data;

    /*
     * closed 回调在组件解锁后执行；若新一代 USB 端口已先完成打开，旧回调不能
     * 清掉新句柄。只处理与当前保存句柄一致的关闭通知。
     */
    if (s_4g.cdc_port != cdc_port_handle) {
        return;
    }
    ESP_LOGW(TAG, "4g at cdc port closed");
    s_4g.cdc_port = NULL;
    s_4g.cdc_dev_addr = 0;
    s_4g.cdc_connected = false;
    if (s_4g.events != NULL) {
        xEventGroupSetBits(s_4g.events, GW_4G_EVENT_USB_DISCONNECTED);
    }
}

static void cdc_device_event_cb(usbh_cdc_device_event_t event,
                                usbh_cdc_device_event_data_t *event_data,
                                void *user_data)
{
    (void)user_data;

    switch (event) {
    case CDC_HOST_DEVICE_EVENT_CONNECTED: {
        usbh_cdc_port_config_t port_config = {0};
        int matched_itf_num = -1;
        esp_err_t ret = ESP_OK;

        if (event_data == NULL || s_4g.cdc_port != NULL) {
            return;
        }
        matched_itf_num = event_data->new_dev.matched_intf_num;
        if (matched_itf_num < 0) {
            matched_itf_num = (int)s_4g.cdc_itf_num;
        }

        port_config.dev_addr = event_data->new_dev.dev_addr;
        port_config.itf_num = (uint8_t)matched_itf_num;
        port_config.in_ringbuf_size = GW_4G_CDC_RX_BUFFER_BYTES;
        port_config.out_ringbuf_size = GW_4G_CDC_TX_BUFFER_BYTES;
        port_config.in_transfer_buffer_size = GW_4G_CDC_TRANSFER_BUFFER_BYTES;
        port_config.out_transfer_buffer_size = GW_4G_CDC_TRANSFER_BUFFER_BYTES;
        port_config.cbs.closed = cdc_port_closed_cb;

        ret = usbh_cdc_port_open(&port_config, &s_4g.cdc_port);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "open 4g at cdc port failed: %s", esp_err_to_name(ret));
            s_4g.cdc_port = NULL;
            s_4g.cdc_open_error = ret;
            xEventGroupSetBits(s_4g.events, GW_4G_EVENT_USB_OPEN_FAILED);
            return;
        }

        s_4g.cdc_open_error = ESP_OK;
        s_4g.cdc_dev_addr = event_data->new_dev.dev_addr;
        s_4g.cdc_itf_num = (uint8_t)matched_itf_num;
        s_4g.cdc_connected = true;
        ESP_LOGI(TAG,
                 "4g at cdc connected dev=%u itf=%u",
                 (unsigned)s_4g.cdc_dev_addr,
                 (unsigned)s_4g.cdc_itf_num);
        xEventGroupSetBits(s_4g.events, GW_4G_EVENT_USB_CONNECTED);
        break;
    }
    case CDC_HOST_DEVICE_EVENT_DISCONNECTED:
        /*
         * 模组可能先以地址 1 枚举引导接口，再以地址 2 枚举正式复合设备。
         * 未打开 AT 端口时的 DEV_GONE 不属于本连接，不能误报 USB 断线。
         */
        if (event_data == NULL ||
            s_4g.cdc_dev_addr == 0U ||
            s_4g.cdc_dev_addr != event_data->dev_gone.dev_addr) {
            return;
        }
        ESP_LOGW(TAG, "4g usb disconnected");
        s_4g.cdc_dev_addr = 0;
        s_4g.cdc_connected = false;
        /*
         * 保留端口句柄直到同步 closed 回调清除，确保恢复任务调用 port_close 时
         * 会等待 CDC task 完成资源回收，而不是越过尚在进行的关闭流程。
         */
        xEventGroupSetBits(s_4g.events, GW_4G_EVENT_USB_DISCONNECTED);
        break;
    default:
        break;
    }
}

static void build_cdc_match(void)
{
    memset(s_4g.cdc_match, 0, sizeof(s_4g.cdc_match));
    s_4g.cdc_match[0].match_flags = USB_DEVICE_ID_MATCH_VENDOR |
                                    USB_DEVICE_ID_MATCH_PRODUCT |
                                    USB_DEVICE_ID_MATCH_INT_NUMBER;
    s_4g.cdc_match[0].idVendor = GW_4G_USB_VID;
    s_4g.cdc_match[0].idProduct = GW_4G_USB_PID;
    s_4g.cdc_match[0].bInterfaceNumber = s_4g.cdc_itf_num;
}

static esp_err_t cdc_init(void)
{
    usbh_cdc_driver_config_t drv_config = {0};
    esp_err_t ret = ESP_OK;

    if (s_4g.cdc_itf_num == 0U) {
        s_4g.cdc_itf_num = GW_4G_USB_AT_INTERFACE;
    }
    if (s_4g.cdc_driver_ready && s_4g.cdc_callback_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(install_usb_host_library(), TAG, "install usb host failed");
    build_cdc_match();

    if (!s_4g.cdc_driver_ready) {
        drv_config.task_stack_size = GW_4G_CDC_TASK_STACK_BYTES;
        drv_config.task_priority = GW_4G_CDC_TASK_PRIORITY;
        drv_config.task_coreid = 0;
        drv_config.skip_init_usb_host_driver = true;

        ret = usbh_cdc_driver_install(&drv_config);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "install cdc driver failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_4g.cdc_driver_ready = true;
    }

    if (!s_4g.cdc_callback_registered) {
        ret = usbh_cdc_register_dev_event_cb(s_4g.cdc_match, cdc_device_event_cb, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register cdc event failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_4g.cdc_callback_registered = true;
    }
    return ESP_OK;
}

static int cdc_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;

    if (!s_4g.cdc_connected || s_4g.cdc_port == NULL || data == NULL) {
        return -1;
    }

    ret = usbh_cdc_write_bytes(s_4g.cdc_port, data, len, pdMS_TO_TICKS(timeout_ms));
    return ret == ESP_OK ? (int)len : -1;
}

static int cdc_read(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);

    if (!s_4g.cdc_connected || s_4g.cdc_port == NULL || data == NULL) {
        return -1;
    }

    while (true) {
        size_t available = 0;
        size_t read_len = 0;
        esp_err_t ret = usbh_cdc_get_rx_buffer_size(s_4g.cdc_port, &available);

        if (ret != ESP_OK) {
            return -1;
        }
        if (available > 0U) {
            read_len = (available < len) ? available : len;
            ret = usbh_cdc_read_bytes(s_4g.cdc_port, data, &read_len, 0);
            if (ret == ESP_OK) {
                return (int)read_len;
            }
            return ret == ESP_ERR_TIMEOUT ? 0 : -1;
        }
        if (timeout_ms == 0U || esp_timer_get_time() >= deadline_us) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void at_drain_rx(void)
{
    uint8_t tmp[64] = {0};

    while (cdc_read(tmp, sizeof(tmp), 0) > 0) {
    }
}

static void sanitize_response(char *text)
{
    size_t read_index = 0;
    size_t write_index = 0;
    bool previous_space = false;

    if (text == NULL) {
        return;
    }

    while (text[read_index] != '\0') {
        char c = text[read_index++];

        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
        if (c == ' ') {
            if (previous_space) {
                continue;
            }
            previous_space = true;
        } else {
            previous_space = false;
        }
        text[write_index++] = c;
    }
    while (write_index > 0U && text[write_index - 1U] == ' ') {
        write_index--;
    }
    text[write_index] = '\0';
}

static esp_err_t at_command_once(const char *command,
                                 const char *expect,
                                 uint32_t timeout_ms,
                                 char *response,
                                 size_t response_size)
{
    char cmd[160] = {0};
    char reply[GW_4G_AT_RESPONSE_BUFFER_BYTES] = {0};
    size_t cmd_len = 0;
    int reply_len = 0;
    int64_t start_ms = 0;

    if (command == NULL || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    cmd_len = strlen(command);
    if (cmd_len >= (sizeof(cmd) - 3U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    (void)snprintf(cmd, sizeof(cmd), "%s", command);
    if (cmd_len < 2U || cmd[cmd_len - 2U] != '\r' || cmd[cmd_len - 1U] != '\n') {
        cmd[cmd_len++] = '\r';
        cmd[cmd_len++] = '\n';
        cmd[cmd_len] = '\0';
    }

    at_drain_rx();
    if (cdc_write((const uint8_t *)cmd, cmd_len, 200) < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    start_ms = now_ms();
    while ((now_ms() - start_ms) < (int64_t)timeout_ms) {
        int read_len = cdc_read((uint8_t *)(reply + reply_len),
                                sizeof(reply) - (size_t)reply_len - 1U,
                                GW_4G_AT_READ_TIMEOUT_MS);
        if (read_len < 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (read_len > 0) {
            reply_len += read_len;
            reply[reply_len] = '\0';

            if ((expect != NULL && strstr(reply, expect) != NULL) ||
                (expect == NULL && strstr(reply, "\r\nOK\r\n") != NULL)) {
                if (response != NULL && response_size > 0U) {
                    copy_text(response, response_size, reply);
                }
                return ESP_OK;
            }
            if (strstr(reply, "\r\nERROR\r\n") != NULL || strstr(reply, "NO CARRIER") != NULL) {
                if (response != NULL && response_size > 0U) {
                    copy_text(response, response_size, reply);
                }
                return ESP_FAIL;
            }

            /*
             * AT 响应超长时保留尾部窗口，避免栈缓冲区溢出，同时继续匹配 OK/ERROR。
             */
            if (reply_len > (int)(sizeof(reply) - 96U)) {
                const int keep_len = 96;
                memmove(reply, reply + reply_len - keep_len, keep_len);
                reply_len = keep_len;
                reply[reply_len] = '\0';
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (response != NULL && response_size > 0U) {
        copy_text(response, response_size, reply);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t at_command(const char *command,
                            const char *expect,
                            uint32_t timeout_ms,
                            uint32_t retry_times,
                            char *response,
                            size_t response_size)
{
    esp_err_t err = ESP_ERR_TIMEOUT;
    const char *log_command = command != NULL ? command : "<null>";

    if (command == NULL || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_4g.at_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_4g.at_lock, pdMS_TO_TICKS(timeout_ms + 1000U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_4g.status.at_busy = true;
    for (uint32_t i = 0; i < retry_times; ++i) {
        err = at_command_once(command, expect, timeout_ms, response, response_size);
        if (err == ESP_OK) {
            break;
        }
        if (response != NULL && response[0] != '\0') {
            char short_rsp[GW_4G_TEXT_LONG_LEN] = {0};

            copy_text(short_rsp, sizeof(short_rsp), response);
            sanitize_response(short_rsp);
            ESP_LOGW(TAG, "AT failed cmd=%s rsp=%s", log_command, short_rsp);
        } else {
            ESP_LOGW(TAG, "AT failed cmd=%s err=%s", log_command, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_4g.status.at_busy = false;
    (void)xSemaphoreGive(s_4g.at_lock);
    return err;
}

/**
 * @brief 只发送一条会触发 USB 断开的 AT 命令，不等待响应。
 *
 * 模组重启命令可能在回复 OK 前直接移除 CDC 端点。命令成功进入 USB OUT 队列后
 * 就立即停止访问端口，后续以 DEV_GONE 和重新枚举为判据，避免读线程与端口释放
 * 并发造成 UAF。
 */
static esp_err_t at_send_no_response(const char *command)
{
    char cmd[160] = {0};
    size_t cmd_len = 0;
    esp_err_t err = ESP_OK;

    if (command == NULL || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_4g.at_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    cmd_len = strlen(command);
    if (cmd_len >= (sizeof(cmd) - 3U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    (void)snprintf(cmd, sizeof(cmd), "%s", command);
    if (cmd_len < 2U || cmd[cmd_len - 2U] != '\r' || cmd[cmd_len - 1U] != '\n') {
        cmd[cmd_len++] = '\r';
        cmd[cmd_len++] = '\n';
        cmd[cmd_len] = '\0';
    }

    if (xSemaphoreTake(s_4g.at_lock, pdMS_TO_TICKS(1200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_4g.status.at_busy = true;
    at_drain_rx();
    if (cdc_write((const uint8_t *)cmd, cmd_len, 200) < 0) {
        err = ESP_ERR_INVALID_STATE;
    }
    s_4g.status.at_busy = false;
    (void)xSemaphoreGive(s_4g.at_lock);
    return err;
}

static int parse_reg_stat(const char *response, const char *prefix)
{
    const char *cursor = NULL;
    int last_value = -1;

    if (response == NULL || prefix == NULL) {
        return -1;
    }
    cursor = strstr(response, prefix);
    if (cursor == NULL) {
        return -1;
    }
    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        return -1;
    }
    cursor++;

    while (*cursor != '\0' && *cursor != '\r' && *cursor != '\n') {
        char *end = NULL;
        long value = 0;

        while (*cursor == ' ' || *cursor == ',') {
            cursor++;
        }
        if (*cursor < '0' || *cursor > '9') {
            cursor++;
            continue;
        }
        value = strtol(cursor, &end, 10);
        if (end == cursor) {
            break;
        }
        last_value = (int)value;
        cursor = end;
    }
    return last_value;
}

static void trim_copy(char *out, size_t out_size, const char *start, size_t len)
{
    while (len > 0U && (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t' || *start == '"')) {
        start++;
        len--;
    }
    while (len > 0U) {
        char c = start[len - 1U];

        if (c != ' ' && c != '\r' && c != '\n' && c != '\t' && c != '"') {
            break;
        }
        len--;
    }
    if (out == NULL || out_size == 0U) {
        return;
    }
    if (len >= out_size) {
        len = out_size - 1U;
    }
    memcpy(out, start, len);
    out[len] = '\0';
}

static void extract_payload_line(const char *raw, char *out, size_t out_size)
{
    const char *line = raw;

    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (raw == NULL) {
        return;
    }

    while (*line != '\0') {
        const char *next = strpbrk(line, "\r\n");
        size_t len = next != NULL ? (size_t)(next - line) : strlen(line);
        char tmp[GW_4G_TEXT_LONG_LEN] = {0};

        trim_copy(tmp, sizeof(tmp), line, len);
        if (tmp[0] != '\0' &&
            strncmp(tmp, "AT", 2) != 0 &&
            strcmp(tmp, "OK") != 0 &&
            strcmp(tmp, "ERROR") != 0) {
            copy_text(out, out_size, tmp);
            return;
        }
        if (next == NULL) {
            break;
        }
        line = next + 1;
    }
}

static void extract_cops(const char *raw, char *out, size_t out_size)
{
    const char *start = strstr(raw != NULL ? raw : "", "+COPS:");

    if (start == NULL) {
        return;
    }
    start = strchr(start, '"');
    if (start == NULL) {
        return;
    }
    start++;
    const char *end = strchr(start, '"');
    if (end == NULL) {
        return;
    }
    trim_copy(out, out_size, start, (size_t)(end - start));
}

static void update_signal(const char *raw)
{
    const char *p = strstr(raw != NULL ? raw : "", "+CSQ:");

    if (p == NULL) {
        return;
    }
    if (sscanf(p, "+CSQ: %d,%d", &s_4g.status.modem.rssi, &s_4g.status.modem.ber) != 2) {
        (void)sscanf(p, "+CSQ:%d,%d", &s_4g.status.modem.rssi, &s_4g.status.modem.ber);
    }
}

static esp_err_t query_text(const char *cmd, char *out, size_t out_size)
{
    char raw[GW_4G_AT_RESPONSE_BUFFER_BYTES] = {0};
    esp_err_t err = at_command(cmd, "\r\nOK\r\n", 2000, 1, raw, sizeof(raw));

    if (err == ESP_OK) {
        extract_payload_line(raw, out, out_size);
    }
    return err;
}

#ifdef GW_4G_ECM_ENABLE_TEST_HOOKS
/*
 * 单元测试钩子：仅在独立 Unity 测试工程中启用，用于覆盖 AT 响应解析
 * 这类纯逻辑边界，不进入 USB、CDC 或真实拨号流程。
 */
int gw_4g_ecm_test_parse_reg_stat(const char *response, const char *prefix)
{
    return parse_reg_stat(response, prefix);
}

void gw_4g_ecm_test_sanitize_response(char *text)
{
    sanitize_response(text);
}

void gw_4g_ecm_test_extract_payload_line(const char *raw, char *out, size_t out_size)
{
    extract_payload_line(raw, out, out_size);
}

void gw_4g_ecm_test_extract_cops(const char *raw, char *out, size_t out_size)
{
    extract_cops(raw, out, out_size);
}

void gw_4g_ecm_test_update_signal(const char *raw)
{
    update_signal(raw);
}

void gw_4g_ecm_test_mark_ready(void)
{
    s_4g.status.ready = true;
    s_4g.status.stage = GW_4G_ECM_STAGE_READY;
}

void gw_4g_ecm_test_simulate_lost_ip(void)
{
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_ETH_LOST_IP, NULL);
}

void gw_4g_ecm_test_simulate_eth_disconnected(void)
{
    iot_eth_event_handler(NULL, IOT_ETH_EVENT, IOT_ETH_EVENT_DISCONNECTED, NULL);
}
#endif

static esp_err_t modem_check_ready(void)
{
    ESP_LOGI(TAG, "checking modem ready");
    return at_command("AT", "\r\nOK\r\n", 300, 30, NULL, 0);
}

static esp_err_t modem_check_sim(void)
{
    ESP_LOGI(TAG, "checking sim card");
    return at_command("AT+CPIN?", "+CPIN: READY", 1000, 30, NULL, 0);
}

static esp_err_t modem_wait_network(void)
{
    char raw[GW_4G_AT_RESPONSE_BUFFER_BYTES] = {0};

    ESP_LOGI(TAG, "waiting network registration");
    for (int retry = 60; retry > 0; --retry) {
        int stat = -1;

        memset(raw, 0, sizeof(raw));
        if (at_command("AT+CEREG?", "+CEREG:", 1200, 1, raw, sizeof(raw)) == ESP_OK) {
            stat = parse_reg_stat(raw, "+CEREG:");
        } else if (at_command("AT+CGREG?", "+CGREG:", 1200, 1, raw, sizeof(raw)) == ESP_OK) {
            stat = parse_reg_stat(raw, "+CGREG:");
        } else if (at_command("AT+CREG?", "+CREG:", 1200, 1, raw, sizeof(raw)) == ESP_OK) {
            stat = parse_reg_stat(raw, "+CREG:");
        }

        if (stat == 1 || stat == 5) {
            s_4g.status.modem.registered = true;
            return ESP_OK;
        }

        memset(raw, 0, sizeof(raw));
        if (at_command("AT+CGATT?", "+CGATT:", 1200, 1, raw, sizeof(raw)) == ESP_OK &&
            (strstr(raw, "+CGATT: 1") != NULL || strstr(raw, "+CGATT:1") != NULL)) {
            s_4g.status.modem.registered = true;
            return ESP_OK;
        }

        ESP_LOGI(TAG, "network not ready stat=%d retry=%d", stat, retry - 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_4g.status.modem.registered = false;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t modem_set_apn(const char *apn)
{
    char cmd[96] = {0};

    if (apn == NULL || apn[0] == '\0') {
        return ESP_OK;
    }
    if (snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn) >= (int)sizeof(cmd)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return at_command(cmd, "\r\nOK\r\n", 1500, 2, NULL, 0);
}

static int modem_get_mdialup_mode(void)
{
    char raw[GW_4G_AT_RESPONSE_BUFFER_BYTES] = {0};
    const char *cursor = NULL;

    if (at_command("AT+MDIALUPCFG=\"mode\"",
                   "\r\nOK\r\n",
                   2000,
                   2,
                   raw,
                   sizeof(raw)) != ESP_OK) {
        return -1;
    }
    cursor = strstr(raw, "+MDIALUPCFG:");
    if (cursor == NULL) {
        return -1;
    }
    cursor = strrchr(cursor, ',');
    if (cursor == NULL) {
        return -1;
    }
    cursor++;
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor < '0' || *cursor > '9') {
        return -1;
    }
    return (int)(*cursor - '0');
}

static bool mdialup_is_connected(const char *status)
{
    return status != NULL &&
           (strstr(status, "+MDIALUP: 1,1") != NULL ||
            strstr(status, "+MDIALUP:1,1") != NULL);
}

static esp_err_t wait_usb_connected(uint32_t timeout_ms)
{
    EventBits_t bits = 0;

    if (s_4g.cdc_connected) {
        return ESP_OK;
    }
    if (s_4g.cdc_open_error != ESP_OK) {
        return s_4g.cdc_open_error;
    }
    bits = xEventGroupWaitBits(s_4g.events,
                               GW_4G_EVENT_USB_CONNECTED |
                                   GW_4G_EVENT_USB_DISCONNECTED |
                                   GW_4G_EVENT_USB_OPEN_FAILED,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & GW_4G_EVENT_USB_CONNECTED) != 0U) {
        return ESP_OK;
    }
    if ((bits & GW_4G_EVENT_USB_OPEN_FAILED) != 0U &&
        s_4g.cdc_open_error != ESP_OK) {
        return s_4g.cdc_open_error;
    }
    return (bits & GW_4G_EVENT_USB_DISCONNECTED) != 0U ?
               ESP_ERR_INVALID_STATE :
               ESP_ERR_TIMEOUT;
}

static esp_err_t modem_wait_ready_ms(uint32_t timeout_ms)
{
    int64_t start_ms = now_ms();

    while ((now_ms() - start_ms) < (int64_t)timeout_ms) {
        if (wait_usb_connected(1000) == ESP_OK &&
            at_command("AT", "\r\nOK\r\n", 400, 1, NULL, 0) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 当前 USB 描述符缺少 ECM 接口时，强制模组以 ECM 模式重新枚举。
 *
 * ML307A 的拨号模式设置要在模组重启后才反映到 USB 配置描述符。仅做 GPIO
 * 上电脉冲可能保留异常 USB 模式，因此这里通过文档命令写 mode=1 并执行一次
 * 模组软重启；CDC/ECM client 保持常驻，等待同一 USB Host 内完成重新枚举。
 */
static esp_err_t modem_recover_ecm_usb_descriptor(void)
{
    uint8_t previous_dev_addr = s_4g.cdc_dev_addr;
    bool saw_disconnect = false;
    bool fallback_reboot_sent = false;
    esp_err_t reboot_err = ESP_FAIL;
    int64_t deadline_ms = 0;
    int64_t fallback_reboot_ms = 0;

    ESP_LOGW(TAG,
             "USB config has no ECM interface; force modem ECM re-enumeration");
    ESP_RETURN_ON_ERROR(at_command("AT", "\r\nOK\r\n", 600, 2, NULL, 0),
                        TAG,
                        "modem unavailable before ECM recovery");
    ESP_RETURN_ON_ERROR(at_command("AT+MDIALUPCFG=\"mode\",1",
                                   "\r\nOK\r\n",
                                   2000,
                                   2,
                                   NULL,
                                   0),
                        TAG,
                        "set ECM USB mode failed");

    s_4g.cdc_open_error = ESP_OK;
    xEventGroupClearBits(s_4g.events,
                         GW_4G_EVENT_USB_CONNECTED |
                             GW_4G_EVENT_USB_DISCONNECTED |
                             GW_4G_EVENT_USB_OPEN_FAILED);

    reboot_err = at_send_no_response("AT+CFUN=1,1");
    if (reboot_err != ESP_OK && s_4g.cdc_connected) {
        reboot_err = at_send_no_response("AT+MREBOOT=0");
        if (reboot_err != ESP_OK && s_4g.cdc_connected) {
            reboot_err = at_send_no_response("AT+MREBOOT");
        }
    }
    /*
     * 部分固件在回复 OK 前就断开 AT 端点，此时命令层会报失败，但重启已经发生。
     * 以实际 USB 断开/重连为准，避免把成功的重启误判为失败。
     */
    if (reboot_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "modem reboot command returned %s; wait for USB re-enumeration",
                 esp_err_to_name(reboot_err));
    }

    deadline_ms = now_ms() + (int64_t)GW_4G_ECM_USB_REENUM_TIMEOUT_MS;
    fallback_reboot_ms = now_ms() + 5000;
    while (now_ms() < deadline_ms) {
        EventBits_t usb_bits = xEventGroupGetBits(s_4g.events);

        if ((usb_bits & GW_4G_EVENT_USB_DISCONNECTED) != 0U ||
            !s_4g.cdc_connected ||
            s_4g.cdc_dev_addr == 0U ||
            s_4g.cdc_dev_addr != previous_dev_addr) {
            saw_disconnect = true;
        }

        if (saw_disconnect && s_4g.cdc_connected) {
            usbh_cdc_port_handle_t ecm_port =
                usb_ecm_get_cdc_port_handle(s_4g.ecm_driver);
            esp_err_t connect_err =
                usb_ecm_get_last_connect_error(s_4g.ecm_driver);

            if (ecm_port != NULL && connect_err == ESP_OK) {
                ESP_LOGI(TAG,
                         "modem re-enumerated with ECM dev=%u",
                         (unsigned)s_4g.cdc_dev_addr);
                return ESP_OK;
            }
            /*
             * ECM callback 在 AT callback 之前、且两者位于同一 CDC task。
             * 因此 AT 已重新连上时，connect_err 已是本代设备的最终打开结果。
            */
            return connect_err != ESP_OK ? connect_err : ESP_ERR_NOT_FOUND;
        }
        if (!saw_disconnect &&
            !fallback_reboot_sent &&
            s_4g.cdc_connected &&
            now_ms() >= fallback_reboot_ms) {
            /*
             * CFUN 返回 OK 也不代表模组确实重启。5 秒内没有真实 DEV_GONE 时，
             * 按 ML307A 文档补发 MREBOOT=0，仍以 USB 断开/重枚举作为判据。
             */
            ESP_LOGW(TAG, "CFUN did not re-enumerate USB; send AT+MREBOOT=0");
            fallback_reboot_sent = true;
            reboot_err = at_send_no_response("AT+MREBOOT=0");
            if (reboot_err != ESP_OK && s_4g.cdc_connected) {
                reboot_err = at_send_no_response("AT+MREBOOT");
            }
        }
        if (s_4g.cdc_open_error != ESP_OK) {
            return s_4g.cdc_open_error;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t modem_get_ecm_status(char *status, size_t status_size)
{
    return at_command("AT+MDIALUP?", "\r\nOK\r\n", 3000, 2, status, status_size);
}

static esp_err_t modem_enter_ecm(char *status, size_t status_size)
{
    char raw[GW_4G_AT_RESPONSE_BUFFER_BYTES] = {0};
    int mode = 0;

    ESP_LOGI(TAG, "entering 4g ECM mode");
    (void)at_command("ATE0", "\r\nOK\r\n", 800, 1, NULL, 0);

    ESP_RETURN_ON_ERROR(at_command("AT+MDIALUPCFG=\"mode\",1",
                                   "\r\nOK\r\n",
                                   2000,
                                   2,
                                   NULL,
                                   0),
                        TAG,
                        "set ECM mode failed");

    mode = modem_get_mdialup_mode();
    if (mode != 1) {
        ESP_LOGW(TAG, "ECM mode inactive mode=%d, reboot modem", mode);
        ESP_RETURN_ON_ERROR(modem_recover_ecm_usb_descriptor(),
                            TAG,
                            "ECM mode reboot/re-enumeration failed");
        ESP_RETURN_ON_ERROR(modem_wait_ready_ms(5000),
                            TAG,
                            "modem not ready after ECM re-enumeration");
        mode = modem_get_mdialup_mode();
        if (mode != 1) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (at_command("AT+MDIALUP=1,1", "\r\nOK\r\n", 5000, 2, NULL, 0) != ESP_OK) {
        /*
         * 有些固件会自动拨号，重复拨号可能回 ERROR。
         * 最终以 MDIALUP? 和后续 GOT_IP 判断为准。
         */
        ESP_LOGW(TAG, "AT+MDIALUP=1,1 did not return OK, continue");
    }

    for (int i = 0; i < 6; ++i) {
        memset(raw, 0, sizeof(raw));
        if (modem_get_ecm_status(raw, sizeof(raw)) == ESP_OK && mdialup_is_connected(raw)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (status != NULL && status_size > 0U) {
        copy_text(status, status_size, raw);
    }
    sanitize_response(raw);
    ESP_LOGI(TAG, "ECM dial status: %s", raw[0] != '\0' ? raw : "<empty>");
    return ESP_OK;
}

static esp_err_t modem_exit_ecm(void)
{
    return at_command("AT+MDIALUP=1,0", "\r\nOK\r\n", 3000, 2, NULL, 0);
}

static esp_err_t ecm_stack_create(void)
{
    iot_usbh_ecm_config_t ecm_cfg = {
        .match_id_list = s_4g.ecm_match,
    };
    iot_eth_config_t eth_cfg = {0};
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();

    esp_err_t err = ESP_OK;

    if (s_4g.ecm_stack_ready) {
        return ESP_OK;
    }

    memset(s_4g.ecm_match, 0, sizeof(s_4g.ecm_match));
    s_4g.ecm_match[0].match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT;
    s_4g.ecm_match[0].idVendor = GW_4G_USB_VID;
    s_4g.ecm_match[0].idProduct = GW_4G_USB_PID;

    err = iot_eth_new_usb_ecm(&ecm_cfg, &s_4g.ecm_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_eth_new_usb_ecm failed: %s", esp_err_to_name(err));
        goto fail;
    }
    eth_cfg.driver = s_4g.ecm_driver;
    err = iot_eth_install(&eth_cfg, &s_4g.ecm_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_eth_install failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_4g.ecm_netif = esp_netif_new(&netif_cfg);
    if (s_4g.ecm_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "create ecm netif failed");
        goto fail;
    }
    s_4g.ecm_glue = iot_eth_new_netif_glue(s_4g.ecm_eth);
    if (s_4g.ecm_glue == NULL) {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "create ecm glue failed");
        goto fail;
    }
    err = esp_netif_attach(s_4g.ecm_netif, s_4g.ecm_glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attach ecm netif failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_4g.ecm_stack_ready = true;
    return ESP_OK;

fail:
    {
        esp_err_t cleanup_err = ecm_stack_destroy();
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "cleanup partial ECM stack failed: %s",
                     esp_err_to_name(cleanup_err));
        }
    }
    return err;
}

static esp_err_t ecm_start(void)
{
    if (!s_4g.ecm_stack_ready) {
        ESP_RETURN_ON_ERROR(ecm_stack_create(), TAG, "create ecm stack failed");
    }
    if (s_4g.ecm_started) {
        return ESP_OK;
    }

    xEventGroupClearBits(s_4g.events, GW_4G_EVENT_GOT_IP | GW_4G_EVENT_LOST_IP);
    for (int i = 0; i < 3; ++i) {
        esp_err_t err = iot_eth_start(s_4g.ecm_eth);

        if (err == ESP_OK) {
            s_4g.ecm_started = true;
            return ESP_OK;
        }
        ESP_LOGW(TAG, "iot_eth_start failed %d/3: %s", i + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_FAIL;
}

static esp_err_t ecm_stop(void)
{
    if (s_4g.ecm_started && s_4g.ecm_eth != NULL) {
        esp_err_t err = iot_eth_stop(s_4g.ecm_eth);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "iot_eth_stop failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    s_4g.ecm_started = false;
    xEventGroupClearBits(s_4g.events, GW_4G_EVENT_GOT_IP | GW_4G_EVENT_LOST_IP);
    return ESP_OK;
}

/** @brief 关闭一个仍存活的 CDC 端口；异步拔出造成的已关闭状态按成功处理。 */
static esp_err_t close_cdc_port(usbh_cdc_port_handle_t port, const char *name)
{
    if (port == NULL) {
        return ESP_OK;
    }

    esp_err_t err = usbh_cdc_port_close(port);
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "%s CDC port already closed", name);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "close %s CDC port failed: %s", name, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 彻底销毁本轮 ECM 数据栈。
 *
 * 仅调用 iot_eth_stop() 不会注销 USB ECM callback，也不会释放 CDC 端口。这里
 * 按 glue/netif/driver 的逆序销毁，确保下一轮不会复用上一代 USB 设备对象。
 */
static esp_err_t ecm_stack_destroy(void)
{
    esp_err_t first_err = ESP_OK;
    usbh_cdc_port_handle_t ecm_port =
        s_4g.ecm_driver != NULL ? usb_ecm_get_cdc_port_handle(s_4g.ecm_driver) : NULL;

    esp_err_t stop_err = ecm_stop();
    if (stop_err != ESP_OK) {
        return stop_err;
    }

    /*
     * ECM CDC transfer callback 的 user_data 指向 ecm_driver。必须先关闭并排空
     * 端口，再 uninstall/free driver，避免 pending callback 访问已释放对象。
     */
    esp_err_t close_err = close_cdc_port(ecm_port, "ECM");
    if (close_err != ESP_OK) {
        return close_err;
    }

    if (s_4g.ecm_glue != NULL) {
        esp_err_t err = iot_eth_del_netif_glue(s_4g.ecm_glue);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
        s_4g.ecm_glue = NULL;
    }
    if (s_4g.ecm_netif != NULL) {
        esp_netif_destroy(s_4g.ecm_netif);
        s_4g.ecm_netif = NULL;
    }
    if (s_4g.ecm_eth != NULL) {
        esp_err_t err = iot_eth_uninstall(s_4g.ecm_eth);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "uninstall ECM driver failed: %s", esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        } else {
            s_4g.ecm_eth = NULL;
            s_4g.ecm_driver = NULL;
        }
    } else if (s_4g.ecm_driver != NULL) {
        /*
         * iot_eth_install() 失败时 USB ECM init 已回滚内部对象，但创建函数分配的
         * driver 外壳仍由调用方拥有。
         */
        free(s_4g.ecm_driver);
        s_4g.ecm_driver = NULL;
    }

    if (s_4g.ecm_eth == NULL && s_4g.ecm_driver == NULL) {
        s_4g.ecm_stack_ready = false;
    }
    return first_err;
}

/**
 * @brief 卸载 CDC client 和 USB Host，保证下一轮从全新枚举状态启动。
 *
 * 物理断电通知与当前任务可能并发到达，因此端口关闭和 callback 注销都保持
 * 幂等；CDC 卸载短暂遇到设备仍在释放时会有限重试。
 */
static esp_err_t usb_stack_destroy(void)
{
    esp_err_t first_err = ecm_stack_destroy();
    if (first_err != ESP_OK) {
        /*
         * ECM driver 仍存活时禁止继续卸载其 CDC/USB Host 依赖；保留真实状态，
         * 让上层执行下一轮有界恢复，而不是构造半释放的 split-brain 栈。
         */
        return first_err;
    }

    if (s_4g.cdc_callback_registered) {
        esp_err_t err = usbh_cdc_unregister_dev_event_cb(cdc_device_event_cb);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
            s_4g.cdc_callback_registered = false;
        } else if (first_err == ESP_OK) {
            first_err = err;
        }
    }

    esp_err_t close_err = close_cdc_port(s_4g.cdc_port, "AT");
    if (close_err != ESP_OK && first_err == ESP_OK) {
        first_err = close_err;
    }
    s_4g.cdc_port = NULL;
    s_4g.cdc_connected = false;
    s_4g.cdc_open_error = ESP_OK;
    s_4g.cdc_dev_addr = 0;

    if (s_4g.cdc_driver_ready) {
        esp_err_t err = ESP_FAIL;
        for (uint32_t attempt = 0; attempt < GW_4G_USB_TEARDOWN_RETRIES; ++attempt) {
            err = usbh_cdc_driver_uninstall();
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG,
                     "uninstall CDC driver retry %" PRIu32 "/%u: %s",
                     attempt + 1U,
                     (unsigned)GW_4G_USB_TEARDOWN_RETRIES,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (err == ESP_OK) {
            s_4g.cdc_driver_ready = false;
            s_4g.cdc_callback_registered = false;
        } else if (first_err == ESP_OK) {
            first_err = err;
        }
    }

    if (!s_4g.cdc_driver_ready &&
        !wait_usb_host_release(GW_4G_USB_HOST_RELEASE_MS) &&
        first_err == ESP_OK) {
        first_err = ESP_ERR_TIMEOUT;
    }

    xEventGroupClearBits(s_4g.events,
                         GW_4G_EVENT_USB_CONNECTED |
                             GW_4G_EVENT_USB_DISCONNECTED |
                             GW_4G_EVENT_USB_OPEN_FAILED |
                             GW_4G_EVENT_GOT_IP |
                             GW_4G_EVENT_LOST_IP);
    if (first_err == ESP_OK) {
        ESP_LOGI(TAG, "4g USB/CDC/ECM stack fully released");
    }
    return first_err;
}

esp_err_t gw_4g_ecm_init(void)
{
    if (s_4g.initialized) {
        return ESP_OK;
    }

    memset(&s_4g, 0, sizeof(s_4g));
    s_4g.events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_4g.events != NULL, ESP_ERR_NO_MEM, TAG, "create 4g event group failed");
    s_4g.at_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_4g.at_lock != NULL, ESP_ERR_NO_MEM, TAG, "create at mutex failed");
    const esp_timer_create_args_t lost_timer_args = {
        .callback = net_lost_retry_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "4g_loss_retry",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&lost_timer_args,
                                         &s_4g.net_lost_retry_timer),
                        TAG,
                        "create net lost retry timer failed");

    s_4g.status.initialized = true;
    s_4g.status.stage = GW_4G_ECM_STAGE_IDLE;
    s_4g.status.modem.rssi = 99;
    s_4g.status.modem.ber = 99;
    s_4g.cdc_itf_num = GW_4G_USB_AT_INTERFACE;
    copy_text(s_4g.status.reason, sizeof(s_4g.status.reason), "idle");
    s_4g.initialized = true;
    ESP_LOGI(TAG, "4g ecm driver ready");
    return ESP_OK;
}

esp_err_t gw_4g_ecm_prepare_usb(void)
{
    ESP_RETURN_ON_ERROR(gw_4g_ecm_init(), TAG, "4g init failed");
    ESP_RETURN_ON_ERROR(port_init(), TAG, "4g gpio prepare failed");

    /*
     * CPU 复位不会保证 4G 模组同步掉电。首次准备先建立一次完整的 5 秒断电
     * 基线，避免仍带电的模组和上一代 USB 端点进入新 Host 生命周期。
     */
    if (!s_4g.power_baseline_ready) {
        power_off();
    }

    esp_err_t err = cdc_init();
    if (err == ESP_OK) {
        return ESP_OK;
    }

    /*
     * 预留阶段不允许留下半初始化 Host。即使 CDC 在注册 client 之前失败，
     * 显式 stop 请求也会让空 Host 退出，后续联网任务可从干净状态重试。
     */
    esp_err_t cleanup_err = usb_stack_destroy();
    if (cleanup_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "cleanup failed USB preparation: %s",
                 esp_err_to_name(cleanup_err));
    }
    return err;
}

esp_err_t gw_4g_ecm_start(const gw_4g_ecm_start_config_t *config)
{
    uint32_t timeout_ms = (config != NULL && config->timeout_ms > 0U) ? config->timeout_ms : 120000U;
    const char *apn = (config != NULL && config->apn != NULL) ? config->apn : "";
    char ecm_status[GW_4G_AT_RESPONSE_BUFFER_BYTES] = {0};
    char failure_reason[sizeof(s_4g.status.reason)] = {0};
    EventBits_t bits = 0;
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_ERROR(gw_4g_ecm_init(), TAG, "4g init failed");
    ESP_RETURN_ON_ERROR(ensure_idf(), TAG, "esp netif init failed");
    ESP_RETURN_ON_ERROR(register_event_handlers(), TAG, "register events failed");

    suppress_net_loss(false);
    set_stage(GW_4G_ECM_STAGE_STARTING, "4g starting", config);
    clear_ip_status();
    copy_text(s_4g.status.apn, sizeof(s_4g.status.apn), apn);

    err = gw_4g_ecm_prepare_usb();
    if (err != ESP_OK) {
        goto fail;
    }
    if (!is_detected()) {
        ESP_LOGW(TAG, "4g detect pin is not active, continue");
    }
    err = ecm_stack_create();
    if (err != ESP_OK) {
        goto fail;
    }
    err = ecm_start();
    if (err != ESP_OK) {
        goto fail;
    }

    /*
     * 清除上一代枚举结果后再上电。连接回调若在 wait 之前失败，会同时写入
     * cdc_open_error 和事件位，因此不会丢失，也不再无意义等待 30 秒。
     */
    s_4g.cdc_open_error = ESP_OK;
    xEventGroupClearBits(s_4g.events,
                         GW_4G_EVENT_USB_CONNECTED |
                             GW_4G_EVENT_USB_DISCONNECTED |
                             GW_4G_EVENT_USB_OPEN_FAILED);
    power_on();
    if (s_4g.consecutive_start_failures >= GW_4G_HARD_RESET_AFTER_FAILURES) {
        hardware_reset();
    }
    set_stage(GW_4G_ECM_STAGE_WAIT_USB, "4g wait usb", config);
    err = wait_usb_connected(GW_4G_WAIT_USB_TIMEOUT_MS);
    if (err != ESP_OK) {
        goto fail;
    }
    if (usb_ecm_get_cdc_port_handle(s_4g.ecm_driver) == NULL) {
        esp_err_t ecm_connect_err =
            usb_ecm_get_last_connect_error(s_4g.ecm_driver);

        if (ecm_connect_err == ESP_ERR_NOT_FOUND) {
            /*
             * 新模组或异常掉电后的模组可能只枚举 AT 接口。走正式 AT 配置流程
             * 切到 ECM 并软重启，不要求用户给整块网关断电。
             */
            err = modem_recover_ecm_usb_descriptor();
        } else {
            err = ecm_connect_err != ESP_OK ?
                      ecm_connect_err :
                      ESP_ERR_INVALID_STATE;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "ECM data port recovery failed: %s",
                     esp_err_to_name(err));
            goto fail;
        }
    }

    set_stage(GW_4G_ECM_STAGE_PROBING, "4g probing", config);
    err = modem_check_ready();
    if (err != ESP_OK) {
        goto fail;
    }

    set_stage(GW_4G_ECM_STAGE_SIM_CHECKING, "4g sim checking", config);
    err = modem_check_sim();
    if (err != ESP_OK) {
        goto fail;
    }

    set_stage(GW_4G_ECM_STAGE_REGISTERING, "4g registering network", config);
    err = modem_wait_network();
    if (err != ESP_OK) {
        goto fail;
    }

    set_stage(GW_4G_ECM_STAGE_CONFIGURING, "4g configuring", config);
    err = modem_set_apn(s_4g.status.apn);
    if (err != ESP_OK) {
        goto fail;
    }
    err = modem_enter_ecm(ecm_status, sizeof(ecm_status));
    if (err != ESP_OK) {
        goto fail;
    }
    err = ecm_start();
    if (err != ESP_OK) {
        goto fail;
    }

    set_stage(GW_4G_ECM_STAGE_GETTING_IP, "4g getting ip", config);
    bits = xEventGroupWaitBits(s_4g.events,
                               GW_4G_EVENT_GOT_IP |
                                   GW_4G_EVENT_LOST_IP |
                                   GW_4G_EVENT_USB_DISCONNECTED,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(timeout_ms > GW_4G_WAIT_IP_TIMEOUT_MS ?
                                                 GW_4G_WAIT_IP_TIMEOUT_MS :
                                                 timeout_ms));
    if ((bits & GW_4G_EVENT_GOT_IP) == 0U && !s_4g.status.ready) {
        err = ESP_ERR_TIMEOUT;
        goto fail;
    }

    set_stage(GW_4G_ECM_STAGE_READY, "4g ready", config);
    s_4g.consecutive_start_failures = 0;
    ESP_LOGI(TAG, "4g ready");
    (void)gw_4g_ecm_refresh_modem_info(2000);
    return ESP_OK;

fail:
    {
        gw_4g_ecm_stage_t failed_stage = s_4g.status.stage;
        (void)snprintf(failure_reason,
                       sizeof(failure_reason),
                       "%s:%s",
                       gw_4g_ecm_stage_to_string(failed_stage),
                       esp_err_to_name(err));
        if (s_4g.consecutive_start_failures < UINT32_MAX) {
            ++s_4g.consecutive_start_failures;
        }
        ESP_LOGW(TAG,
                 "4g start failed: stage=%s err=%s consecutive=%" PRIu32
                 ", rebuild USB stack",
                 gw_4g_ecm_stage_to_string(failed_stage),
                 esp_err_to_name(err),
                 s_4g.consecutive_start_failures);
    }
    clear_ip_status();
    s_4g.status.modem.registered = false;
    suppress_net_loss(true);
    (void)ecm_stop();
    power_off();
    esp_err_t cleanup_err = usb_stack_destroy();
    if (cleanup_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "4g recovery cleanup failed: %s",
                 esp_err_to_name(cleanup_err));
    }
    set_stage(GW_4G_ECM_STAGE_FAILED, failure_reason, config);
    return err;
}

esp_err_t gw_4g_ecm_stop(void)
{
    ESP_RETURN_ON_ERROR(gw_4g_ecm_init(), TAG, "4g init failed");

    suppress_net_loss(true);
    if (s_4g.status.stage == GW_4G_ECM_STAGE_READY) {
        (void)modem_exit_ecm();
    }
    bool resources_active = s_4g.ecm_stack_ready ||
                            s_4g.ecm_started ||
                            s_4g.cdc_driver_ready ||
                            s_4g.cdc_port != NULL ||
                            s_4g.usb_lib_task != NULL;
    esp_err_t cleanup_err = ESP_OK;
    if (resources_active) {
        (void)ecm_stop();
        power_off();
        cleanup_err = usb_stack_destroy();
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "stop 4g recovery cleanup failed: %s",
                     esp_err_to_name(cleanup_err));
        }
    } else {
        (void)gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_EN, 0);
        (void)gpio_set_level((gpio_num_t)GW_4G_PIN_POWER_CTRL, 0);
    }
    clear_ip_status();
    set_stage(GW_4G_ECM_STAGE_STOPPED, "stopped", NULL);
    return cleanup_err;
}

esp_err_t gw_4g_ecm_get_status(gw_4g_ecm_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(gw_4g_ecm_init(), TAG, "4g init failed");
    *out = s_4g.status;
    out->initialized = s_4g.initialized;
    out->ready = s_4g.status.ready;
    return ESP_OK;
}

esp_err_t gw_4g_ecm_refresh_modem_info(uint32_t timeout_ms)
{
    char raw[GW_4G_AT_RESPONSE_BUFFER_BYTES] = {0};
    uint32_t per_cmd_timeout = timeout_ms == 0U ? 2000U : timeout_ms;

    if (s_4g.status.stage != GW_4G_ECM_STAGE_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)query_text("AT+CGMI", s_4g.status.modem.manufacturer, sizeof(s_4g.status.modem.manufacturer));
    (void)query_text("AT+CGMM", s_4g.status.modem.model, sizeof(s_4g.status.modem.model));
    (void)query_text("AT+CGMR", s_4g.status.modem.revision, sizeof(s_4g.status.modem.revision));
    (void)query_text("AT+CGSN", s_4g.status.modem.imei, sizeof(s_4g.status.modem.imei));
    (void)query_text("AT+CIMI", s_4g.status.modem.imsi, sizeof(s_4g.status.modem.imsi));
    (void)query_text("AT+ICCID", s_4g.status.modem.iccid, sizeof(s_4g.status.modem.iccid));

    memset(raw, 0, sizeof(raw));
    if (at_command("AT+COPS?", "\r\nOK\r\n", per_cmd_timeout, 1, raw, sizeof(raw)) == ESP_OK) {
        extract_cops(raw, s_4g.status.modem.operator_name, sizeof(s_4g.status.modem.operator_name));
    }
    memset(raw, 0, sizeof(raw));
    if (at_command("AT+CSQ", "\r\nOK\r\n", per_cmd_timeout, 1, raw, sizeof(raw)) == ESP_OK) {
        update_signal(raw);
    }
    memset(raw, 0, sizeof(raw));
    if (at_command("AT+CEREG?", "+CEREG:", per_cmd_timeout, 1, raw, sizeof(raw)) == ESP_OK) {
        int stat = parse_reg_stat(raw, "+CEREG:");

        s_4g.status.modem.registered = (stat == 1 || stat == 5);
    }
    s_4g.status.modem.updated_at_ms = now_ms();
    return ESP_OK;
}

esp_err_t gw_4g_ecm_at(const char *command,
                       uint32_t timeout_ms,
                       char *out_response,
                       size_t response_size)
{
    if (command == NULL || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0U) {
        timeout_ms = 2000U;
    }
    return at_command(command, "\r\nOK\r\n", timeout_ms, 1, out_response, response_size);
}

const char *gw_4g_ecm_stage_to_string(gw_4g_ecm_stage_t stage)
{
    switch (stage) {
    case GW_4G_ECM_STAGE_IDLE:
        return "idle";
    case GW_4G_ECM_STAGE_STARTING:
        return "starting";
    case GW_4G_ECM_STAGE_WAIT_USB:
        return "wait_usb";
    case GW_4G_ECM_STAGE_PROBING:
        return "probing";
    case GW_4G_ECM_STAGE_SIM_CHECKING:
        return "sim_checking";
    case GW_4G_ECM_STAGE_REGISTERING:
        return "registering";
    case GW_4G_ECM_STAGE_CONFIGURING:
        return "configuring";
    case GW_4G_ECM_STAGE_GETTING_IP:
        return "getting_ip";
    case GW_4G_ECM_STAGE_READY:
        return "ready";
    case GW_4G_ECM_STAGE_FAILED:
        return "failed";
    case GW_4G_ECM_STAGE_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}
