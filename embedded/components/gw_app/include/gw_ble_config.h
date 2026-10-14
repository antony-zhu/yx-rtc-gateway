#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BLE 配置服务运行状态。
 */
typedef struct {
    bool initialized;          /*!< NimBLE 与 GATT 服务是否已初始化 */
    bool advertising;          /*!< 当前是否正在广播 */
    bool connected;            /*!< 是否已有手机连接 */
    bool encrypted;            /*!< 当前连接是否已建立加密 */
    bool response_subscribed;  /*!< 手机是否订阅命令回复 */
    bool event_subscribed;     /*!< 手机是否订阅异步事件 */
    uint16_t conn_handle;      /*!< NimBLE 连接句柄；未连接时为 UINT16_MAX */
    uint16_t mtu;              /*!< 当前 ATT MTU；未连接时为默认值 */
} gw_ble_config_status_t;

/**
 * @brief 初始化并启动 BLE 定时配置服务。
 *
 * 调用前必须完成 `gw_storage_init()`、`gw_time_init()` 和
 * `gw_scheduler_init()`。本服务只作为 BLE Peripheral/GATT Server，最多允许
 * 一台手机连接；重复调用不会重复创建 NimBLE host。
 *
 * @return ESP_OK 表示服务已启动，其他值表示 NimBLE 或 GATT 初始化失败。
 */
esp_err_t gw_ble_config_init(void);

/**
 * @brief 获取 BLE 配置服务状态快照。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_ble_config_get_status(gw_ble_config_status_t *out);

/**
 * @brief 通知已连接 APP 某个定时动作的执行结果。
 *
 * 未连接、连接未加密或 APP 未订阅事件时会静默跳过；本函数不缓存历史事件。
 * 可从设备管理任务调用，不得在中断上下文调用。
 *
 * @param task_id 定时任务 ID。
 * @param occurrence_id 本次触发的唯一编号。
 * @param scheduled_utc_ms 计划触发 UTC 毫秒；不可信时为 0。
 * @param action_result 实际动作执行结果。
 */
void gw_ble_config_notify_schedule_fired(uint32_t task_id,
                                         uint64_t occurrence_id,
                                         uint64_t scheduled_utc_ms,
                                         esp_err_t action_result);

#ifdef __cplusplus
}
#endif
