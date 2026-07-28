#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_defaults.h
 * @brief 固件编译期默认值和限制。
 */

/**
 * 固件固定默认值和限制。
 *
 * 这里放跨模块共享的编译期常量，运行期可配置参数的最终值仍应从
 * NVS 参数表读取；无配置时再回落到这些默认值。
 */

#define GW_PRODUCT_ID_DEFAULT                 "yx_gw"
#define GW_GROUP_ID_DEFAULT                   "yx_gw"
#define GW_DEVICE_MODEL                       "ESP32S3R8N16"

/**
 * 启动自检开关。
 *
 * 0：正常业务启动；1：运行 P1-P13 开发/工厂自检链。
 * 后续如需按键、产测指令或 NVS 参数进入自检，可挂到 app_main 的启动模式选择处。
 */
#define GW_BOOT_SELF_CHECK_ENABLED            0

/**
 * 业务脚本自动运行开关。
 *
 * 0：保留 MicroPython 和脚本 API 模块，但启动、配置更新及 OTA 流程均不自动执行脚本；
 * 1：恢复旧版脚本自动运行行为。
 */
#define GW_SCRIPT_AUTOSTART_ENABLED           0

#define GW_MQTT_MAX_PAYLOAD_SIZE              8192U
#define GW_MQTT_DEFAULT_QOS                   1
#define GW_MQTT_DEFAULT_CLEAN_SESSION         1
#define GW_MQTT_DEFAULT_RETAIN                0
#define GW_MQTT_DEFAULT_KEEP_ALIVE_SEC        60U
#define GW_MQTT_DEFAULT_CONNECT_TIMEOUT_SEC   10U
#define GW_MQTT_DEFAULT_RECONNECT_SEC         3U
#define GW_MQTT_DEFAULT_RECONNECT_MAX_SEC     60U
#define GW_MQTT_DEFAULT_TASK_STACK_BYTES      (6U * 1024U)
#define GW_BUSINESS_MQTT_PAYLOAD_SIZE         2048U
#define GW_BUSINESS_MQTT_TASK_STACK_BYTES     (6U * 1024U)

#define GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC             10U
#define GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC   3U
#define GW_HTTP_DEFAULT_RETRY_COUNT                     3U
#define GW_HTTP_MAX_URL_LEN                             512U
#define GW_HTTP_DEFAULT_RESPONSE_MAX_BYTES              8192U
#define GW_HTTP_WORKER_STACK_BYTES                      (12U * 1024U)
#define GW_SCRIPT_HTTP_QUEUE_LEN                        2U
#define GW_SCRIPT_HTTP_RESPONSE_MAX_BYTES               (64U * 1024U)
#define GW_SCRIPT_HTTP_BODY_MAX_BYTES                   (64U * 1024U)

#define GW_NET_APN_MAX_LEN                    64U
#define GW_NET_DEFAULT_RETRY_FIRST_SEC        3U
#define GW_NET_DEFAULT_RETRY_SECOND_SEC       10U
#define GW_NET_DEFAULT_RETRY_THIRD_SEC        30U

#define GW_4G_USB_VID                         0x2ECC
#define GW_4G_USB_PID                         0x3012
#define GW_4G_USB_AT_INTERFACE                2U
#define GW_4G_PIN_POWER_EN                    12
#define GW_4G_PIN_POWER_CTRL                  46
#define GW_4G_PIN_RESET                       11
#define GW_4G_PIN_DETECT                      38
#define GW_4G_WAIT_USB_TIMEOUT_MS             30000U
#define GW_4G_ECM_USB_REENUM_TIMEOUT_MS       35000U
#define GW_4G_WAIT_IP_TIMEOUT_MS              45000U
#define GW_4G_POWER_ON_DELAY_MS               3000U
/* 4G 失败恢复时的断电、硬复位和 USB 栈释放参数。 */
#define GW_4G_POWER_OFF_SETTLE_MS             5000U
#define GW_4G_RESET_PULSE_MS                  200U
#define GW_4G_RESET_DELAY_MS                  5000U
#define GW_4G_HARD_RESET_AFTER_FAILURES       2U
#define GW_4G_USB_TEARDOWN_RETRIES            3U
#define GW_4G_USB_HOST_RELEASE_MS             3000U
#define GW_4G_USB_HOST_POLL_MS                20U
#define GW_4G_CDC_TASK_STACK_BYTES            (12U * 1024U)
#define GW_4G_CDC_TASK_PRIORITY               9U
#define GW_4G_CDC_RX_BUFFER_BYTES             (32U * 1024U)
#define GW_4G_CDC_TX_BUFFER_BYTES             (16U * 1024U)
/*
 * AT 通道单次响应受下方 512 字节缓冲限制，1 KiB USB transfer 已覆盖两个
 * full-speed bulk 包；大容量排队仍由 PSRAM 中的 RX/TX ringbuffer 承担。
 */
