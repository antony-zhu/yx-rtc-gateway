#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief dev_mqtt 下行消息负载。
 *
 * topic 和 payload 均由 gw_mqtt_manager 分配，事件接收方处理完成后必须通过
 * gw_mqtt_downlink_release 释放。payload 会额外补一个 '\0'，方便 JSON 协议层解析，
 * 但 payload_len 始终表示 MQTT 原始字节长度。
 */
typedef struct {
    char *topic;             /*!< MQTT Topic，已补 '\0' */
    char *payload;           /*!< MQTT Payload，已补 '\0' */
    size_t payload_len;      /*!< MQTT Payload 原始字节长度 */
} gw_mqtt_downlink_t;

/**
 * @brief dev_mqtt 管理状态快照。
 */
typedef struct {
    bool initialized;                   /*!< 模块是否已初始化 */
    bool config_loaded;                 /*!< 是否已从 NVS 读取 MQTT 信息 */
    bool heartbeat_enabled;             /*!< 是否启用设备状态心跳上报 */
    gw_mqtt_client_status_t mqtt_status;      /*!< 底层 MQTT 状态 */
    uint32_t heartbeat_interval_sec;     /*!< 心跳上报间隔，单位秒 */
    uint32_t heartbeat_report_count;     /*!< 已成功发起的心跳上报次数 */
    uint32_t heartbeat_failed_count;     /*!< 心跳上报失败次数 */
    uint32_t property_report_count;      /*!< 已成功发起的属性上报次数 */
    uint32_t property_failed_count;      /*!< 属性上报失败次数 */
    uint32_t connect_attempts;          /*!< 连接尝试次数 */
    uint32_t rejected_no_network;       /*!< 因网络不可用拒绝连接次数 */
    uint32_t boot_reconnect_times;      /*!< 本次启动 dev_mqtt 重新上线次数 */
    uint64_t online_since_uptime_ms;    /*!< 当前 dev_mqtt 管理通道上线时间，未在线为 0 */
    uint64_t online_uptime_ms;          /*!< 当前 dev_mqtt 管理通道在线时长，未在线为 0 */
    uint64_t next_heartbeat_uptime_ms;   /*!< 下一次心跳计划时间，本地运行时毫秒 */
    char broker_uri[192];               /*!< Broker URI，不含密码 */
    char client_id[128];                /*!< Client ID */
    char last_reason[64];               /*!< 最近失败原因 */
} gw_mqtt_manager_status_t;

/**
 * @brief 初始化 dev_mqtt 管理模块。
 *
 * 会从 NVS 读取出厂写入的 MQTT 连接信息，并创建 gw_mqtt_client 实例。缺少任一必要
 * 字段时返回错误，并将设备管理 MQTT 状态置为 `not_configured`。
 *
 * @return ESP_OK 表示初始化成功。
 */
esp_err_t gw_mqtt_manager_init(void);

/**
 * @brief 在网络可用时连接 dev_mqtt。
 *
 * 如果网络尚未连接，函数会拒绝连接并返回 ESP_ERR_INVALID_STATE。
 *
 * @return ESP_OK 表示连接请求已提交。
 */
esp_err_t gw_mqtt_manager_connect_if_network_ready(void);

/**
 * @brief 断开 dev_mqtt。
 *
 * @return ESP_OK 表示请求已处理。
 */
esp_err_t gw_mqtt_manager_disconnect(void);

/**
 * @brief 清除 dev_mqtt 本地实例和出厂写入的 MQTT 信息。
 *
 * 该接口只应由恢复出厂或重新产测写号流程调用。运行时 Broker 鉴权失败不得自动
 * 清除这些信息，否则设备会丢失出厂连接参数。
 *
 * @param reason 清理原因，可为空；只用于脱敏日志和诊断。
 * @return ESP_OK 表示已清理。
 */
esp_err_t gw_mqtt_manager_clear_factory_info(const char *reason);

/**
 * @brief 发布设备管理 MQTT 消息。
 *
 * @param topic topic 字符串。
 * @param payload payload 数据，可为空。
 * @param payload_len payload 长度。
 * @return ESP_OK 表示发布请求已接受。
 */
esp_err_t gw_mqtt_manager_publish(const char *topic, const void *payload, size_t payload_len);

