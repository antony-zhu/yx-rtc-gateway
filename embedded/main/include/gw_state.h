#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 设备主状态。
 */
typedef enum {
    GW_DEVICE_STATUS_BOOTING = 0,
    GW_DEVICE_STATUS_OFFLINE,
    GW_DEVICE_STATUS_ONLINE,
    GW_DEVICE_STATUS_CONFIG_CHECKING,
    GW_DEVICE_STATUS_CONFIG_UPDATING,
    GW_DEVICE_STATUS_OTA_CHECKING,
    GW_DEVICE_STATUS_OTA_UPGRADING,
    GW_DEVICE_STATUS_DEGRADED,
    GW_DEVICE_STATUS_BUSY,
    GW_DEVICE_STATUS_FACTORY_RESETTING,
    GW_DEVICE_STATUS_REBOOTING,
} gw_device_status_t;

/**
 * @brief 网络状态。
 */
typedef enum {
    GW_NETWORK_STATUS_DISCONNECTED = 0,
    GW_NETWORK_STATUS_CONNECTING,
    GW_NETWORK_STATUS_CONNECTED,
    GW_NETWORK_STATUS_DEGRADED,
} gw_network_status_t;

/**
 * @brief 设备管理 MQTT 状态，只表示 dev_mqtt。
 */
typedef enum {
    GW_MQTT_STATUS_DISCONNECTED = 0,
    GW_MQTT_STATUS_NOT_CONFIGURED,
    GW_MQTT_STATUS_CONNECTING,
    GW_MQTT_STATUS_CONNECTED,
    GW_MQTT_STATUS_RECONNECTING,
    GW_MQTT_STATUS_AUTH_FAILED,
} gw_mqtt_status_t;

/**
 * @brief 脚本运行状态。
 */
typedef enum {
    GW_SCRIPT_STATUS_DISABLED = 0,
    GW_SCRIPT_STATUS_DISABLED_BY_FAULT,
    GW_SCRIPT_STATUS_STARTING,
    GW_SCRIPT_STATUS_RUNNING,
    GW_SCRIPT_STATUS_STOPPED,
    GW_SCRIPT_STATUS_ERROR,
} gw_script_status_t;

/**
 * @brief 设备管理任务状态。
 */
typedef enum {
    GW_TASK_STATUS_IDLE = 0,
    GW_TASK_STATUS_BUSY,
    GW_TASK_STATUS_CONFIG_CHECKING,
    GW_TASK_STATUS_CONFIG_UPDATING,
    GW_TASK_STATUS_OTA_CHECKING,
    GW_TASK_STATUS_OTA_UPGRADING,
    GW_TASK_STATUS_LOG_UPLOADING,
    GW_TASK_STATUS_FACTORY_RESETTING,
    GW_TASK_STATUS_REBOOTING,
} gw_task_status_t;

/**
 * @brief 健康状态摘要。
 */
typedef enum {
    GW_HEALTH_STATUS_OK = 0,
    GW_HEALTH_STATUS_WARN,
    GW_HEALTH_STATUS_DEGRADED,
    GW_HEALTH_STATUS_FATAL_PENDING,
} gw_health_status_t;

/**
 * @brief 设备状态快照。
 *
 * 该结构用于跨模块只读查询，不表示每一次状态迁移历史。
 */
typedef struct {
    gw_device_status_t device_status;       /*!< 设备主状态 */
    gw_network_status_t network_status;     /*!< 网络状态 */
    gw_mqtt_status_t mqtt_status;           /*!< dev_mqtt 状态 */
    gw_script_status_t script_status;       /*!< Python 脚本状态 */
    gw_task_status_t task_status;           /*!< 当前互斥任务状态 */
    gw_health_status_t health_status;       /*!< 健康状态摘要 */
    uint64_t boot_uptime_ms;                /*!< 本次启动运行时间 */
    uint32_t event_pending_count;           /*!< dev_mgr 事件队列待处理数量 */
    uint32_t event_dropped_count;           /*!< dev_mgr 事件队列丢弃计数 */
    uint32_t log_written_count;             /*!< 已接收日志数量 */
    uint32_t log_dropped_count;             /*!< 丢弃日志数量 */
    uint32_t dev_mgr_stack_free_words;      /*!< dev_mgr_task 栈高水位 */
    uint32_t script_stack_free_words;       /*!< script_task 栈高水位 */
    uint32_t log_stack_free_words;          /*!< log_task 栈高水位 */
} gw_state_snapshot_t;

/**
 * @brief 初始化状态快照。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_state_init(void);

/**
 * @brief 获取状态快照。
 *
 * @param out 输出快照，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_state_get_snapshot(gw_state_snapshot_t *out);

/**
 * @brief 设置设备主状态。
 *
 * @param status 新状态。
 */
void gw_state_set_device_status(gw_device_status_t status);

/**
 * @brief 设置网络状态。
 *
 * @param status 新状态。
 */
void gw_state_set_network_status(gw_network_status_t status);

/**
 * @brief 设置设备管理 MQTT 状态。
 *
 * @param status 新状态。
 */
void gw_state_set_mqtt_status(gw_mqtt_status_t status);

/**
 * @brief 设置脚本状态。
 *
 * @param status 新状态。
 */
void gw_state_set_script_status(gw_script_status_t status);

/**
 * @brief 设置设备管理任务状态。
 *
 * @param status 新状态。
 */
void gw_state_set_task_status(gw_task_status_t status);

/**
 * @brief 设置健康状态摘要。
 *
 * @param status 新状态。
 */
void gw_state_set_health_status(gw_health_status_t status);

/**
 * @brief 更新任务栈高水位。
 *
 * @param task_name 任务名，支持 dev_mgr_task、script_task、log_task。
 * @param free_words 栈高水位，单位为 FreeRTOS 返回的 word。
 */
void gw_state_update_stack_high_watermark(const char *task_name, uint32_t free_words);

const char *gw_device_status_to_string(gw_device_status_t status);
const char *gw_network_status_to_string(gw_network_status_t status);
const char *gw_mqtt_status_to_string(gw_mqtt_status_t status);
const char *gw_script_status_to_string(gw_script_status_t status);
const char *gw_task_status_to_string(gw_task_status_t status);
const char *gw_health_status_to_string(gw_health_status_t status);

#ifdef __cplusplus
}
#endif
