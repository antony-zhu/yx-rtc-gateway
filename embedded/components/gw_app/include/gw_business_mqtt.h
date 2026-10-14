#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_defaults.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN          256U
/* 管理协议与底层 dev_mqtt 共用的最大 MQTT Payload。 */
#define GW_BUSINESS_MQTT_MANAGEMENT_PAYLOAD_MAX_LEN GW_MQTT_MAX_PAYLOAD_SIZE
#define GW_BUSINESS_MQTT_TOPIC_MAX_LEN            128U
#define GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY   64U
#define GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY  32U

#define GW_BUSINESS_MQTT_DEFAULT_TOPIC_GROUP_ID   "yx_gw"
#define GW_BUSINESS_MQTT_DEFAULT_RETRY_MS         5000U
#define GW_BUSINESS_MQTT_DEFAULT_AUTH_RETRY_MS    60000U

/**
 * @brief 统一 dev_mqtt 会话状态。
 *
 * 该会话同时承载设备管理 JSON 和 Dongle 原始二进制；生产启动路径只创建这一条
 * MQTT client，client name/role 固定为 `dev_mqtt`。
 */
typedef enum {
    GW_BUSINESS_MQTT_STATE_STOPPED = 0,   /*!< 尚未启动，或显式停止已完成 */
    GW_BUSINESS_MQTT_STATE_WAIT_NETWORK,  /*!< 等待编译选择的网络获得可用 IP */
    GW_BUSINESS_MQTT_STATE_REGISTERING,   /*!< 正在通过 HTTPS 获取 MQTT 凭据 */
    GW_BUSINESS_MQTT_STATE_CONNECTING,    /*!< 已创建 dev_mqtt，正在连接 Broker */
    GW_BUSINESS_MQTT_STATE_SUBSCRIBING,   /*!< 已连接，正在等待下行 Topic 的 SUBACK */
    GW_BUSINESS_MQTT_STATE_ONLINE,        /*!< 订阅完成，可以收发业务消息 */
    GW_BUSINESS_MQTT_STATE_RETRY_WAIT,    /*!< 普通或鉴权失败后的退避等待 */
} gw_business_mqtt_state_t;

/**
 * @brief 业务 MQTT 服务配置。
 *
 * 字符串在 `gw_business_mqtt_init()` 内复制，调用方无需长期保存。传入 NULL 配置时
 * 使用当前业务契约的全部默认值。若传入自定义配置，空字符串和数值 0 仍使用默认值；
 * 注册/Topic 分组和业务设备号是固定契约，相关兼容字段不会改变其值。
 * `allow_untrusted_https` 是显式布尔值，调用方需要自行设定。
 */
typedef struct {
    const char *register_url;          /*!< HTTPS 注册地址；NULL/空串使用默认地址 */
    /*
     * The following identity fields are retained for source compatibility with older
     * callers.  The current business contract fixes them to GID_yx_gw, yx_gw, and the
     * ESP32 eFuse MAC respectively; supplied overrides are ignored by the implementation.
     */
    const char *register_group_id;     /*!< 保留兼容字段；注册 groupId 固定为 GID_yx_gw */
    const char *topic_group_id;        /*!< 保留兼容字段；Topic 分组固定为 yx_gw */
    const char *business_device_id;    /*!< 保留兼容字段；设备号固定为 ESP32 eFuse MAC */
    uint32_t register_timeout_ms;      /*!< 单次 HTTPS 注册超时；0 使用 15000ms */
    size_t register_max_response_bytes;/*!< 注册响应上限；0 使用 1024 字节 */
    uint32_t retry_ms;                 /*!< 连接断开重建退避；0 使用 5000ms，范围 500~300000ms */
    uint32_t auth_retry_ms;            /*!< 鉴权失败退避；0 使用 60000ms，范围 5~3600s */
    size_t mqtt_task_stack_bytes;      /*!< ESP-MQTT task 栈；0 使用默认值，范围 4~32KiB */
    bool allow_untrusted_https;        /*!< UTC 未可信时允许发起 HTTPS，TLS 证书仍需校验 */
} gw_business_mqtt_config_t;

/**
 * @brief 一条兼容旧调用方的原始下行消息。
 *
 * `gw_business_mqtt_recv()` 仍以调用方提供的固定缓冲返回 Dongle 小帧。需要接收完整
 * 管理协议（最大 `GW_MQTT_MAX_PAYLOAD_SIZE`）时使用下面的 owned message API。
 */
typedef struct {
    char topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];      /*!< 完整下行 Topic */
    uint8_t payload[GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN]; /*!< 原始 MQTT Payload */
    size_t payload_len;                              /*!< Payload 实际长度 */
} gw_business_mqtt_message_t;

/**
 * @brief 一条由统一 dev_mqtt transport 持有的完整下行消息。
 *
 * topic/payload 均在 PSRAM 中分配，并额外补一个 `\\0`（payload_len 仍是原始字节数）。
 * 成功从 `gw_business_mqtt_recv_raw()` 或 `gw_business_mqtt_recv_management()` 领取后，
 * 调用方必须调用 `gw_business_mqtt_message_release()` 释放。结构本身可以在任务栈上，
 * 其中的大缓冲不会占用任务栈。
 */
