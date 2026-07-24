#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT client 角色。
 */
typedef enum {
    GW_MQTT_CLIENT_ROLE_SYSTEM_DEV = 0,  /*!< 设备管理 MQTT，也就是 dev_mqtt */
    GW_MQTT_CLIENT_ROLE_SCRIPT,          /*!< 脚本侧 MQTT */
    GW_MQTT_CLIENT_ROLE_BUSINESS,        /*!< 固件原生业务 MQTT */
} gw_mqtt_client_role_t;

/**
 * @brief 通用 MQTT 状态。
 */
typedef enum {
    GW_MQTT_CLIENT_STATUS_IDLE = 0,       /*!< 已创建但未连接 */
    GW_MQTT_CLIENT_STATUS_CONNECTING,     /*!< 正在连接 */
    GW_MQTT_CLIENT_STATUS_CONNECTED,      /*!< 已连接 */
    GW_MQTT_CLIENT_STATUS_DISCONNECTED,   /*!< 已断开 */
    GW_MQTT_CLIENT_STATUS_AUTH_FAILED,    /*!< 鉴权失败 */
    GW_MQTT_CLIENT_STATUS_ERROR,          /*!< 其他错误 */
} gw_mqtt_client_status_t;

/**
 * @brief 通用 MQTT 事件。
 */
typedef enum {
    GW_MQTT_CLIENT_EVENT_CONNECTED = 0,
    GW_MQTT_CLIENT_EVENT_DISCONNECTED,
    GW_MQTT_CLIENT_EVENT_AUTH_FAILED,
    GW_MQTT_CLIENT_EVENT_SUBSCRIBED,
    GW_MQTT_CLIENT_EVENT_UNSUBSCRIBED,
    GW_MQTT_CLIENT_EVENT_PUBLISHED,
    GW_MQTT_CLIENT_EVENT_DATA,
    GW_MQTT_CLIENT_EVENT_ERROR,
} gw_mqtt_client_event_id_t;

typedef struct gw_mqtt_client gw_mqtt_client_t;

/**
 * @brief MQTT 配置。
 */
typedef struct {
    gw_mqtt_client_role_t role;              /*!< client 角色 */
    const char *name;                  /*!< 实例名，例如 dev_mqtt */
    const char *broker_uri;            /*!< Broker URI */
    const char *client_id;             /*!< MQTT Client ID */
    const char *username;              /*!< MQTT 用户名，可为空 */
    const char *password;              /*!< MQTT 密码，可为空，禁止打印 */
    uint16_t keepalive_sec;            /*!< keepalive 秒数，0 使用默认值 */
    uint8_t qos_default;               /*!< 默认 QoS */
    bool clean_session;                /*!< clean session */
    bool retain_default;               /*!< 默认 retain */
    bool last_will_enabled;            /*!< 是否启用 Last Will */
    const char *last_will_topic;       /*!< Last Will Topic，启用时不能为空 */
    const char *last_will_payload;     /*!< Last Will Payload，启用时不能为空 */
    size_t last_will_payload_len;      /*!< Last Will Payload 长度，0 表示按字符串长度计算 */
    uint8_t last_will_qos;             /*!< Last Will QoS，取值 0、1 或 2 */
    bool last_will_retain;             /*!< Last Will retain 标志 */
    bool reconnect_enabled;            /*!< 是否启用自动重连 */
    uint16_t reconnect_min_sec;        /*!< 最小重连间隔 */
    uint16_t reconnect_max_sec;        /*!< 最大重连间隔 */
    size_t max_payload_size;           /*!< Payload 最大字节数，0 使用默认值 */
    size_t task_stack_bytes;           /*!< MQTT task 栈大小，0 使用项目默认值 */
} gw_mqtt_client_config_t;

/**
 * @brief MQTT 事件内容。
 *
 * topic 和 data 指针仅在回调期间有效；若接收方要异步处理，必须复制。
 */
