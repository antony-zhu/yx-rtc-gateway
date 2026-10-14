#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 系统事件优先级。
 */
typedef enum {
    GW_EVENT_PRIORITY_LOW = 0,
    GW_EVENT_PRIORITY_NORMAL,
    GW_EVENT_PRIORITY_HIGH,
    GW_EVENT_PRIORITY_CRITICAL,
} gw_event_priority_t;

/**
 * @brief 系统内部事件 ID。
 */
typedef enum {
    GW_EVT_BOOT_READY = 0,
    GW_EVT_STORAGE_READY,
    GW_EVT_STORAGE_FAULT,
    GW_EVT_DATAFS_FAULT,
    GW_EVT_LOGFS_FAULT,
    GW_EVT_NET_STARTING,
    GW_EVT_NET_READY,
    GW_EVT_NET_LOST,
    GW_EVT_NET_FAILED,
    GW_EVT_NET_DEGRADED,
    GW_EVT_MQTT_CONNECTED,
    GW_EVT_MQTT_DISCONNECTED,
    GW_EVT_MQTT_AUTH_FAILED,
    GW_EVT_MQTT_DOWNLINK,
    GW_EVT_MQTT_PUBLISH_DONE,
    GW_EVT_TIME_TRUSTED,
    GW_EVT_TIME_UNTRUSTED,
    GW_EVT_TIME_SYNC_FAILED,
    GW_EVT_SCHEDULE_DUE,
    GW_EVT_PERIODIC_TICK,
    GW_EVT_PROTOCOL_REQUEST_ACCEPTED,
    GW_EVT_PROTOCOL_REQUEST_REJECTED,
    GW_EVT_CONFIG_CHECK_REQUEST,
    GW_EVT_CONFIG_UPDATE_REQUEST,
    GW_EVT_CONFIG_PROGRESS,
    GW_EVT_CONFIG_DONE,
    GW_EVT_CONFIG_FAILED,
    GW_EVT_OTA_CHECK_REQUEST,
    GW_EVT_OTA_UPGRADE_REQUEST,
    GW_EVT_OTA_PROGRESS,
    GW_EVT_OTA_DONE,
    GW_EVT_OTA_FAILED,
    GW_EVT_LOG_CLEAN_DONE,
    GW_EVT_LOG_UPLOAD_DONE,
    GW_EVT_LOG_FAULT,
    GW_EVT_HEALTH_WARN,
    GW_EVT_HEALTH_FATAL,
    GW_EVT_REBOOT_REQUESTED,
    GW_EVT_FACTORY_RESET_REQUESTED,
} gw_event_id_t;

/**
 * @brief 事件 payload 释放函数。
 *
 * 事件投递成功后，如 release_cb 非空，payload 所有权归接收方。
 */
typedef void (*gw_event_release_cb_t)(void *payload);

#define GW_EVENT_RESULT_REASON_COPY_LEN 64U
#define GW_EVENT_RESULT_DETAIL_COPY_LEN 128U

/**
 * @brief 跨模块事件包。
 *
 * `result.reason_code` 与 `result.detail` 在 `gw_event_post()` 入队时会复制到
 * 事件自带缓冲区，因此调用方允许传入当前函数栈上的临时字符串。接收方不得依赖
 * 下方 `_result_*` 字段的具体布局，它们只用于队列内部的生命周期管理。
 */
typedef struct {
    gw_event_id_t event_id;                /*!< 事件 ID */
    gw_event_priority_t priority;          /*!< 事件优先级 */
    const char *source;                    /*!< 来源模块名，静态字符串或生命周期足够长的字符串 */
    const char *target;                    /*!< 目标模块名，可为空 */
    const char *request_id;                /*!< 关联平台请求 ID，可为空 */
    const char *method;                    /*!< 关联平台方法或内部请求名，可为空 */
    uint64_t uptime_ms;                    /*!< 单调运行时间 */
    uint64_t timestamp;                    /*!< 可信 UTC 毫秒；不可信时为 0 */
    gw_result_t result;                    /*!< 事件结果 */
    void *payload;                         /*!< 可选负载，不允许指向栈内存 */
    size_t payload_len;                    /*!< 负载长度 */
    gw_event_release_cb_t release_cb;      /*!< 负载释放回调 */
    char _result_reason_copy[GW_EVENT_RESULT_REASON_COPY_LEN]; /*!< 队列内部使用 */
    char _result_detail_copy[GW_EVENT_RESULT_DETAIL_COPY_LEN]; /*!< 队列内部使用 */
    bool _result_reason_copied;             /*!< 队列内部使用 */
    bool _result_detail_copied;             /*!< 队列内部使用 */
} gw_event_t;

/**
 * @brief 初始化设备管理事件队列。
 *
 * @return ESP_OK 表示成功；重复初始化也返回 ESP_OK。
 */
esp_err_t gw_event_init(void);

/**
 * @brief 非阻塞投递事件到设备管理事件队列。
 *
 * 事件结构以及结果中的错误短码、诊断摘要会被复制到队列中。普通、高优先级和
 * 关键事件分别进入独立 FIFO，并各自拥有固定容量；接收方优先处理关键和高优先级，
 * 同一优先级内仍保持投递顺序。若事件携带 payload 且投递成功，payload 所有权按
 * release_cb 约定转移给接收方；投递失败时仍由调用方负责释放。
 *
 * @param event 待投递事件，不能为空。
 * @return ESP_OK 表示成功；ESP_ERR_TIMEOUT 表示队列满；其他错误表示参数或状态异常。
 */
esp_err_t gw_event_post(const gw_event_t *event);

/**
 * @brief 从设备管理事件队列读取一个事件。
 *
 * @param out_event 输出事件。
 * @param timeout_ms 等待时间，0 表示不等待，UINT32_MAX 表示永久等待。
 * @return ESP_OK 表示读取成功；ESP_ERR_TIMEOUT 表示超时或无事件。
 */
esp_err_t gw_event_receive(gw_event_t *out_event, uint32_t timeout_ms);

/**
 * @brief 获取事件队列中等待处理的事件数量。
 *
 * @return 当前等待处理的事件数量；未初始化时返回 0。
 */
uint32_t gw_event_get_pending_count(void);

/**
 * @brief 获取事件队列满导致的投递失败次数。
 *
 * @return 累计丢弃计数。
 */
uint32_t gw_event_get_dropped_count(void);

/**
 * @brief 将事件 ID 转成稳定短字符串。
 *
 * @param event_id 事件 ID。
 * @return 静态字符串，未知事件返回 unknown。
 */
const char *gw_event_id_to_string(gw_event_id_t event_id);

#ifdef __cplusplus
}
#endif
