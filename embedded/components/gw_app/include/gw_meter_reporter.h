#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_meter_reporter.h
 * @brief DTSU666 周期 HTTP 上报应用。
 *
 * 电表协议和 RS485 访问由 gw_dtsu666 驱动层负责，本模块只负责调度采集和发送。
 */

/**
 * @brief 启动电表周期上报任务。
 *
 * 启动不会等待网络、电表或 HTTP 服务；任务按周期采集，失败时下一周期继续重试。
 * 重复调用返回 ESP_OK。
 */
esp_err_t gw_meter_reporter_start(void);

/**
 * @brief 查询电表上报任务是否已创建。
 */
bool gw_meter_reporter_is_started(void);

#ifdef __cplusplus
}
#endif