/**
 * @brief 发布设备管理 MQTT 消息，并等待对应 PUBLISHED 事件。
 *
 * 该接口用于必须保证“前一个应答已被 Broker 确认后再执行后续动作”的窄场景，
 * 例如 config.update 的 accepted 应答。普通上报仍应使用 gw_mqtt_manager_publish()。
 *
 * @param topic topic 字符串。
 * @param payload payload 数据，可为空。
 * @param payload_len payload 长度。
 * @param timeout_ms 等待 PUBLISHED 事件的超时时间。
 * @return ESP_OK 表示已收到对应消息 ID 的 PUBLISHED 事件。
 */
esp_err_t gw_mqtt_manager_publish_wait(const char *topic,
                                       const void *payload,
                                       size_t payload_len,
                                       uint32_t timeout_ms);

/**
 * @brief 分配设备管理 MQTT payload 缓冲。
 *
 * 该接口严格申请 PSRAM，避免大 payload 挤占内部 RAM。返回的指针必须使用
 * gw_mqtt_payload_free 释放。
 *
 * @param size 申请字节数，0 会按 1 字节处理。
 * @return 成功返回缓冲指针，PSRAM 不足返回 NULL。
 */
void *gw_mqtt_payload_alloc(size_t size);

/**
 * @brief 分配并清零设备管理 MQTT payload 缓冲。
 *
 * @param n 元素数量。
 * @param size 单元素字节数。
 * @return 成功返回缓冲指针，参数溢出或 PSRAM 不足返回 NULL。
 */
void *gw_mqtt_payload_calloc(size_t n, size_t size);

/**
 * @brief 释放设备管理 MQTT payload 缓冲。
 *
 * @param ptr gw_mqtt_payload_alloc/calloc 返回的指针，可为空。
 */
void gw_mqtt_payload_free(void *ptr);

/**
 * @brief 配置设备状态心跳。
 *
 * 配置会写入系统 NVS。interval_sec 必须不小于 GW_HEARTBEAT_MIN_INTERVAL_SEC。
 *
 * @param enabled 是否启用周期心跳。
 * @param interval_sec 心跳间隔，单位秒。
 * @return ESP_OK 表示配置已保存并生效。
 */
esp_err_t gw_mqtt_manager_set_heartbeat_config(bool enabled, uint32_t interval_sec);

/**
 * @brief 获取当前设备状态心跳配置。
 *
 * @param out_enabled 输出是否启用，可为空。
 * @param out_interval_sec 输出心跳间隔，可为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_mqtt_manager_get_heartbeat_config(bool *out_enabled, uint32_t *out_interval_sec);

/**
 * @brief 立即上报一次设备登录。
 *
 * 该接口在 dev_mqtt 订阅完成后调用，用于明确通知平台设备管理通道已上线。
 *
 * @param reason 触发原因，仅用于 payload 诊断字段和本地日志，可为空。
 * @return ESP_OK 表示发布请求已接受。
 */
esp_err_t gw_mqtt_manager_publish_login_report(const char *reason);

/**
 * @brief 立即上报一次设备状态。
 *
 * 该接口用于 MQTT 订阅完成后的上线状态上报，也可由周期心跳轮询触发。
 *
 * @param reason 触发原因，仅用于本地日志，可为空。
 * @return ESP_OK 表示发布请求已接受。
 */
esp_err_t gw_mqtt_manager_publish_status_report(const char *reason);

/**
 * @brief 立即上报一次设备基础属性。
 *
 * 上报内容不包含 MQTT 用户名、密码、脚本业务数据等敏感字段。
 *
 * @param reason 触发原因，仅用于本地日志，可为空。
 * @return ESP_OK 表示发布请求已接受。
 */
esp_err_t gw_mqtt_manager_publish_property_report(const char *reason);

/**
 * @brief 轮询周期心跳。
 *
 * 应由 dev_mgr_task 周期调用。未连接、订阅未就绪或心跳未到期时不会发布。
 *
 * @return ESP_OK 表示轮询完成；发布失败时返回对应错误。
 */
esp_err_t gw_mqtt_manager_poll_heartbeat(void);

/**
 * @brief 查询 dev_mqtt 管理状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_mqtt_manager_get_status(gw_mqtt_manager_status_t *out);

/**
 * @brief 释放 dev_mqtt 下行消息负载。
 *
 * @param payload gw_mqtt_downlink_t 指针，可为空。
 */
void gw_mqtt_downlink_release(void *payload);

#ifdef __cplusplus
}
#endif
