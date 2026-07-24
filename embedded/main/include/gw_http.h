#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP 方法。
 */
typedef enum {
    GW_HTTP_METHOD_GET = 0,   /*!< HTTP GET */
    GW_HTTP_METHOD_POST,      /*!< HTTP POST */
} gw_http_method_t;

/**
 * @brief HTTP 请求参数。
 */
typedef struct {
    gw_http_method_t method;                  /*!< 请求方法 */
    const char *url;                          /*!< 请求 URL，必须为 http 或 https */
    const char *content_type;                 /*!< POST Content-Type，可为空 */
    const void *body;                         /*!< POST 请求体，可为空 */
    size_t body_len;                          /*!< 请求体长度 */
    char *response_buf;                       /*!< 响应缓冲区，可为空 */
    size_t response_buf_size;                 /*!< 响应缓冲区大小 */
    uint32_t connect_timeout_sec;             /*!< 连接超时，0 表示默认值 */
    uint32_t response_interval_timeout_sec;   /*!< 应答间隔超时，0 表示默认值 */
    uint32_t total_timeout_sec;               /*!< 整次请求总超时，0 表示不额外限制 */
    bool allow_untrusted_https;               /*!< UTC 未可信时仍允许 HTTPS；不会关闭证书链校验 */
} gw_http_request_t;

/**
 * @brief HTTP 响应结果。
 */
typedef struct {
    gw_result_t result;       /*!< 统一结果 */
    int http_status;          /*!< HTTP 状态码，未收到响应时为 0 */
    size_t response_len;      /*!< 已写入响应缓冲区的字节数 */
    bool response_truncated;  /*!< 响应是否因缓冲区不足被截断 */
} gw_http_response_t;

/**
 * @brief 执行一次 HTTP/HTTPS 请求。
 *
 * 该接口是同步阻塞接口，只能由 worker 或自检流程调用，不得从 MQTT、
 * HTTP、timer 回调或脚本 VM 上下文直接调用。
 *
 * @param req 请求参数，不能为空。
 * @param out 输出响应，不能为空。
 * @return ESP_OK 表示请求成功且收到 2xx 响应。
 */
esp_err_t gw_http_perform(const gw_http_request_t *req, gw_http_response_t *out);

/**
 * @brief 脱敏 URL。
 *
 * 输出仅保留 scheme、host、path，query 统一替换为 ?***。
 *
 * @param url 原始 URL。
 * @param out 输出缓冲区。
 * @param out_size 输出缓冲区大小。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_http_redact_url(const char *url, char *out, size_t out_size);

/**
 * @brief 判断 URL scheme 是否被允许。
 *
 * @param url 原始 URL。
 * @return true 表示为 http:// 或 https://。
 */
bool gw_http_url_scheme_allowed(const char *url);

#ifdef __cplusplus
}
#endif
