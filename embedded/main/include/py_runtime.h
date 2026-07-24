#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_fs.h"
#include "gw_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 脚本运行环境状态快照。
 *
 * 该结构只暴露脚本运行环境的诊断信息，不包含 MicroPython VM 私有对象、C 内部 handle
 * 或用户脚本数据。字段由 py_runtime 维护，可被设备管理状态和 Python runtime API 读取。
 */
typedef struct {
    bool started;                              /*!< script_task 是否已创建 */
    bool stop_requested;                       /*!< 是否已有停止请求 */
    gw_script_status_t script_status;          /*!< 当前脚本状态 */
    uint32_t start_count;                      /*!< 本次启动累计脚本启动次数 */
    uint32_t exit_count;                       /*!< 本次启动累计正常退出次数 */
    uint32_t exception_count;                  /*!< 本次启动累计异常次数 */
    uint32_t script_stack_free_words;          /*!< script_task 最近一次栈高水位，单位为 word */
    size_t current_heap_size;                  /*!< 当前 MicroPython heap 大小；未运行时为 0 */
    size_t last_heap_size;                     /*!< 最近一次成功分配的 MicroPython heap 大小 */
    esp_err_t last_error;                      /*!< 最近一次脚本运行错误；无错误为 ESP_OK */
    char active_slot[8];                       /*!< 当前配置区，cfg_a 或 cfg_b */
    char active_slot_path[GW_FS_MAX_PATH_LEN]; /*!< 当前配置区 VFS 路径 */
    char script_path[GW_FS_MAX_PATH_LEN];      /*!< main.py VFS 路径 */
    char stop_reason[32];                      /*!< 最近一次停止原因 */
    char last_exception[384];                  /*!< 最近一次脚本异常摘要，已截断 */
} py_runtime_status_t;

/**
 * @brief 启动脚本运行任务。
 *
 * 运行任务会从当前生效配置区执行 main.py。active_slot 无效或脚本不存在时，
 * 不创建任务，脚本状态置为 error，但接口仍返回 ESP_OK，避免脚本缺失阻塞
 * 设备管理主流程。
 *
 * @param active_slot 当前配置区，可为空；有效值为 cfg_a 或 cfg_b。
 * @return ESP_OK 表示成功或已经启动。
 */
esp_err_t py_runtime_start(const char *active_slot);

/**
 * @brief 请求脚本在安全点停止。
 *
 * 该接口只设置停止标志，不直接强杀 VM。真实 MicroPython 接入后，Python 侧通过
 * runtime.should_stop() 观察该标志并主动退出。
 *
 * @param reason 停止原因，允许为空。
 * @return ESP_OK 表示请求已记录；脚本未启动时也返回 ESP_OK。
 */
esp_err_t py_runtime_request_stop(const char *reason);

/**
 * @brief 等待脚本任务停止。
 *
 * @param timeout_ms 最大等待时间。
 * @return ESP_OK 表示脚本已停止；ESP_ERR_TIMEOUT 表示超时。
 */
esp_err_t py_runtime_wait_stopped(uint32_t timeout_ms);

/**
 * @brief 查询脚本停止标志。
 *
 * @return true 表示已有停止请求。
 */
bool py_runtime_should_stop(void);

/**
 * @brief 查询脚本任务是否已启动。
 *
 * @return true 表示任务已创建。
 */
bool py_runtime_is_started(void);

/**
 * @brief 获取脚本运行环境状态快照。
 *
 * @param out 输出状态快照，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t py_runtime_get_status(py_runtime_status_t *out);

#ifdef __cplusplus
}
#endif