#define GW_4G_CDC_TRANSFER_BUFFER_BYTES       (1U * 1024U)
#define GW_4G_AT_RESPONSE_BUFFER_BYTES        512U
#define GW_4G_AT_READ_TIMEOUT_MS              30U

#define GW_HEARTBEAT_DEFAULT_INTERVAL_SEC     60U
#define GW_HEARTBEAT_MIN_INTERVAL_SEC         10U

#define GW_TASK_STACK_DEV_MGR_BYTES           (24U * 1024U)
#define GW_TASK_STACK_SCRIPT_BYTES            (64U * 1024U)
#define GW_TASK_STACK_LOG_BYTES               (8U * 1024U)
#define GW_TASK_STACK_CONFIG_BYTES            (16U * 1024U)
#define GW_TASK_STACK_OTA_BYTES               (16U * 1024U)
#define GW_TASK_STACK_REBOOT_BYTES            (4U * 1024U)
#define GW_TASK_STACK_TIME_SYNC_BYTES         (6U * 1024U)
#define GW_TIME_SYNC_TASK_PRIORITY            6U
#define GW_TIME_SNTP_SERVER                    "ntp.aliyun.com"

#define GW_EVENT_QUEUE_DEV_MGR_LEN            32U
#define GW_EVENT_QUEUE_SCRIPT_LEN             32U
#define GW_EVENT_QUEUE_LOG_LEN                64U
#define GW_EVENT_QUEUE_HEALTH_LEN             16U
#define GW_MQTT_CMD_QUEUE_LEN                 16U
#define GW_SCRIPT_MQTT_EVENT_QUEUE_LEN        GW_EVENT_QUEUE_SCRIPT_LEN
#define GW_SCRIPT_HTTP_EVENT_QUEUE_LEN        GW_EVENT_QUEUE_SCRIPT_LEN
#define GW_SCRIPT_MQTT_MAX_CLIENTS            4U
#define GW_SCRIPT_MQTT_TOPIC_MAX_LEN          256U
#define GW_SCRIPT_MQTT_REASON_MAX_LEN         64U
#define GW_SCRIPT_NET_WAIT_POLL_MS            50U
#define GW_SCRIPT_RUNTIME_SLEEP_POLL_MS       50U

#define GW_REQUEST_ID_MAX_LEN                 64U
#define GW_REQUEST_ID_CACHE_SIZE              32U
#define GW_REQUEST_ID_RETENTION_SEC           3600U

#define GW_LOG_MAX_LINE_SIZE                  4096U
#define GW_LOG_SEGMENT_SIZE                   (128U * 1024U)
#define GW_LOG_TARGET_SIZE                    (1024U * 1024U)
#define GW_LOG_FLOAT_SIZE                     (256U * 1024U)
#define GW_LOG_RETENTION_DAYS                 30U
#define GW_LOG_SCAN_INTERVAL_SEC              (24U * 60U * 60U)
#define GW_LOG_HIGH_WATERMARK_PERCENT         80U
#define GW_LOG_CLEAN_TO_PERCENT               50U
#define GW_LOG_MQTT_DIRECT_MAX_SIZE           8192U
#define GW_LOG_QUEUE_MAX_BODY_BYTES           (64U * 1024U)

#define GW_DATAFS_DEFAULT_QUOTA_BYTES         (1024U * 1024U)
#define GW_CONFIG_SLOT_MAX_BYTES              (1800U * 1024U)
#define GW_CONFIG_MANIFEST_MAX_BYTES          (64U * 1024U)
#define GW_CONFIG_MANIFEST_MAX_FILES          128U
#define GW_CONFIG_UPDATE_QUEUE_LEN            2U

#define GW_OTA_HTTP_BUF_BYTES                 (4U * 1024U)
#define GW_OTA_PROGRESS_INTERVAL_SEC          5U

#define GW_FS_MOUNT_RETRY_COUNT              3U
#define GW_FS_MOUNT_RETRY_DELAY_MS           200U

