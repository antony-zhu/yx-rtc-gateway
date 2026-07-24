#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动日志任务。
 *
 * P3 阶段是 log_task 桩，只做健康心跳和栈高水位记录；P4 再接入
 * logfs 持久化队列。
 *
 * @return ESP_OK 表示成功或已经启动。
 */
esp_err_t gw_log_task_start(void);

/**
 * @brief 查询日志任务是否已启动。
 *
 * @return true 表示任务已创建。
 */
bool gw_log_task_is_started(void);

#ifdef __cplusplus
}
#endif
