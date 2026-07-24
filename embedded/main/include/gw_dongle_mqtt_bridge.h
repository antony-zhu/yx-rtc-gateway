#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief CH37X/YMZL 与业务 MQTT 数据桥运行状态。 */
typedef struct {
    bool initialized;                 /*!< 固定队列、解析器和 CH37X observer 已就绪 */
    bool running;                     /*!< 数据桥 worker 已启动 */
    uint32_t reset_count;             /*!< USB 连接状态变化导致的协议解析复位次数 */
    uint32_t uplink_frame_count;       /*!< 已解析并提交 MQTT 的上行帧数 */
    uint32_t uplink_queue_drop_count;  /*!< 完整帧队列满导致的上行丢弃数 */
    uint32_t uplink_mqtt_error_count;  /*!< 提交业务 MQTT 失败数 */
    uint32_t downlink_frame_count;     /*!< 已成功写入 dongle 的 MQTT 下行帧数 */
    uint32_t downlink_invalid_count;   /*!< 非法 hex 或 YMZL 帧下行数 */
    uint32_t downlink_retry_count;     /*!< MQTT 下行写 dongle 的临时失败重试数 */
    uint32_t downlink_drop_count;      /*!< 保留字段；合法下行不会因重试次数达到上限而丢弃 */
    uint32_t tx_error_count;           /*!< 所有来源写 dongle 失败数 */
} gw_dongle_mqtt_bridge_status_t;

/**
 * @brief 初始化 C 业务数据桥并注册 CH37X 原始数据 observer。
 *
 * 必须在 `gw_ch37x_bridge_start()` 前调用。初始化后 C 服务独占 CH37X 接收流，
 * 保留的脚本模块不再并发读取同一数据。重复调用安全。
 *
 * @return ESP_OK 表示初始化成功；资源不足或 observer 注册失败时返回错误。
 */
esp_err_t gw_dongle_mqtt_bridge_init(void);

/**
 * @brief 启动 CH37X↔业务 MQTT 双向转发 worker。
 *
 * 调用前必须完成 `gw_business_mqtt_init()` 和 `gw_business_mqtt_start()`；重复启动
 * 安全。上行只发布 CRC 正确的完整 YMZL 帧，并统一转换为大写 ASCII hex。
 *
 * @return ESP_OK 表示 worker 已运行；未初始化或任务创建失败时返回错误。
 */
esp_err_t gw_dongle_mqtt_bridge_start(void);

/**
 * @brief 校验并向 dongle 发送一个完整 YMZL 帧。
 *
 * MQTT 下行和定时任务必须共用本接口。接口会校验帧头、长度、CRC，并在全局范围
 * 保证相邻两帧至少间隔 30 ms，避免多个来源并发写导致串帧。
 *
 * @param frame 完整 YMZL 帧。
 * @param frame_len 帧长度，范围 6～40 字节。
 * @param timeout_ms 获取发送权和等待最小帧间隔的总超时；0 表示不等待。
 * @return ESP_OK 表示已写入；帧非法、dongle 未就绪或等待超时时返回对应错误。
 */
esp_err_t gw_dongle_mqtt_bridge_send_frame(const uint8_t *frame,
                                           size_t frame_len,
                                           uint32_t timeout_ms);

/**
 * @brief 发送读取场景号帧，并等待指定灯具返回真实场景号。
 *
 * 接口在发送前清理旧场景号状态，再从共享上行解析流中匹配源地址和网络 ID。
 * 匹配到的上行帧仍正常发布到 MQTT，不会被本接口独占。同一时刻只允许一个
 * 场景号读取事务；无 ACK 的设置场景号继续使用 `send_frame()`。
 *
 * @param frame 由嵌入式协议层组装的完整 `4E 11` YMZL 读取帧。
 * @param frame_len 完整帧长度。
 * @param expected_source_addr 期望返回状态的灯具源地址；协议允许地址 0。
 * @param expected_network_id 期望返回状态所属的网络/区域 ID。
 * @param out_scene_no 成功时返回灯具当前场景号，不能为 NULL。
 * @param timeout_ms 获取读取事务、发送帧并等待状态的总超时，必须大于 0。
 * @return ESP_OK 表示收到匹配状态；超时、帧非法或 dongle 未就绪返回对应错误。
 */
esp_err_t gw_dongle_mqtt_bridge_read_scene(
    const uint8_t *frame,
    size_t frame_len,
    uint16_t expected_source_addr,
    uint16_t expected_network_id,
    uint8_t *out_scene_no,
    uint32_t timeout_ms);

/**
 * @brief 获取业务数据桥状态快照。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功；服务未初始化时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t gw_dongle_mqtt_bridge_get_status(gw_dongle_mqtt_bridge_status_t *out);

#ifdef __cplusplus
}
#endif
