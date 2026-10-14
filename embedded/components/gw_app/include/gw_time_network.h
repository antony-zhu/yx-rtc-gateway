#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 网络校时工作线程状态。
 *
 * SNTP 由 4G 网络就绪事件启动，并由协议栈按配置周期继续复校。回调只唤醒
 * 独立任务，NVS、RTC 和调度器更新不会阻塞 lwIP 线程。
 */
typedef struct {
    bool worker_started;          /*!< 网络校时工作线程是否已经创建 */
    bool sntp_initialized;        /*!< ESP-NETIF SNTP 服务是否已经初始化 */
    uint32_t request_count;       /*!< 收到网络就绪校时请求的累计次数 */
    uint32_t sync_success_count;  /*!< 已接受的 SNTP 授时累计次数 */
    uint64_t last_sync_utc_ms;    /*!< 最近一次成功授时的 UTC 毫秒 */
    esp_err_t last_error;         /*!< 最近一次初始化、启动或授时错误 */
} gw_time_network_status_t;

/**
 * @brief 请求立即进行一次网络校时。
 *
 * 首次调用会创建工作线程并初始化 SNTP；后续调用用于网络重连后立即重启
 * SNTP 查询。函数不等待 DNS 或 NTP 响应，可在设备管理事件循环中调用。
 *
 * @return ESP_OK 表示请求已提交；ESP_ERR_NO_MEM 表示任务创建失败。
 */
esp_err_t gw_time_network_sync_request(void);

/**
 * @brief 获取网络校时状态快照。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功；ESP_ERR_INVALID_ARG 表示参数为空。
 */
esp_err_t gw_time_network_get_status(gw_time_network_status_t *out);

#ifdef __cplusplus
}
#endif
