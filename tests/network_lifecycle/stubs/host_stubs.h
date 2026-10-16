#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FINISHED 0x10c
const char *esp_err_to_name(esp_err_t error);
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expression, ...) do { esp_err_t error_ = (expression); if (error_ != ESP_OK) return error_; } while (0)
#define ESP_RETURN_ON_FALSE(condition, error, ...) do { if (!(condition)) return (error); } while (0)

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0U
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define BIT0 1U
#define BIT1 2U
void host_enter_critical(portMUX_TYPE *lock);
void host_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) host_enter_critical(lock)
#define portEXIT_CRITICAL(lock) host_exit_critical(lock)
size_t strlcpy(char *target, const char *source, size_t size);
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *handle);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);

typedef uint32_t EventBits_t;
typedef EventBits_t *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear, BaseType_t all, TickType_t ticks);

typedef const char *esp_event_base_t;
#define IOT_ETH_EVENT "eth"
#define IP_EVENT "ip"
#define ESP_EVENT_ANY_ID -1
#define IOT_ETH_EVENT_CONNECTED 1
#define IOT_ETH_EVENT_DISCONNECTED 2
#define IP_EVENT_ETH_GOT_IP 3
#define IP_EVENT_ETH_LOST_IP 4
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t event, esp_event_handler_t callback, void *arg);

typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio, int level);
int64_t esp_timer_get_time(void);

typedef struct { uint8_t bytes[4]; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { struct { union { esp_ip4_addr_t ip4; } u_addr; } ip; } esp_netif_dns_info_t;
typedef struct { int unused; } esp_netif_t;
typedef struct { int unused; } esp_netif_config_t;
typedef struct { esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (ip)->bytes[0], (ip)->bytes[1], (ip)->bytes[2], (ip)->bytes[3]
#define ESP_NETIF_DNS_MAIN 0
#define ESP_NETIF_DNS_BACKUP 1
#define ESP_NETIF_DEFAULT_ETH() {0}
esp_err_t esp_netif_init(void);
esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *dns);
esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
void esp_netif_destroy(esp_netif_t *netif);
esp_err_t esp_netif_attach(esp_netif_t *netif, void *glue);

typedef void *iot_eth_handle_t;
typedef struct { int unused; } iot_eth_driver_t;
typedef struct { iot_eth_driver_t *driver; } iot_eth_config_t;
typedef void *iot_eth_netif_glue_handle_t;
typedef struct { const void *match_id_list; } iot_usbh_ecm_config_t;
#define ESP_USB_DEVICE_MATCH_ID_ANY NULL
esp_err_t iot_eth_new_usb_ecm(const iot_usbh_ecm_config_t *config, iot_eth_driver_t **driver);
esp_err_t iot_eth_install(const iot_eth_config_t *config, iot_eth_handle_t *handle);
esp_err_t iot_eth_start(iot_eth_handle_t handle);
esp_err_t iot_eth_stop(iot_eth_handle_t handle);
esp_err_t iot_eth_uninstall(iot_eth_handle_t handle);
iot_eth_netif_glue_handle_t iot_eth_new_netif_glue(iot_eth_handle_t handle);
esp_err_t iot_eth_del_netif_glue(iot_eth_netif_glue_handle_t glue);
void *usb_ecm_get_cdc_port_handle(iot_eth_driver_t *driver);
esp_err_t usb_ecm_get_last_connect_error(iot_eth_driver_t *driver);
typedef struct { size_t task_stack_size; unsigned task_priority; int task_coreid; bool skip_init_usb_host_driver; } usbh_cdc_driver_config_t;
esp_err_t usbh_cdc_driver_install(const usbh_cdc_driver_config_t *config);
esp_err_t usbh_cdc_driver_uninstall(void);

typedef struct { uint8_t bNumConfigurations; uint16_t idVendor, idProduct; } usb_device_desc_t;
typedef struct { bool skip_phy_setup; int intr_flags; bool (*enum_filter_cb)(const usb_device_desc_t *, uint8_t *); } usb_host_config_t;
#define ESP_INTR_FLAG_LEVEL1 1
#define USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS 1U
#define USB_HOST_LIB_EVENT_FLAGS_ALL_FREE 2U
esp_err_t usb_host_install(const usb_host_config_t *config);
esp_err_t usb_host_lib_handle_events(TickType_t ticks, uint32_t *flags);
esp_err_t usb_host_device_free_all(void);
esp_err_t usb_host_uninstall(void);