typedef struct {
    char *topic;             /*!< 完整 MQTT Topic，已补 `\\0` */
    uint8_t *payload;        /*!< 原始 MQTT Payload，已补 `\\0` */
    size_t payload_len;      /*!< Payload 原始字节长度 */
    bool retained;           /*!< MQTT retained 标志 */
} gw_business_mqtt_owned_message_t;

/* 管理消息的更明确别名，便于协议层表达消息来源。 */
typedef gw_business_mqtt_owned_message_t gw_business_mqtt_management_message_t;

/**
 * @brief 业务 MQTT 只读状态快照。
 *
 * 快照不包含用户名、密码、Broker URI query 等敏感信息。
 */
typedef struct {
    bool initialized;                   /*!< 服务是否已初始化 */
    bool start_requested;               /*!< 是否处于显式 start 后、stop 前 */
    bool credentials_valid;             /*!< 内存中是否存在一组已注册凭据 */
    bool online;                        /*!< 已连接且 down Topic 已收到成功 SUBACK */
    gw_business_mqtt_state_t state;     /*!< 当前状态机状态 */
    uint32_t register_attempts;         /*!< HTTPS 注册尝试次数 */
    uint32_t connect_attempts;          /*!< MQTT 连接尝试次数 */
    uint32_t connected_count;           /*!< MQTT CONNECTED 次数 */
    uint32_t subscribe_count;           /*!< 成功提交的订阅请求次数 */
    uint32_t published_count;           /*!< 已提交到底层 MQTT 发送队列的上行条数（QoS0） */
    uint32_t received_count;            /*!< 完成重组并交付下行队列的条数 */
    uint32_t outbound_dropped_count;     /*!< 未在线或 FIFO 满时丢弃上行消息的次数 */
    uint32_t downlink_dropped_count;     /*!< 下行队列满或分片非法的丢弃次数 */
    uint32_t event_dropped_count;        /*!< MQTT 回调事件队列满或数据超限次数 */
    size_t outbound_pending;             /*!< 当前在线上行 FIFO 条数 */
    size_t downlink_pending;             /*!< 当前待领取下行条数（raw+management） */
    size_t raw_downlink_pending;         /*!< 当前待领取 Dongle 原始下行条数 */
    size_t management_downlink_pending;  /*!< 当前待领取管理 JSON 下行条数 */
    uint32_t retry_remaining_ms;         /*!< 当前退避剩余时间；未退避时为 0 */
    int last_esp_err;                    /*!< 最近 ESP-IDF 错误码 */
    int last_connect_return_code;        /*!< 最近 Broker 连接返回码 */
    char business_device_id[32];         /*!< 业务设备号，固定等于 eFuse MAC 大写文本 */
    char up_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];   /*!< 固定原始上行 Topic */
    char up_config_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN]; /*!< 配置控制应答 Topic */
    char down_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN]; /*!< 固定原始下行 Topic */
    char down_config_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN]; /*!< 配置控制下行 Topic */
    char last_reason[64];                /*!< 最近状态原因，不含敏感数据 */
} gw_business_mqtt_status_t;

/**
 * @brief 初始化业务 MQTT 服务及其固定容量队列。
 *
 * 默认从 `gw_storage_get_identity()` 读取由 eFuse 派生的大写原始 MAC，并作为业务设备号。
 * 在线上行及收发队列存储一次性分配在 PSRAM，队列控制块和 worker 使用静态内存。
 * 初始化不会开始注册或连接；重复调用不会重配实例。
 *
 * @param config 可选配置；NULL 使用业务默认值。
 * @return ESP_OK 表示初始化成功；身份、字符串长度或内存资源异常时返回对应错误。
 */
esp_err_t gw_business_mqtt_init(const gw_business_mqtt_config_t *config);

/**
 * @brief 启动业务 MQTT 状态机。
 *
 * worker 会先等待网络可用，再执行 HTTPS 注册、创建唯一 dev_mqtt client、连接和订阅。该函数
 * 不等待网络或 Broker 上线，重复启动视为成功。
 *
 * @return ESP_OK 表示启动请求已提交。
 */
esp_err_t gw_business_mqtt_start(void);

/**
 * @brief 显式停止业务 MQTT。
 *
 * 显式停止会清空上行 FIFO、下行队列和注册凭据；QoS0 业务上行在断网或重建 client 时
 * 也会丢弃尚未提交的 FIFO 条目。注册 HTTP 正在阻塞时，停止可能等待到本次 HTTP 调用返回。
 *
 * @param timeout_ms 等待 worker 完成清理的最长时间；0 表示只检查即时结果。
 * @return ESP_OK 表示已停止；超时返回 ESP_ERR_TIMEOUT。
 */
esp_err_t gw_business_mqtt_stop(uint32_t timeout_ms);

