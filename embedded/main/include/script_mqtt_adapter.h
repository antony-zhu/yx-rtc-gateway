#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_defaults.h"
#include "gw_mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file script_mqtt_adapter.h
 * @brief 脚本 MQTT 多客户端适配层接口。
 *
 * 该层把脚本 MQTT 与设备管理 dev_mqtt 完全隔离。脚本可以创建多个 MQTT client；
 * 每个 client 独立保存连接配置、事件队列和 DATA 分片组包状态。所有 DATA payload
 * 均在 PSRAM 中组包，Python 侧看到的永远是完整包。
 */

typedef int32_t script_mqtt_handle_t;

#define SCRIPT_MQTT_INVALID_HANDLE ((script_mqtt_handle_t)0)

/**
 * @brief 脚本 MQTT 配置。
 */
typedef struct {
    const char *name;             /*!< 诊断名称，可为空 */
    const char *broker_uri;       /*!< MQTT Broker URI，例如 mqtt://host:1883 */
    const char *client_id;        /*!< 脚本 MQTT client_id */
    const char *username;         /*!< 用户名，可为空 */
    const char *password;         /*!< 密码，可为空，禁止日志明文输出 */
    uint16_t keepalive_sec;       /*!< keepalive 秒数，0 使用默认值 */
    uint8_t qos_default;          /*!< 默认 QoS */
    bool clean_session;           /*!< clean session */
    bool reconnect_enabled;       /*!< 是否由 esp-mqtt 自动重连 */
} script_mqtt_config_t;

/**
 * @brief 脚本 MQTT 事件类型。
 */
typedef enum {
    SCRIPT_MQTT_EVENT_CONNECTED = 0,
    SCRIPT_MQTT_EVENT_DISCONNECTED,
    SCRIPT_MQTT_EVENT_AUTH_FAILED,
    SCRIPT_MQTT_EVENT_SUBSCRIBED,
    SCRIPT_MQTT_EVENT_UNSUBSCRIBED,
    SCRIPT_MQTT_EVENT_PUBLISHED,
    SCRIPT_MQTT_EVENT_DATA,
    SCRIPT_MQTT_EVENT_ERROR,
} script_mqtt_event_id_t;

/**
 * @brief 脚本 MQTT 事件。
 *
 * payload 由适配层分配；调用方处理完成后必须调用
 * script_mqtt_adapter_release_event() 释放。
 */
typedef struct {
    script_mqtt_handle_t handle;                       /*!< 事件所属脚本 MQTT 句柄 */
    script_mqtt_event_id_t event_id;                   /*!< 事件类型 */
    int msg_id;                                        /*!< MQTT 消息 ID */
    int esp_err;                                       /*!< ESP 错误码 */
    int connect_return_code;                           /*!< Broker 连接返回码 */
    uint64_t timestamp_ms;                             /*!< UTC 可信时为 UTC 毫秒，否则为 0 */
    char name[32];                                     /*!< client 诊断名称 */
    char topic[GW_SCRIPT_MQTT_TOPIC_MAX_LEN];          /*!< Topic，可为空 */
    uint8_t *payload;                                  /*!< Payload，可为空，位于 PSRAM */
    size_t payload_len;                                /*!< Payload 长度 */
    char reason[GW_SCRIPT_MQTT_REASON_MAX_LEN];        /*!< 错误或诊断原因 */
} script_mqtt_event_t;

/**
 * @brief 脚本 MQTT 单 client 状态。
 */
typedef struct {
    bool initialized;                                  /*!< 适配层是否已初始化 */
    bool in_use;                                      /*!< 句柄槽位是否已占用 */
    bool configured;                                  /*!< 是否已写入连接配置 */
    bool client_created;                              /*!< 底层 MQTT client 是否存在 */
    script_mqtt_handle_t handle;                      /*!< 句柄 */
    gw_mqtt_client_status_t mqtt_status;              /*!< 底层 MQTT 状态 */
    uint32_t queued_count;                            /*!< 已入队事件数 */
    uint32_t dropped_count;                           /*!< 因内存或队列满丢弃的事件数 */
    uint32_t polled_count;                            /*!< Python 已读取的事件数 */
    uint32_t publish_count;                           /*!< publish 成功次数 */
    uint32_t subscribe_count;                         /*!< subscribe 成功次数 */
    uint32_t unsubscribe_count;                       /*!< unsubscribe 成功次数 */
    uint32_t cleanup_count;                           /*!< 清理次数 */
    uint32_t queue_pending;                           /*!< 当前待读事件数 */
    char name[32];                                    /*!< client 诊断名称 */
    char broker_uri[192];                             /*!< Broker URI，不包含密码 */
    char last_reason[GW_SCRIPT_MQTT_REASON_MAX_LEN];  /*!< 最近原因 */
} script_mqtt_status_t;

