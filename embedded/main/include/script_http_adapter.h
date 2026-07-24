#pragma once

/**
 * @file script_http_adapter.h
 * @brief 脚本 HTTP 异步适配层接口。
 *
 * 该模块只服务用户脚本运行期的 HTTP/HTTPS 小请求，不参与设备注册、配置下载、
 * OTA 下载或系统日志上传。脚本侧调用 `http.request()` / `http.get()` /
 * `http.post()` 时只提交请求并立即返回 request_id；实际网络请求由独立 worker
 * 执行，完成后通过 `http.poll()` 取回结果。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_http.h"
#include "gw_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 脚本侧 HTTP 请求参数。
 *
 * 该结构只描述用户脚本业务 HTTP。适配层会在提交时把 URL、Content-Type、
 * 请求体和响应缓冲区复制或分配到 PSRAM，避免脚本对象生命周期和内部 RAM 占用
 * 影响底层 HTTP worker。
 */
typedef struct {
    gw_http_method_t method;                  /*!< 请求方法，仅支持 GET 和 POST */
    const char *url;                          /*!< http:// 或 https:// URL，不能为空 */
    const char *content_type;                 /*!< POST Content-Type，可为空 */
    const void *body;                         /*!< POST 请求体，可为空 */
    size_t body_len;                          /*!< POST 请求体长度，GET 时必须为 0 */
    size_t max_response_bytes;                /*!< 返回给 Python 的最大响应体字节数，0 表示默认值 */
    uint32_t connect_timeout_sec;             /*!< 连接超时秒数，0 表示默认值 */
    uint32_t response_interval_timeout_sec;   /*!< 两次响应数据间隔超时秒数，0 表示默认值 */
} script_http_request_t;

/**
 * @brief 脚本侧 HTTP 响应。
 *
 * `body` 由适配层使用 PSRAM 分配。调用方从 `script_http_adapter_poll()` 取到事件后，
 * 处理完成必须调用 `script_http_adapter_release_event()`；如果只持有该响应结构，
 * 则调用 `script_http_adapter_release_response()`。
 */
typedef struct {
    gw_result_t result;             /*!< 统一结果，包含 HTTP 失败摘要 */
    int http_status;                /*!< HTTP 状态码，未收到响应时为 0 */
    size_t body_len;                /*!< body 有效长度 */
    bool body_truncated;            /*!< 响应体是否被 max_response_bytes 截断 */
    uint8_t *body;                  /*!< 响应体，可能为空；内存位于 PSRAM */
    uint64_t timestamp_ms;          /*!< 可信 UTC 毫秒；不可信时为 0 */
} script_http_response_t;

/**
 * @brief 脚本 HTTP 完成事件。
 *
 * 每个事件对应一次已提交的 request_id。HTTP 状态码非 2xx、DNS/连接失败、
 * 读取失败都会以完成事件形式返回给脚本，避免 worker 内部直接抛 Python 异常。
 */
typedef struct {
    uint32_t request_id;            /*!< 适配层分配的本地请求 ID */
    esp_err_t esp_err;              /*!< gw_http_perform() 的返回值 */
    script_http_response_t response;/*!< HTTP 响应和统一错误信息 */
} script_http_event_t;

/**
 * @brief 脚本 HTTP worker 状态快照。
 */
typedef struct {
    bool initialized;               /*!< adapter 是否已初始化 */
    bool worker_running;            /*!< worker 任务是否存在 */
    bool active;                    /*!< 当前是否有请求执行中 */
    bool closing;                   /*!< 是否正在执行脚本退出清理 */
    uint32_t generation;            /*!< 生命周期代数，用于丢弃旧脚本遗留事件 */
    uint32_t active_request_id;     /*!< 当前执行中的 request_id，无请求时为 0 */
    uint32_t last_request_id;       /*!< 最近一次受理的 request_id */
    uint32_t request_count;         /*!< 已受理请求数 */
    uint32_t completed_count;       /*!< 已执行完成的请求数，不含提交前被拒绝的请求 */
    uint32_t success_count;         /*!< HTTP 2xx 请求数 */
    uint32_t failure_count;         /*!< 失败请求数，含非 2xx */
    uint32_t rejected_count;        /*!< 参数、状态或资源不足导致拒绝的次数 */
    uint32_t dropped_count;         /*!< 因事件队列满或脚本代数失效丢弃的完成事件数 */
    uint32_t polled_count;          /*!< Python 已 poll 的完成事件数 */
    uint32_t queue_pending;         /*!< 请求队列中等待执行的请求数 */
    uint32_t event_pending;         /*!< 完成事件队列中等待 poll 的事件数 */
    int last_http_status;           /*!< 最近一次 HTTP 状态码 */
    esp_err_t last_esp_err;         /*!< 最近一次 ESP 错误 */
    char last_reason[64];           /*!< 最近一次失败或诊断原因 */
} script_http_status_t;

/**
 * @brief 初始化脚本 HTTP adapter。
 *
 * 该函数可重复调用。首次调用会创建请求队列、完成事件队列和 HTTP worker。
 *
 * @return ESP_OK 表示初始化成功。
 */
esp_err_t script_http_adapter_init(void);

/**
 * @brief 提交一次异步脚本 HTTP 请求。
 *
 * 该函数只完成参数校验、PSRAM 拷贝和入队，不等待网络请求完成。返回 ESP_OK 后，
 * 调用方可通过 `script_http_adapter_poll()` 根据返回的 request_id 获取完成事件。
 *
 * @param req 请求参数，不能为空。
 * @param out_request_id 输出本地 request_id，不能为空。
 * @return ESP_OK 表示请求已受理；其他值表示本次请求未入队。
 */
esp_err_t script_http_adapter_start(const script_http_request_t *req, uint32_t *out_request_id);

/**
 * @brief 获取一个 HTTP 完成事件。
 *
 * @param timeout_ms 最长等待时间，0 表示立即返回。
 * @param out_event 输出完成事件，不能为空；成功后需释放事件。
 * @return ESP_OK 表示取到事件；ESP_ERR_TIMEOUT 表示当前没有完成事件。
 */
esp_err_t script_http_adapter_poll(uint32_t timeout_ms, script_http_event_t *out_event);

/**
 * @brief 释放脚本 HTTP 完成事件内持有的 PSRAM 响应体。
 *
 * @param event 完成事件，可为空。
 */
void script_http_adapter_release_event(script_http_event_t *event);

/**
 * @brief 释放脚本 HTTP 响应体。
 *
 * @param response 响应结构，可为空。
 */
void script_http_adapter_release_response(script_http_response_t *response);

/**
 * @brief 查询脚本 HTTP adapter 状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t script_http_adapter_get_status(script_http_status_t *out);

/**
 * @brief 脚本退出时清理脚本 HTTP 临时资源。
 *
 * 该函数会递增 generation，清空等待执行的请求和等待 poll 的完成事件。正在执行的
 * 请求不会被强行终止，但完成后会因为 generation 不匹配而丢弃结果。
 */
void script_http_adapter_cleanup_on_script_exit(void);

#ifdef __cplusplus
}
#endif
