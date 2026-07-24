#include "gw_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"

#include "gw_defaults.h"
#include "gw_time.h"

static const char *TAG = "gw_http";

bool gw_http_url_scheme_allowed(const char *url)
{
    return url != NULL &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

/**
 * @brief 判断 URL 是否为 HTTPS。
 *
 * @param url 待检查的 URL，可以为 NULL。
 * @return true 表示 URL 使用 https:// 前缀；false 表示不是 HTTPS。
 */
static bool url_is_https(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

esp_err_t gw_http_redact_url(const char *url, char *out, size_t out_size)
{
    if (url == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *query = strchr(url, '?');
    size_t keep_len = query != NULL ? (size_t)(query - url) : strlen(url);
    const char *suffix = query != NULL ? "?***" : "";
    size_t suffix_len = strlen(suffix);

    if (keep_len + suffix_len + 1U > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, url, keep_len);
    memcpy(out + keep_len, suffix, suffix_len + 1U);
    return ESP_OK;
}

static esp_http_client_method_t to_esp_method(gw_http_method_t method)
{
    switch (method) {
    case GW_HTTP_METHOD_POST:
        return HTTP_METHOD_POST;
    case GW_HTTP_METHOD_GET:
    default:
        return HTTP_METHOD_GET;
    }
}

static esp_http_client_addr_type_t select_addr_type(const char *url)
{
    if (url == NULL) {
        return HTTP_ADDR_TYPE_INET;
    }

    const char *scheme = strstr(url, "://");
    const char *host = scheme != NULL ? scheme + 3 : url;
    if (host[0] == '[') {
        return HTTP_ADDR_TYPE_INET6;
    }

    return HTTP_ADDR_TYPE_INET;
}

/** @brief 按绝对截止时间收紧当前阶段超时；返回 0 表示总超时已经到达。 */
static int bounded_timeout_ms(uint32_t requested_sec, int64_t deadline_us)
{
    uint64_t requested_ms = (uint64_t)requested_sec * 1000ULL;
    if (requested_ms > INT32_MAX) {
        requested_ms = INT32_MAX;
    }

    if (deadline_us > 0) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return 0;
        }
        uint64_t remaining_ms = ((uint64_t)remaining_us + 999ULL) / 1000ULL;
        if (remaining_ms < requested_ms) {
            requested_ms = remaining_ms;
        }
    }
    return requested_ms > 0U ? (int)requested_ms : 1;
}

static void fill_error(gw_http_response_t *out,
                       int32_t code,
                       const char *reason,
                       int http_status,
                       esp_err_t esp_err,
                       const char *detail)
{
    out->result = gw_result_error(code, reason, detail);
    out->result.http_status = http_status;
    out->result.esp_err = esp_err;
    out->http_status = http_status;
}

/**
 * @brief 比较 esp-tls 返回的错误码和 mbedTLS 原始错误码。
 *
 * ESP-IDF 不同路径保存的 TLS code 可能是 mbedTLS 负数错误码，也可能是其相反数。
 * 统一在这里比较，避免证书错误、内存分配错误等原因被误判为普通 HTTP 错误。
 *
 * @param tls_code esp_http_client_get_and_clear_last_tls_error() 读取到的 TLS code。
 * @param mbedtls_err mbedTLS 宏定义的负数错误码。
 * @return true 表示二者匹配。
 */
static bool tls_code_matches(int tls_code, int mbedtls_err)
{
    return tls_code == mbedtls_err || tls_code == -mbedtls_err;
}

/**
 * @brief 根据 esp_http_client 保存的 TLS 细节填充更明确的失败原因。
 *
 * esp_http_client_open/fetch/read 对外经常只返回通用连接失败。这里读取并清空
 * 底层 TLS 错误和证书校验 flags，把平台可判断的原因稳定映射为 reason_code。
 */
static esp_err_t fill_tls_or_http_error(gw_http_response_t *out,
                                        esp_http_client_handle_t client,
                                        esp_err_t fallback_err,
                                        int http_status,
                                        const char *fallback_detail)
{
    int tls_code = 0;
    int tls_flags = 0;
    esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(client,
                                                                    &tls_code,
                                                                    &tls_flags);
    esp_err_t result_err = tls_err != ESP_OK ? tls_err : fallback_err;
    const char *reason = GW_REASON_HTTP_ERROR;
    const char *detail = fallback_detail;

    if (tls_flags != 0 || tls_code_matches(tls_code, MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)) {
        reason = GW_REASON_TLS_VERIFY_FAILED;
        detail = "tls cert verify failed";
    } else if (tls_code_matches(tls_code, MBEDTLS_ERR_SSL_ALLOC_FAILED)) {
        reason = GW_REASON_TLS_ERROR;
        detail = "tls memory alloc failed";
    } else if (tls_err != ESP_OK) {
        reason = GW_REASON_TLS_ERROR;
        detail = "tls transport failed";
    }

    if (tls_err != ESP_OK || tls_code != 0 || tls_flags != 0) {
        ESP_LOGW(TAG,
                 "https failed: tls_err=%s(%d) tls_code=%d tls_flags=0x%x fallback=%s(%d)",
                 esp_err_to_name(tls_err),
                 (int)tls_err,
                 tls_code,
                 tls_flags,
                 esp_err_to_name(fallback_err),
                 (int)fallback_err);
    }

    fill_error(out,
               GW_RESULT_INTERNAL_ERROR,
               reason,
               http_status,
               result_err,
               detail);
    return result_err;
}

esp_err_t gw_http_perform(const gw_http_request_t *req, gw_http_response_t *out)
{
    if (req == NULL || out == NULL || req->url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->result = gw_result_ok();

    if (!gw_http_url_scheme_allowed(req->url)) {
        fill_error(out,
                   GW_RESULT_INVALID_FIELD,
                   GW_REASON_INVALID_FIELD,
                   0,
                   ESP_ERR_INVALID_ARG,
                   "unsupported url scheme");
        return ESP_ERR_INVALID_ARG;
    }

    bool is_https = url_is_https(req->url);
    if (is_https) {
        gw_time_point_t now = gw_time_now();
        if (!now.utc_trusted && !req->allow_untrusted_https) {
            fill_error(out,
                       GW_RESULT_INTERNAL_ERROR,
                       GW_REASON_TIME_NOT_TRUSTED,
                       0,
                       ESP_ERR_INVALID_STATE,
                       "https requires trusted utc");
            return ESP_ERR_INVALID_STATE;
        }
        if (!now.utc_trusted && req->allow_untrusted_https) {
            /*
             * MQTT 注册是网络建立后的引导请求，设备此时可能尚未完成 NTP 同步。
             * 这里只跳过本地 UTC 前置检查，crt_bundle 仍会执行 TLS 证书链校验。
             */
            ESP_LOGW(TAG, "https starts while utc is not trusted");
        }
    }

    char redacted[GW_HTTP_MAX_URL_LEN];
    if (gw_http_redact_url(req->url, redacted, sizeof(redacted)) != ESP_OK) {
        strlcpy(redacted, "<url-too-long>", sizeof(redacted));
    }

    uint32_t connect_timeout_sec = req->connect_timeout_sec != 0 ?
                                   req->connect_timeout_sec :
                                   GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC;
    uint32_t interval_timeout_sec = req->response_interval_timeout_sec != 0 ?
                                     req->response_interval_timeout_sec :
                                     GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC;
    int64_t deadline_us = req->total_timeout_sec != 0U ?
                          esp_timer_get_time() +
                              ((int64_t)req->total_timeout_sec * 1000000LL) :
                          0;
    int connect_timeout_ms = bounded_timeout_ms(connect_timeout_sec, deadline_us);
    if (connect_timeout_ms == 0) {
        fill_error(out,
                   GW_RESULT_INTERNAL_ERROR,
                   GW_REASON_TIMEOUT,
                   0,
                   ESP_ERR_TIMEOUT,
                   "http overall timeout");
        return ESP_ERR_TIMEOUT;
    }

    esp_http_client_config_t config = {
        .url = req->url,
        .method = to_esp_method(req->method),
        .timeout_ms = connect_timeout_ms,
        /*
         * 当前 4G ECM 会同时获得 IPv6 地址，但部分公网 HTTP/HTTPS 站点在该链路的
         * IPv6 连接上会被对端 reset。系统 HTTP 默认固定 IPv4，避免配置下载随机失败。
         * 如果平台显式下发 IPv6 字面量 URL，则按该 URL 走 IPv6。
         */
        .addr_type = select_addr_type(req->url),
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        fill_error(out,
                   GW_RESULT_INTERNAL_ERROR,
                   GW_REASON_HTTP_ERROR,
                   0,
                   ESP_FAIL,
                   "http client init failed");
        return ESP_FAIL;
    }

    if (req->content_type != NULL && req->content_type[0] != '\0') {
        (void)esp_http_client_set_header(client, "Content-Type", req->content_type);
    }

    ESP_LOGI(TAG, "http request: %s", redacted);

    int write_len = req->method == GW_HTTP_METHOD_POST ? (int)req->body_len : 0;
    esp_err_t err = esp_http_client_open(client, write_len);
    if (err != ESP_OK) {
        esp_err_t result_err = is_https ?
                               fill_tls_or_http_error(out, client, err, 0, "http open failed") :
                               err;
        if (!is_https) {
            fill_error(out,
                       GW_RESULT_INTERNAL_ERROR,
                       GW_REASON_HTTP_ERROR,
                       0,
                       err,
                       "http open failed");
        }
        esp_http_client_cleanup(client);
        return result_err;
    }

    if (write_len > 0 && req->body != NULL) {
        int written = esp_http_client_write(client, (const char *)req->body, write_len);
        if (written != write_len) {
            fill_error(out,
                       GW_RESULT_INTERNAL_ERROR,
                       GW_REASON_HTTP_ERROR,
                       0,
                       ESP_FAIL,
                       "http write failed");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    int response_timeout_ms = bounded_timeout_ms(interval_timeout_sec, deadline_us);
    if (response_timeout_ms == 0) {
        fill_error(out,
                   GW_RESULT_INTERNAL_ERROR,
                   GW_REASON_TIMEOUT,
                   0,
                   ESP_ERR_TIMEOUT,
                   "http overall timeout");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_TIMEOUT;
    }
    (void)esp_http_client_set_timeout_ms(client, response_timeout_ms);

    int64_t header_len = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    out->http_status = http_status;
    if (header_len < 0) {
        esp_err_t result_err = is_https ?
                               fill_tls_or_http_error(out,
                                                      client,
                                                      ESP_FAIL,
                                                      http_status,
                                                      "http fetch headers failed") :
                               ESP_FAIL;
        if (!is_https) {
            fill_error(out,
                       GW_RESULT_INTERNAL_ERROR,
                       GW_REASON_HTTP_ERROR,
                       http_status,
                       ESP_FAIL,
                       "http fetch headers failed");
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result_err;
    }

    char read_buf[512];
    for (;;) {
        response_timeout_ms = bounded_timeout_ms(interval_timeout_sec, deadline_us);
        if (response_timeout_ms == 0) {
            fill_error(out,
                       GW_RESULT_INTERNAL_ERROR,
                       GW_REASON_TIMEOUT,
                       http_status,
                       ESP_ERR_TIMEOUT,
                       "http overall timeout");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_TIMEOUT;
        }
        (void)esp_http_client_set_timeout_ms(client, response_timeout_ms);

        int read_len = esp_http_client_read(client, read_buf, sizeof(read_buf));
        if (read_len < 0) {
            esp_err_t result_err = is_https ?
                                   fill_tls_or_http_error(out,
                                                          client,
                                                          ESP_FAIL,
                                                          http_status,
                                                          "http read failed") :
                                   ESP_FAIL;
            if (!is_https) {
                fill_error(out,
                           GW_RESULT_INTERNAL_ERROR,
                           GW_REASON_HTTP_ERROR,
                           http_status,
                           ESP_FAIL,
                           "http read failed");
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return result_err;
        }
        if (read_len == 0) {
            break;
        }

        if (req->response_buf != NULL && req->response_buf_size > 0) {
            size_t room = req->response_buf_size - out->response_len;
            size_t copy_len = (size_t)read_len;
            if (copy_len >= room) {
                copy_len = room > 0 ? room - 1U : 0;
                out->response_truncated = true;
            }
            if (copy_len > 0) {
                memcpy(req->response_buf + out->response_len, read_buf, copy_len);
                out->response_len += copy_len;
                req->response_buf[out->response_len] = '\0';
            }
            if (out->response_truncated) {
                /* 响应上限已经确定超出，立即关闭连接，避免慢速响应长期占住 worker。 */
                break;
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (http_status < 200 || http_status >= 300) {
        fill_error(out,
                   GW_RESULT_INTERNAL_ERROR,
                   GW_REASON_HTTP_ERROR,
                   http_status,
                   ESP_OK,
                   "http status is not 2xx");
        return ESP_FAIL;
    }

    out->result = gw_result_ok();
    out->result.http_status = http_status;
    return ESP_OK;
}