/**
 * @brief 提交一条业务 MQTT 上行消息。
 *
 * 在线消息先进入 64 条 PSRAM FIFO，再由 worker 按 FIFO 顺序以 QoS0 发布；底层提交成功
 * 后立即移除队首并计数，不等待 PUBACK。MQTT 尚未完成下行订阅时直接丢弃消息，不做离线
 * 缓存。FIFO 已满时丢弃最旧条目并接收新消息。
 * 返回 ESP_OK 仅表示消息已进入本地发送流程。
 *
 * @param payload 原始 Payload；长度为 0 时允许为 NULL。
 * @param payload_len Payload 长度，不能超过 256 字节。
 * @return ESP_OK 表示已入队；未初始化、未启动、尚未 ONLINE 或参数非法时返回错误。
 */
esp_err_t gw_business_mqtt_send(const void *payload, size_t payload_len);

/**
 * @brief 提交统一 dev_mqtt 上行消息。
 *
 * 这是管理协议和其它应用上报共用的上行入口。为避免绕过 Lua 兼容契约，topic 必须
 * 精确等于初始化后状态中的固定 `up_topic`；Payload 最大为 `GW_MQTT_MAX_PAYLOAD_SIZE`，
 * 发布 QoS 固定为 0、retain 固定为 false。成功只表示进入本地发送 FIFO。
 */
esp_err_t gw_business_mqtt_publish(const char *topic,
                                   const void *payload,
                                   size_t payload_len);

typedef void (*gw_business_mqtt_gateway_done_fn)(void *context, esp_err_t result);

/**
 * @brief 提交新网关协议 /up 消息到独立的 32 条 FIFO。
 *
 * 仅在网关 /down 的 QoS0 SUBACK 成功后接受消息。发布尝试至少间隔 1000ms，固定 QoS0、
 * retain=false；失败不重试，断线或停止后不补发。队满时拒绝新消息。
 * 接受成功后 done（非 NULL 时）由独立发送任务在尝试或丢弃后恰好调用一次，且不持锁。
 * 入队返回错误时不会调用 done。Payload 复制到 PSRAM，长度不受旧协议 8KiB 限制。
 */
esp_err_t gw_business_mqtt_publish_gateway(const void *payload,
                                          size_t payload_len,
                                          gw_business_mqtt_gateway_done_fn done,
                                          void *context);

/**
 * @brief 提交统一 dev_mqtt 上行并等待底层发送队列接受。
 *
 * QoS0 没有 PUBACK；该接口等待获得统一 transport 的发布互斥并同步调用底层 enqueue，
 * 返回时表示消息已经交给 ESP-MQTT outbox（不是 Broker 已确认）。
 */
esp_err_t gw_business_mqtt_publish_wait(const char *topic,
                                        const void *payload,
                                        size_t payload_len,
                                        uint32_t timeout_ms);

/**
 * @brief 获取一条完整业务 MQTT 下行消息。
 *
 * MQTT DATA 分片由 worker 按 msg_id、offset 和 total_data_len 严格重组；该接口不会
 * 返回半包。只能在普通任务上下文调用。
 *
 * @param out_message 输出消息，不能为空。
 * @param timeout_ms 等待时间；0 表示非阻塞。
 * @return ESP_OK 表示成功；队列为空或等待超时返回 ESP_ERR_TIMEOUT。
 */
esp_err_t gw_business_mqtt_recv(gw_business_mqtt_message_t *out_message,
                                 uint32_t timeout_ms);

/**
 * @brief 获取一条动态分配的 Dongle 原始下行。
 *
 * 与旧 `gw_business_mqtt_recv()` 相比不会截断 Payload；调用方领取后必须释放消息。
 */
esp_err_t gw_business_mqtt_recv_raw(gw_business_mqtt_owned_message_t *out_message,
                                     uint32_t timeout_ms);

/**
 * @brief 获取一条动态分配的管理协议下行。
 *
 * transport 按 Payload 是否为 JSON 对象/数组将固定 down_raw Topic 分流。管理消息最大
 * `GW_MQTT_MAX_PAYLOAD_SIZE` 字节，调用方领取后必须调用 release。
 */
esp_err_t gw_business_mqtt_recv_management(gw_business_mqtt_management_message_t *out_message,
                                            uint32_t timeout_ms);

/**
 * @brief 释放动态下行消息及其 PSRAM 缓冲。
 *
 * 可安全传入 NULL；释放后结构字段会清零，避免重复释放。
 */
void gw_business_mqtt_message_release(gw_business_mqtt_owned_message_t *message);

/** @brief 领取新网关 /down 完整消息；调用方随后必须调用 message_release。 */
esp_err_t gw_business_mqtt_recv_gateway(gw_business_mqtt_owned_message_t *out_message,
                                       uint32_t timeout_ms);

/**
 * @brief 获取业务 MQTT 状态快照。
 *
 * @param out_status 输出状态，不能为空。
 * @return ESP_OK 表示成功；未初始化返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t gw_business_mqtt_get_status(gw_business_mqtt_status_t *out_status);

/**
 * @brief 将业务 MQTT 状态转换为稳定英文名称。
 *
 * @param state 状态枚举。
 * @return 静态字符串；未知状态返回 "unknown"。
 */
const char *gw_business_mqtt_state_to_string(gw_business_mqtt_state_t state);

#ifdef __cplusplus
}
#endif