typedef struct {
    gw_mqtt_client_event_id_t event_id;      /*!< 事件 ID */
    int msg_id;                        /*!< ESP-MQTT 消息 ID */
    const char *topic;                 /*!< 收到的 topic，可为空 */
    int topic_len;                     /*!< topic 长度 */
    const char *data;                  /*!< 收到的 payload，可为空 */
    int data_len;                      /*!< payload 当前片段长度 */
    int total_data_len;                /*!< payload 总长度 */
    int current_data_offset;           /*!< 当前片段偏移 */
    int esp_err;                       /*!< ESP 错误码 */
    int connect_return_code;           /*!< Broker 拒绝连接返回码 */
    bool subscribe_failed;             /*!< SUBACK 是否明确拒绝了本次订阅 */
} gw_mqtt_client_event_t;

/**
 * @brief MQTT 事件回调。
 *
 * 回调中不得执行长耗时业务；应复制必要数据并投递到对应队列。
 */
typedef void (*gw_mqtt_client_event_cb_t)(gw_mqtt_client_t *client,
                                    const gw_mqtt_client_event_t *event,
                                    void *user_ctx);

/**
 * @brief MQTT 状态快照。
 */
typedef struct {
    gw_mqtt_client_role_t role;              /*!< client 角色 */
    gw_mqtt_client_status_t status;          /*!< 当前连接状态 */
    uint32_t connected_count;          /*!< 连接成功次数 */
    uint32_t disconnected_count;       /*!< 断开次数 */
    uint32_t error_count;              /*!< 错误次数 */
    int last_msg_id;                   /*!< 最近消息 ID */
    int last_esp_err;                  /*!< 最近 ESP 错误 */
    int last_connect_return_code;      /*!< 最近 Broker 连接返回码 */
    char name[32];                     /*!< 实例名 */
    char broker_uri[192];              /*!< Broker URI，不含密码 */
} gw_mqtt_client_status_snapshot_t;

/**
 * @brief 创建通用 MQTT client。
 *
 * 该接口只创建底层 esp-mqtt 实例并注册事件回调，不会立即连接 Broker。client 内部会
 * 保存 broker_uri、client_id、username、password 的副本；密码禁止日志明文输出。
 *
 * @param config MQTT 配置，不能为空。
 * @param event_cb 事件回调，可为空；回调内不得执行长耗时操作。
 * @param user_ctx 传给事件回调的用户上下文。
 * @param out_client 输出 client 句柄，不能为空。
 * @return ESP_OK 表示创建成功。
 */
esp_err_t gw_mqtt_client_create(const gw_mqtt_client_config_t *config,
                          gw_mqtt_client_event_cb_t event_cb,
                          void *user_ctx,
                          gw_mqtt_client_t **out_client);

/**
 * @brief 销毁 MQTT client。
 *
 * 如果 client 已启动，会先尝试停止底层 esp-mqtt。调用后句柄失效。
 *
 * @param client MQTT client 句柄。
 * @return ESP_OK 表示销毁成功。
 */
esp_err_t gw_mqtt_client_destroy(gw_mqtt_client_t *client);

/**
 * @brief 启动 MQTT 连接。
 *
 * @param client MQTT client 句柄。
 * @return ESP_OK 表示连接流程已启动；连接成功需等待 CONNECTED 事件。
 */
esp_err_t gw_mqtt_client_connect(gw_mqtt_client_t *client);

/**
 * @brief 停止 MQTT 连接。
 *
 * @param client MQTT client 句柄。
 * @return ESP_OK 表示停止成功或原本未启动。
 */
esp_err_t gw_mqtt_client_disconnect(gw_mqtt_client_t *client);

/**
 * @brief 发布 MQTT 消息。
 *
 * 该函数是 `gw_mqtt_client_publish_with_msg_id()` 的简化形式，不返回消息 ID。
 *
 * @param client MQTT client 句柄。
 * @param topic 目标 topic。
 * @param payload payload 数据，可为空。
 * @param payload_len payload 长度。
 * @param qos QoS，取值 0、1 或 2。
 * @param retain 是否 retain。
 * @return ESP_OK 表示发布请求已提交。
 */