#define GW_TIME_MIN_TRUSTED_YEAR              2026
#define GW_TIME_MAX_TRUSTED_YEAR              2046
#define GW_TIME_ROLLBACK_TOLERANCE_MS         300000LL
#define GW_TIME_SAVE_ADVANCE_INTERVAL_SEC     (24U * 60U * 60U)
#define GW_NTP_DEFAULT_ENABLED                1
#define GW_NTP_DEFAULT_METHOD                 "sntp"
#define GW_NTP_DEFAULT_SERVER                 "ntp.aliyun.com"
#define GW_NTP_DEFAULT_SYNC_INTERVAL_SEC      3600U

#define GW_SCRIPT_EXCEPTION_ACTION_RESTART_IMMEDIATELY  "restart_immediately"
#define GW_SCRIPT_EXCEPTION_ACTION_RESTART_BY_WATCHDOG  "restart_by_watchdog"
#define GW_SCRIPT_EXCEPTION_ACTION_IGNORE                "ignore"
#define GW_SCRIPT_EXCEPTION_ACTION_DEFAULT               GW_SCRIPT_EXCEPTION_ACTION_RESTART_BY_WATCHDOG
#define GW_SCRIPT_STOP_TIMEOUT_MS             5000U
#define GW_REBOOT_DEFAULT_DELAY_MS            1500U
#define GW_REBOOT_MAX_DELAY_MS                60000U

#define GW_BOARD_LED_COUNT                    3U
#define GW_BOARD_LED0_GPIO                    40
#define GW_BOARD_LED1_GPIO                    41
#define GW_BOARD_LED2_GPIO                    42
/** @brief 三路可控板载指示灯均为低电平点亮。 */
#define GW_BOARD_LED_ACTIVE_LEVEL             0
/** @brief 三路可控板载指示灯的熄灭电平。 */
#define GW_BOARD_LED_INACTIVE_LEVEL           1
/** @brief 业务 MQTT 在线指示灯索引，对应 GPIO40。 */
#define GW_BOARD_MQTT_LED_INDEX               0U
/** @brief 定时任务板载输出灯索引，对应 GPIO41。 */
#define GW_BOARD_TASK_LED_INDEX               1U
/** @brief BLE“闪灯找设备”使用的板载 LED 索引，当前选择未承载历史业务的 GPIO42。 */
#define GW_BOARD_IDENTIFY_LED_INDEX           2U
/** @brief 单次设备识别闪灯持续时间。 */
#define GW_BOARD_IDENTIFY_BLINK_MS            350U
#define GW_BOARD_RS485_UART                   1
#define GW_BOARD_RS485_TX_GPIO                18
#define GW_BOARD_RS485_RX_GPIO                8
#define GW_BOARD_RS485_BAUDRATE               115200U
#define GW_BOARD_RS485_RX_BUF_BYTES           2048U
#define GW_BOARD_RS485_TX_BUF_BYTES           512U
#define GW_BOARD_RS485_WAIT_TX_MS             1000U

#define GW_HARDWARE_WATCHDOG_ENABLED          1
#define GW_HARDWARE_WATCHDOG_GPIO             4
#define GW_HARDWARE_WATCHDOG_PULSE_HIGH_MS    100U
#define GW_HARDWARE_WATCHDOG_FEED_INTERVAL_MS (30U * 1000U)
#define GW_HARDWARE_WATCHDOG_TASK_STACK       4096U
#define GW_HARDWARE_WATCHDOG_TASK_PRIORITY    3U

#define GW_CH37X_DEFAULT_INTERFACE            0U
#define GW_CH37X_MAX_INTERFACE                3U
#define GW_CH37X_HOST_TASK_STACK              8192U
#define GW_CH37X_HOST_TASK_PRIORITY           5U
#define GW_CH37X_RX_QUEUE_LEN                 2048U

/* 定时任务与 BLE 配置协议使用固定上限，防止配置写入耗尽运行期内存。 */
#define GW_SCHEDULER_MAX_TASKS                16U
#define GW_SCHEDULER_TASK_NAME_MAX_LEN        31U
#define GW_SCHEDULER_ACTION_DATA_MAX_LEN      64U
#define GW_SCHEDULER_MAX_DURATION_WEEKS       52U
/* hello 增加时间恢复锁字段后，最坏长度超过 640 字节，预留到 768 字节。 */
#define GW_BLE_CONFIG_JSON_MAX_BYTES          768U
#define GW_BLE_DEVICE_ALIAS_MAX_LEN           31U

#ifdef __cplusplus
}
#endif
