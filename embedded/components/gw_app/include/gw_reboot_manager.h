#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_REBOOT_REASON_CONFIG_UPDATED  "config_updated"
#define GW_REBOOT_REASON_REMOTE_REQUEST  "remote_request"
#define GW_REBOOT_REASON_OTA_UPDATED     "ota_updated"

/**
 * @brief 重启请求参数。
 *
 * 重启统一走该结构，避免配置更新、OTA 和远程重启各自直接调用 esp_restart。
 */
typedef struct {
    char request_id[65];    /*!< 关联的 MQTT request_id；内部触发时可为空 */
    char reason[32];        /*!< 稳定原因码，例如 config_updated */
    uint32_t delay_ms;      /*!< 延迟重启时间；0 表示使用默认值 */
    bool execute;           /*!< true 执行 esp_restart；false 只跑状态流程，供自检使用 */
    bool gateway_protocol;  /*!< 新网关入口不占用或结束旧管理协议的任务 */
} gw_reboot_request_t;

/**
 * @brief 重启管理状态。
 */
typedef struct {
    bool initialized;              /*!< 模块是否已初始化 */
    bool pending;                  /*!< 是否已有重启请求等待执行 */
    bool execute;                  /*!< 当前请求是否会真正重启 */
    char request_id[65];           /*!< 当前请求 ID */
    char reason[32];               /*!< 当前或最近一次重启原因 */
    uint32_t delay_ms;             /*!< 当前请求延迟 */
    uint32_t request_count;        /*!< 已接受请求数 */
    uint32_t rejected_count;       /*!< 因忙或参数错误拒绝数 */
    uint32_t dry_run_count;        /*!< 自检不真正重启的次数 */
} gw_reboot_status_t;

/**
 * @brief 初始化重启管理模块。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_reboot_manager_init(void);

/**
 * @brief 提交重启请求。
 *
 * @param request 重启请求，不能为空。
 * @param out_result 输出统一结果，不能为空。
 * @return ESP_OK 表示请求已接受；其他值表示拒绝。
 */
esp_err_t gw_reboot_manager_request(const gw_reboot_request_t *request, gw_result_t *out_result);

/**
 * @brief 查询重启管理状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_reboot_manager_get_status(gw_reboot_status_t *out);

#ifdef __cplusplus
}
#endif
