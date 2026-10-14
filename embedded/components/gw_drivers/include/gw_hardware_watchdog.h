#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_hardware_watchdog.h
 * @brief 外部硬件看门狗喂狗任务接口。
 *
 * 外部硬件看门狗独立于 ESP-IDF task watchdog。固件只负责按固定周期输出喂狗
 * 脉冲；如果系统严重卡死，喂狗任务无法运行时由外部硬件复位设备。
 */

/**
 * @brief 外部硬件看门狗状态快照。
 */
typedef struct {
    bool enabled;              /*!< 编译期默认是否启用外部看门狗 */
    bool running;              /*!< 喂狗任务是否已运行 */
    int gpio;                  /*!< 喂狗 GPIO */
    uint32_t pulse_high_ms;    /*!< 单次高电平脉冲宽度 */
    uint32_t feed_interval_ms; /*!< 两次喂狗之间的间隔 */
} gw_hardware_watchdog_info_t;

/**
 * @brief 启动外部硬件看门狗喂狗任务。
 *
 * 可重复调用；已启动时直接返回 ESP_OK。当前参数来自编译期默认值，后续接入系统
 * 配置后可扩展为运行期配置。
 *
 * @return ESP_OK 表示启动成功或已启动；其他值表示 GPIO、时序或资源错误。
 */
esp_err_t gw_hardware_watchdog_start(void);

/**
 * @brief 获取外部硬件看门狗状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_hardware_watchdog_get_info(gw_hardware_watchdog_info_t *out);

#ifdef __cplusplus
}
#endif
