#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN          256U
#define GW_BUSINESS_MQTT_TOPIC_MAX_LEN            128U
#define GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY   64U
#define GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY  32U

#define GW_BUSINESS_MQTT_DEFAULT_TOPIC_GROUP_ID   "yx_gw"
#define GW_BUSINESS_MQTT_DEFAULT_RETRY_MS         15000U
#define GW_BUSINESS_MQTT_DEFAULT_AUTH_RETRY_MS    60000U

/**
 * @brief 业务 MQTT 会话状态。
 *
 * 状态只描述 dongle 业务通道，不会覆盖设备管理 dev_mqtt 的公共状态。
 */
typedef enum {
    GW_BUSINESS_MQTT_STATE_STOPPED = 0,   /*!< 尚未启动，或显式停止已完成 */
    GW_BUSINESS_MQTT_STATE_WAIT_NETWORK,  /*!< 等待 4G 获得可用 IP */
    GW_BUSINESS_MQTT_STATE_REGISTERING,   /*!< 正在通过 HTTPS 获取 MQTT 凭据 */
    GW_BUSINESS_MQTT_STATE_CONNECTING,    /*!< 已创建独立 client，正在连接 Broker */
    GW_BUSINESS_MQTT_STATE_SUBSCRIBING,   /*!< 已连接，正在等待下行 Topic 的 SUBACK */
    GW_BUSINESS_MQTT_STATE_ONLINE,        /*!< 订阅完成，可以收发业务消息 */
    GW_BUSINESS_MQTT_STATE_RETRY_WAIT,    /*!< 普通或鉴权失败后的退避等待 */
} gw_business_mqtt_state_t;

/**
 * @brief 业务 MQTT 服务配置。
 *
 * 字符串在 `gw_business_mqtt_init()` 内复制，调用方无需长期保存。传入 NULL 配置时
 * 使用当前业务契约的全部默认值。若传入自定义配置，空字符串和数值 0 仍使用默认值；
 * `allow_untrusted_https` 是显式布尔值，调用方需要自行设定。
 */
typedef struct {
    const char *register_url;          /*!< HTTPS 注册地址；NULL/空串使用默认地址 */
    const char *register_group_id;     /*!< 注册 query 的 groupId；默认 GID_yx_gw */
    const char *topic_group_id;        /*!< Topic 中的分组；默认 yx_gw */
    const char *business_device_id;    /*!< 可选覆盖；默认直接使用 storage 中的原始 MAC */
    uint32_t register_timeout_ms;      /*!< 单次 HTTPS 注册超时；0 使用 15000ms */
    size_t register_max_response_bytes;/*!< 注册响应上限；0 使用 1024 字节 */
    uint32_t retry_ms;                 /*!< 普通失败退避；0 使用 15000ms，范围 500~300000ms */
    uint32_t auth_retry_ms;            /*!< 鉴权失败退避；0 使用 60000ms，范围 5~3600s */
    size_t mqtt_task_stack_bytes;      /*!< ESP-MQTT task 栈；0 使用默认值，范围 4~32KiB */
    bool allow_untrusted_https;        /*!< UTC 未可信时允许发起 HTTPS，TLS 证书仍需校验 */
} gw_business_mqtt_config_t;

/**
 * @brief 一条已完成分片重组的业务 MQTT 下行消息。
 *
 * 当前只订阅固定 down Topic，但仍保留 topic，便于调用方诊断和后续扩展。结构内存由
 * 调用方持有，不需要额外释放。
 */
typedef struct {
    char topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];      /*!< 完整下行 Topic */
    uint8_t payload[GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN]; /*!< 原始 MQTT Payload */
    size_t payload_len;                              /*!< Payload 实际长度 */
} gw_business_mqtt_message_t;

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
    uint32_t published_count;           /*!< 已收到 QoS1 PUBACK 的上行条数 */
    uint32_t received_count;            /*!< 完成重组并交付下行队列的条数 */
    uint32_t outbound_dropped_count;     /*!< 上行 FIFO 满时丢弃最旧消息的次数 */
    uint32_t downlink_dropped_count;     /*!< 下行队列满或分片非法的丢弃次数 */
    uint32_t event_dropped_count;        /*!< MQTT 回调事件队列满或数据超限次数 */
    size_t outbound_pending;             /*!< 当前离线上行 FIFO 条数 */
    size_t downlink_pending;             /*!< 当前待领取下行条数 */
    uint32_t retry_remaining_ms;         /*!< 当前退避剩余时间；未退避时为 0 */
    int last_esp_err;                    /*!< 最近 ESP-IDF 错误码 */
    int last_connect_return_code;        /*!< 最近 Broker 连接返回码 */
    char business_device_id[32];         /*!< 业务设备号，默认等于大写原始 MAC */
    char up_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];   /*!< 固定上行 Topic */
    char down_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN]; /*!< 固定下行 Topic */
    char last_reason[64];                /*!< 最近状态原因，不含敏感数据 */
} gw_business_mqtt_status_t;

/**
 * @brief 初始化业务 MQTT 服务及其固定容量队列。
 *
 * 默认从 `gw_storage_get_identity()` 读取大写原始 MAC，并直接作为业务设备号。
 * 离线上行及收发队列存储一次性分配在 PSRAM，队列控制块和 worker 使用静态内存。
 * 初始化不会开始注册或连接；重复调用不会重配实例。
 *
 * @param config 可选配置；NULL 使用业务默认值。
 * @return ESP_OK 表示初始化成功；身份、字符串长度或内存资源异常时返回对应错误。
 */
esp_err_t gw_business_mqtt_init(const gw_business_mqtt_config_t *config);

/**
 * @brief 启动业务 MQTT 状态机。
 *
 * worker 会先等待 4G 可用，再执行 HTTPS 注册、创建独立 client、连接和订阅。该函数
 * 不等待网络或 Broker 上线，重复启动视为成功。
 *
 * @return ESP_OK 表示启动请求已提交。
 */
esp_err_t gw_business_mqtt_start(void);

/**
 * @brief 显式停止业务 MQTT。
 *
 * 只有显式停止才会清空上行 FIFO、下行队列和注册凭据。普通断网、连接失败及鉴权失败
 * 均保留上行 FIFO。注册 HTTP 正在阻塞时，停止可能等待到本次 HTTP 调用返回。
 *
 * @param timeout_ms 等待 worker 完成清理的最长时间；0 表示只检查即时结果。
 * @return ESP_OK 表示已停止；超时返回 ESP_ERR_TIMEOUT。
 */
esp_err_t gw_business_mqtt_stop(uint32_t timeout_ms);

/**
 * @brief 提交一条业务 MQTT 上行消息。
 *
 * 所有消息先进入 64 条 RAM FIFO，再由在线 worker 按 FIFO 顺序以 QoS1 发布；只有收到
 * 当前会话 PUBACK 后才移除队首。FIFO 已满时丢弃最旧的非在途条目并接收新消息。
 * 返回 ESP_OK 仅表示消息已进入本地发送流程。
 *
 * @param payload 原始 Payload；长度为 0 时允许为 NULL。
 * @param payload_len Payload 长度，不能超过 256 字节。
 * @return ESP_OK 表示已入队；未初始化、未启动或参数非法时返回错误。
 */
esp_err_t gw_business_mqtt_send(const void *payload, size_t payload_len);

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
