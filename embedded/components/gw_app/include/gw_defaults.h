#pragma once

#include <stdint.h>

#include "gw_hw_config.h"

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

#define GW_MQTT_MAX_PAYLOAD_SIZE              8192U
#define GW_MQTT_DEFAULT_QOS                   1
#define GW_MQTT_DEFAULT_CLEAN_SESSION         1
#define GW_MQTT_DEFAULT_RETAIN                0
#define GW_MQTT_DEFAULT_KEEP_ALIVE_SEC        60U
#define GW_MQTT_DEFAULT_CONNECT_TIMEOUT_SEC   10U
#define GW_MQTT_DEFAULT_RECONNECT_SEC         3U
#define GW_MQTT_DEFAULT_RECONNECT_MAX_SEC     60U
#define GW_MQTT_DEFAULT_TASK_STACK_BYTES      (6U * 1024U)
#define GW_BUSINESS_MQTT_PAYLOAD_SIZE         GW_MQTT_MAX_PAYLOAD_SIZE
#define GW_BUSINESS_MQTT_TASK_STACK_BYTES     (6U * 1024U)

#define GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC             10U
#define GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC   3U
#define GW_HTTP_DEFAULT_RETRY_COUNT                     3U
#define GW_HTTP_MAX_URL_LEN                             512U
#define GW_HTTP_DEFAULT_RESPONSE_MAX_BYTES              8192U
#define GW_HTTP_WORKER_STACK_BYTES                      (8U * 1024U)

#define GW_NET_APN_MAX_LEN                    64U
#define GW_NET_DEFAULT_RETRY_FIRST_SEC        3U
#define GW_NET_DEFAULT_RETRY_SECOND_SEC       10U
#define GW_NET_DEFAULT_RETRY_THIRD_SEC        30U

#define GW_HEARTBEAT_DEFAULT_INTERVAL_SEC     60U
#define GW_HEARTBEAT_MIN_INTERVAL_SEC         10U

#define GW_TASK_STACK_DEV_MGR_BYTES           (8U * 1024U)
#define GW_TASK_STACK_LOG_BYTES               (8U * 1024U)
#define GW_TASK_STACK_CONFIG_BYTES            (8U * 1024U)
#define GW_TASK_STACK_OTA_BYTES               (16U * 1024U)
#define GW_TASK_STACK_REBOOT_BYTES            (4U * 1024U)
#define GW_TASK_STACK_TIME_SYNC_BYTES         (6U * 1024U)
#define GW_TASK_STACK_ACTION_EXECUTOR_BYTES   (8U * 1024U)
#define GW_TIME_SYNC_TASK_PRIORITY            6U
#define GW_TIME_SNTP_SERVER                    "ntp.aliyun.com"

#define GW_EVENT_QUEUE_DEV_MGR_LEN            32U
#define GW_EVENT_QUEUE_LOG_LEN                64U
#define GW_EVENT_QUEUE_HEALTH_LEN             16U
#define GW_MQTT_CMD_QUEUE_LEN                 16U

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

#define GW_REBOOT_DEFAULT_DELAY_MS            1500U
#define GW_REBOOT_MAX_DELAY_MS                60000U

/* 定时任务与 BLE 配置协议使用固定上限，防止配置写入耗尽运行期内存。 */
#define GW_SCHEDULER_MAX_TASKS                16U
/* 0：不加载运行期任务表并暂停后台执行；1：启用定时任务服务。NVS 格式始终保留。 */
#define GW_SCHEDULER_SERVICE_ENABLED         0
#define GW_SCHEDULER_TASK_NAME_MAX_LEN        31U
#define GW_SCHEDULER_ACTION_DATA_MAX_LEN      64U
#define GW_SCHEDULER_MAX_DURATION_WEEKS       52U
/* hello 增加时间恢复锁字段后，最坏长度超过 640 字节，预留到 768 字节。 */
#define GW_BLE_CONFIG_JSON_MAX_BYTES          768U
#define GW_BLE_DEVICE_ALIAS_MAX_LEN           31U

#ifdef __cplusplus
}
#endif