/**
 * @brief 初始化脚本 MQTT 适配层。
 */
esp_err_t script_mqtt_adapter_init(void);

/**
 * @brief 创建一个脚本 MQTT client 句柄。
 *
 * @param name 诊断名称，可为空。
 * @param out_handle 输出句柄，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t script_mqtt_adapter_create(const char *name, script_mqtt_handle_t *out_handle);

/**
 * @brief 配置脚本 MQTT client。
 *
 * @param handle 脚本 MQTT 句柄。
 * @param config 连接配置，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t script_mqtt_adapter_configure(script_mqtt_handle_t handle, const script_mqtt_config_t *config);

/**
 * @brief 启动脚本 MQTT client。
 *
 * @param handle 脚本 MQTT 句柄。
 * @return ESP_OK 表示连接流程已启动。
 */
esp_err_t script_mqtt_adapter_start(script_mqtt_handle_t handle);

/**
 * @brief 停止脚本 MQTT client。
 *
 * 停止不会释放句柄和配置；后续可再次 start。
 */
esp_err_t script_mqtt_adapter_stop(script_mqtt_handle_t handle);

/**
 * @brief 销毁脚本 MQTT client。
 *
 * 销毁会断开底层 MQTT、清空事件队列和未完成 DATA 分片。
 */
esp_err_t script_mqtt_adapter_destroy(script_mqtt_handle_t handle);

/**
 * @brief 创建、配置并启动一个脚本 MQTT client。
 *
 * 兼容旧版单例 API 使用。
 */
esp_err_t script_mqtt_adapter_connect(const script_mqtt_config_t *config, script_mqtt_handle_t *out_handle);

/**
 * @brief 销毁所有脚本 MQTT client。
 */
esp_err_t script_mqtt_adapter_disconnect_all(void);

/**
 * @brief 发布脚本 MQTT 消息。
 */
esp_err_t script_mqtt_adapter_publish(script_mqtt_handle_t handle,
                                      const char *topic,
                                      const void *payload,
                                      size_t payload_len,
                                      int qos,
                                      bool retain);

/**
 * @brief 订阅脚本 MQTT Topic。
 */
esp_err_t script_mqtt_adapter_subscribe(script_mqtt_handle_t handle, const char *topic, int qos);

/**
 * @brief 取消订阅脚本 MQTT Topic。
 */
esp_err_t script_mqtt_adapter_unsubscribe(script_mqtt_handle_t handle, const char *topic);

/**
 * @brief 读取一个脚本 MQTT 事件。
 *
 * @param handle 脚本 MQTT 句柄；SCRIPT_MQTT_INVALID_HANDLE 表示扫描所有 client。
 * @param timeout_ms 最长等待时间，0 表示立即返回。
 * @param out_event 输出事件，不能为空；成功后需释放事件。
 * @return ESP_OK 表示取到事件；ESP_ERR_TIMEOUT 表示当前无事件。
 */
esp_err_t script_mqtt_adapter_recv(script_mqtt_handle_t handle,
                                   uint32_t timeout_ms,
                                   script_mqtt_event_t *out_event);

/**
 * @brief 释放脚本 MQTT 事件内持有的 PSRAM payload。
 */
void script_mqtt_adapter_release_event(script_mqtt_event_t *event);

/**
 * @brief 查询脚本 MQTT 适配层状态。
 *
 * @param handle 脚本 MQTT 句柄；SCRIPT_MQTT_INVALID_HANDLE 返回全局汇总。
 * @param out 输出状态，不能为空。
 */
esp_err_t script_mqtt_adapter_get_status(script_mqtt_handle_t handle, script_mqtt_status_t *out);

/**
 * @brief 脚本退出时清理脚本 MQTT 资源。
 */
void script_mqtt_adapter_cleanup_on_script_exit(void);

/**
 * @brief 将脚本 MQTT 事件枚举转换为 Python/API 使用的字符串。
 */
const char *script_mqtt_event_to_string(script_mqtt_event_id_t event_id);

#ifdef __cplusplus
}
#endif
