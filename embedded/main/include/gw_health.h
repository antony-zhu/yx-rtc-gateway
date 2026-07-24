#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 健康心跳快照。
 */
typedef struct {
    char object[32];          /*!< 健康对象名，例如 app_main、dev_mgr_task */
    char stage[32];           /*!< 当前阶段名，例如 boot、self_check */
    uint64_t uptime_ms;       /*!< 最近心跳时间 */
    uint32_t count;           /*!< 累计心跳次数 */
} gw_health_heartbeat_t;

/**
 * @brief 初始化健康监控基础状态。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_health_init(void);

/**
 * @brief 上报一个健康心跳。
 *
 * P1 阶段只记录最近一次心跳；P3/P12 会扩展为任务健康监控。
 *
 * @param object 健康对象名，不能为空。
 * @param stage 当前阶段名，可为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_health_report_heartbeat(const char *object, const char *stage);

/**
 * @brief 获取最近一次健康心跳快照。
 *
 * @param out 输出快照，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_health_get_last_heartbeat(gw_health_heartbeat_t *out);

#ifdef __cplusplus
}
#endif
