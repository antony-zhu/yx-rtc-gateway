#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SCRIPT_STOP_REASON_CONFIG_UPDATING  "config_updating"
#define GW_SCRIPT_STOP_REASON_OTA_UPDATING     "ota_updating"
#define GW_SCRIPT_STOP_REASON_REBOOTING        "rebooting"
#define GW_SCRIPT_STOP_REASON_SELF_CHECK       "self_check"

/**
 * @brief 脚本停止协调状态。
 */
typedef struct {
    bool initialized;              /*!< 模块是否已初始化 */
    uint32_t request_count;        /*!< 停止请求次数 */
    uint32_t stopped_count;        /*!< 正常停止次数 */
    uint32_t timeout_count;        /*!< 等待超时次数 */
    uint32_t restart_count;        /*!< 重新启动脚本次数 */
    char last_reason[32];          /*!< 最近一次停止原因 */
    char exception_action[32];      /*!< 脚本异常处理策略 */
    bool last_stopped;             /*!< 最近一次是否在超时前停止 */
} gw_script_manager_status_t;

/**
 * @brief 初始化脚本协调模块。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_script_manager_init(void);

/**
 * @brief 请求脚本停止并等待。
 *
 * 该接口由设备管理侧 worker 调用，最多等待 timeout_ms。超时不强杀脚本，调用方可按业务策略继续。
 *
 * @param reason 停止原因。
 * @param timeout_ms 最大等待时间，传 0 使用系统默认值。
 * @param out_stopped 输出是否已停止，可为空。
 * @return ESP_OK 表示脚本已停止或原本未运行；ESP_ERR_TIMEOUT 表示超时。
 */
esp_err_t gw_script_manager_request_stop(const char *reason, uint32_t timeout_ms, bool *out_stopped);

/**
 * @brief 按当前配置重新启动脚本。
 *
 * @param active_slot 当前配置区，可为空。
 * @return ESP_OK 表示成功或已经运行。
 */
esp_err_t gw_script_manager_restart(const char *active_slot);

/**
 * @brief 设置脚本异常处理策略。
 *
 * 当前支持 restart_immediately、restart_by_watchdog 和 ignore。策略会写入
 * 系统 NVS，下一次脚本异常处理时生效。
 *
 * @param action 异常处理策略字符串，不能为空。
 * @return ESP_OK 表示保存成功；ESP_ERR_INVALID_ARG 表示取值非法。
 */
esp_err_t gw_script_manager_set_exception_action(const char *action);

/**
 * @brief 获取脚本异常处理策略。
 *
 * @param out 输出缓冲区，不能为空。
 * @param out_size 输出缓冲区大小。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_script_manager_get_exception_action(char *out, size_t out_size);

/**
 * @brief 查询脚本协调状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_script_manager_get_status(gw_script_manager_status_t *out);

#ifdef __cplusplus
}
#endif
