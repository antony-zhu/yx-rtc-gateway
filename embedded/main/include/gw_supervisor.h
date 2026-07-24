#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 设备管理主任务运行状态。
 */
typedef struct {
    bool started;                    /*!< dev_mgr_task 是否已启动 */
    bool runtime_enabled;            /*!< 是否启用运行期业务状态机 */
    bool legacy_mqtt_enabled;        /*!< 是否允许 supervisor 自动启动旧 dev_mqtt */
    bool boot_ready_seen;            /*!< 是否收到 BOOT_READY */
    bool net_start_requested;        /*!< 是否已请求默认网络启动 */
    bool mqtt_config_missing;        /*!< 出厂 MQTT 连接信息是否缺失或损坏 */
    uint32_t net_start_count;        /*!< dev_mgr 发起联网次数 */
    uint32_t mqtt_config_missing_count; /*!< 检测到 MQTT 连接信息缺失或损坏的次数 */
    uint32_t mqtt_init_count;        /*!< 尝试初始化 dev_mqtt 次数 */
    uint32_t mqtt_connect_count;     /*!< 尝试连接 dev_mqtt 次数 */
    char last_reason[64];            /*!< 最近状态推进结果，已脱敏 */
} gw_supervisor_status_t;

/**
 * @brief 启动设备管理主任务。
 *
 * 该接口只启动事件驱动骨架，不联网、不注册、不连接 MQTT，供自检或特殊流程使用。
 *
 * @return ESP_OK 表示成功或已经启动。
 */
esp_err_t gw_supervisor_start(void);

/**
 * @brief 启动运行期设备管理主任务。
 *
 * 运行期模式会在收到 BOOT_READY 后，由 dev_mgr_task 发起默认 4G 联网；
 * 网络可用后只读取出厂写入的 MQTT 连接信息并连接 dev_mqtt。缺少连接信息时
 * 进入 not_configured 状态，固件不会通过 HTTP 注册接口自动补齐。
 *
 * @return ESP_OK 表示成功或已经启动。
 */
esp_err_t gw_supervisor_start_runtime(void);

/**
 * @brief 启动仅负责网络的运行期设备管理主任务。
 *
 * 收到 BOOT_READY 后会启动并维护默认 4G 网络，但不会初始化或连接旧 dev_mqtt。
 * 旧 MQTT 模块仍保留，可由明确的诊断或兼容流程单独调用。
 *
 * @return ESP_OK 表示成功或已经启动。
 */
esp_err_t gw_supervisor_start_network_runtime(void);

/**
 * @brief 查询设备管理主任务是否已启动。
 *
 * @return true 表示任务已创建。
 */
bool gw_supervisor_is_started(void);

/**
 * @brief 查询设备管理主任务运行状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_supervisor_get_status(gw_supervisor_status_t *out);

#ifdef __cplusplus
}
#endif