esp_err_t gw_mqtt_client_publish(gw_mqtt_client_t *client,
                           const char *topic,
                           const void *payload,
                           size_t payload_len,
                           int qos,
                           bool retain);
/**
 * @brief 发布 MQTT 消息，并返回本次 PUBLISH 的消息 ID。
 *
 * 返回 ESP_OK 仅表示消息已提交给 ESP-MQTT 发送队列，不代表 Broker 已确认。
 * 如需等待 QoS1 PUBACK，应由调用方结合 GW_MQTT_CLIENT_EVENT_PUBLISHED 事件处理。
 *
 * @param client MQTT client 句柄，不能为 NULL。
 * @param topic 目标 topic，不能为空字符串。
 * @param payload payload 数据，可为空。
 * @param payload_len payload 长度。
 * @param qos QoS，取值 0、1 或 2。
 * @param retain 是否 retain。
 * @param out_msg_id 输出 ESP-MQTT 消息 ID，可为 NULL。
 * @return ESP_OK 表示发布请求已提交。
 */
esp_err_t gw_mqtt_client_publish_with_msg_id(gw_mqtt_client_t *client,
                                       const char *topic,
                                       const void *payload,
                                       size_t payload_len,
                                       int qos,
                                       bool retain,
                                       int *out_msg_id);
/**
 * @brief 订阅 MQTT Topic，并返回本次 SUBSCRIBE 的消息 ID。
 *
 * 该接口用于需要把后续 SUBACK 与订阅请求对应起来的场景。普通调用可继续使用
 * `gw_mqtt_client_subscribe()`。
 *
 * @param client MQTT client 句柄，不能为 NULL。
 * @param topic 待订阅 topic，不能为空字符串。
 * @param qos 订阅 QoS，取值 0、1 或 2。
 * @param out_msg_id 输出 ESP-MQTT 消息 ID，可为 NULL。
 * @return ESP_OK 表示订阅请求已提交；其他值表示参数、状态或底层提交失败。
 */
esp_err_t gw_mqtt_client_subscribe_with_msg_id(gw_mqtt_client_t *client,
                                         const char *topic,
                                       int qos,
                                       int *out_msg_id);

/**
 * @brief 订阅 MQTT Topic。
 *
 * 该函数是 `gw_mqtt_client_subscribe_with_msg_id()` 的简化形式，不返回消息 ID。
 *
 * @param client MQTT client 句柄。
 * @param topic 待订阅 topic。
 * @param qos 订阅 QoS，取值 0、1 或 2。
 * @return ESP_OK 表示订阅请求已提交。
 */
esp_err_t gw_mqtt_client_subscribe(gw_mqtt_client_t *client, const char *topic, int qos);

/**
 * @brief 取消订阅 MQTT Topic。
 *
 * @param client MQTT client 句柄。
 * @param topic 待取消订阅 topic。
 * @return ESP_OK 表示取消订阅请求已提交。
 */
esp_err_t gw_mqtt_client_unsubscribe(gw_mqtt_client_t *client, const char *topic);

/**
 * @brief 获取 MQTT client 状态快照。
 *
 * @param client MQTT client 句柄。
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_mqtt_client_get_status(gw_mqtt_client_t *client, gw_mqtt_client_status_snapshot_t *out);

/**
 * @brief 将 MQTT 状态转换为字符串。
 *
 * @param status 状态枚举。
 * @return 状态名称；未知值返回 "unknown"。
 */
const char *gw_mqtt_client_status_to_string(gw_mqtt_client_status_t status);

/**
 * @brief 将 MQTT 事件转换为字符串。
 *
 * @param event_id 事件枚举。
 * @return 事件名称；未知值返回 "unknown"。
 */
const char *gw_mqtt_client_event_to_string(gw_mqtt_client_event_id_t event_id);

#ifdef __cplusplus
}
#endif
