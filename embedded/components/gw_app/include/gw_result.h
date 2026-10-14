#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_REASON_OK                         "ok"
#define GW_REASON_INVALID_FIELD              "invalid_field"
#define GW_REASON_ACCEPTED                   "accepted"
#define GW_REASON_METHOD_NOT_FOUND           "method_not_found"
#define GW_REASON_VERSION_NOT_INCREASED      "version_not_increased"
#define GW_REASON_PAYLOAD_TOO_LARGE          "payload_too_large"
#define GW_REASON_DEVICE_BUSY                "device_busy"
#define GW_REASON_DUPLICATE_REQUEST_ID       "duplicate_request_id"
#define GW_REASON_STORAGE_FULL               "storage_full"
#define GW_REASON_INTERNAL_ERROR             "internal_error"
#define GW_REASON_QUEUE_FULL                 "queue_full"
#define GW_REASON_NOT_INITIALIZED            "not_initialized"
#define GW_REASON_UNSUPPORTED                "unsupported"
#define GW_REASON_NETWORK_UNAVAILABLE        "network_unavailable"
#define GW_REASON_HTTP_ERROR                 "http_error"
#define GW_REASON_TIME_NOT_TRUSTED           "time_not_trusted"
#define GW_REASON_TLS_ERROR                  "tls_error"
#define GW_REASON_TLS_VERIFY_FAILED          "tls_verify_failed"
#define GW_REASON_TIMEOUT                    "timeout"
#define GW_REASON_PARSE_FAILED               "parse_failed"
#define GW_REASON_AUTH_FAILED                "auth_failed"
#define GW_REASON_MQTT_NOT_CONFIGURED        "mqtt_not_configured"
#define GW_REASON_INVALID_MANIFEST           "invalid_manifest"
#define GW_REASON_SHA256_MISMATCH            "sha256_mismatch"
#define GW_REASON_SIZE_MISMATCH              "size_mismatch"
#define GW_REASON_MIN_PRODUCT_VERSION        "min_product_version_not_met"

/**
 * @brief 模块内部统一结果码。
 *
 * MQTT 协议应答的业务 code 由协议模块根据该结果码再映射生成。
 */
typedef enum {
    GW_RESULT_OK = 0,
    GW_RESULT_INVALID_FIELD = 400,
    GW_RESULT_METHOD_NOT_FOUND = 404,
    GW_RESULT_VERSION_NOT_INCREASED = 409,
    GW_RESULT_PAYLOAD_TOO_LARGE = 413,
    GW_RESULT_DEVICE_BUSY = 423,
    GW_RESULT_DUPLICATE_REQUEST_ID = 425,
    GW_RESULT_INTERNAL_ERROR = 500,
    GW_RESULT_UNSUPPORTED = 501,
    GW_RESULT_STORAGE_FULL = 507,
} gw_result_code_t;

/**
 * @brief 跨模块统一结果结构。
 *
 * 该结构只保存稳定错误信息和必要诊断摘要，不允许携带密码、token、
 * NVS 加密材料或完整敏感 URL。
 */
typedef struct {
    int32_t code;                 /*!< 模块内部处理结果，0 表示成功 */
    const char *reason_code;      /*!< 稳定英文短码，例如 ok、invalid_field */
    int32_t http_status;          /*!< HTTP 状态码；未收到 HTTP 响应时为 0 */
    int32_t server_code;          /*!< 服务端 JSON code；没有应用层 code 时为 0 */
    int32_t esp_err;              /*!< ESP-IDF 错误码；无 ESP 错误时为 0 */
    const char *detail;           /*!< 简短诊断摘要，必须脱敏 */
} gw_result_t;

/**
 * @brief 构造成功结果。
 *
 * @return 成功的统一结果结构。
 */
static inline gw_result_t gw_result_ok(void)
{
    return (gw_result_t) {
        .code = GW_RESULT_OK,
        .reason_code = GW_REASON_OK,
        .http_status = 0,
        .server_code = 0,
        .esp_err = 0,
        .detail = "",
    };
}

/**
 * @brief 构造错误结果。
 *
 * @param code 模块内部结果码。
 * @param reason_code 稳定英文短码。
 * @param detail 已脱敏的诊断摘要，可为空字符串。
 * @return 错误的统一结果结构。
 */
static inline gw_result_t gw_result_error(int32_t code, const char *reason_code, const char *detail)
{
    return (gw_result_t) {
        .code = code,
        .reason_code = reason_code,
        .http_status = 0,
        .server_code = 0,
        .esp_err = 0,
        .detail = detail,
    };
}

#ifdef __cplusplus
}
#endif
