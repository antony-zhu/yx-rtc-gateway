#include "gw_business_mqtt_register.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "gw_defaults.h"
#include "gw_http.h"

static const char *TAG = "business_mqtt_reg";

/**
 * @brief 使用 volatile 写入，避免编译器省略凭据清理。
 */
static void secure_zero(void *data, size_t len)
{
    volatile unsigned char *cursor = (volatile unsigned char *)data;
    while (cursor != NULL && len > 0U) {
        *cursor++ = 0U;
        --len;
    }
}

void gw_business_mqtt_credentials_clear(gw_business_mqtt_credentials_t *credentials)
{
    if (credentials != NULL) {
        secure_zero(credentials, sizeof(*credentials));
    }
}

static esp_err_t fail_with_result(gw_result_t *out_result,
                                  esp_err_t err,
                                  int32_t code,
                                  const char *reason,
                                  const char *detail,
                                  int http_status,
                                  int server_code)
{
    if (out_result != NULL) {
        *out_result = gw_result_error(code, reason, detail);
        out_result->http_status = http_status;
        out_result->server_code = server_code;
        out_result->esp_err = err;
    }
    return err;
}

static bool query_char_is_safe(unsigned char value)
{
    return isalnum(value) != 0 || value == '_' || value == '.' || value == '-';
}

/**
 * @brief 按旧脚本规则对 query value 做百分号编码。
 */
static esp_err_t append_query_value(char *url,
                                    size_t url_size,
                                    size_t *inout_len,
                                    const char *value)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t used = *inout_len;

    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0';
         ++cursor) {
        size_t required = query_char_is_safe(*cursor) ? 1U : 3U;
        if (used + required + 1U > url_size) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (required == 1U) {
            url[used++] = (char)*cursor;
        } else {
            url[used++] = '%';
            url[used++] = HEX[*cursor >> 4U];
            url[used++] = HEX[*cursor & 0x0FU];
        }
    }

    url[used] = '\0';
    *inout_len = used;
    return ESP_OK;
}

static esp_err_t append_text(char *out, size_t out_size, size_t *inout_len, const char *text)
{
    size_t used = *inout_len;
    size_t text_len = strlen(text);
    if (used + text_len + 1U > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out + used, text, text_len + 1U);
    *inout_len = used + text_len;
    return ESP_OK;
}

static esp_err_t build_register_url(const char *base_url,
                                    const char *device_id,
                                    const char *group_id,
                                    char *out,
                                    size_t out_size)
{
    size_t base_len = strlen(base_url);
    if (base_len == 0U || base_len + 1U > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, base_url, base_len + 1U);
    size_t used = base_len;
    const char *separator = strchr(base_url, '?') != NULL ? "&mac=" : "?mac=";

    esp_err_t err = append_text(out, out_size, &used, separator);
    if (err == ESP_OK) {
        err = append_query_value(out, out_size, &used, device_id);
    }
    if (err == ESP_OK) {
        err = append_text(out, out_size, &used, "&groupId=");
    }
    if (err == ESP_OK) {
        err = append_query_value(out, out_size, &used, group_id);
    }
    return err;
}

static const char *json_string(const cJSON *object, const char *primary, const char *alias)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, primary);
    if ((!cJSON_IsString(item) || item->valuestring == NULL) && alias != NULL) {
        item = cJSON_GetObjectItemCaseSensitive(object, alias);
    }
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : NULL;
}

/** @brief 在释放 cJSON 堆对象前擦除其中复制出的凭据字符串。 */
static void secure_wipe_json_credentials(cJSON *object)
{
    static const char *const FIELD_NAMES[] = {
        "clientId", "client_id", "userName", "username", "password",
    };
    if (!cJSON_IsObject(object)) {
        return;
    }

    for (size_t i = 0U; i < sizeof(FIELD_NAMES) / sizeof(FIELD_NAMES[0]); ++i) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(object, FIELD_NAMES[i]);
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            secure_zero(item->valuestring, strlen(item->valuestring));
        }
    }
}

/** @brief 擦除根对象及 data 对象内的凭据副本后释放 JSON。 */
static void secure_delete_json(cJSON *root)
{
    if (root == NULL) {
        return;
    }
    secure_wipe_json_credentials(root);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (data != root) {
        secure_wipe_json_credentials(data);
    }
    cJSON_Delete(root);
}

static int parse_server_code(const cJSON *root, bool *valid)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (code == NULL) {
        *valid = true;
        return 200;
    }
    if (cJSON_IsNumber(code) && code->valuedouble >= INT32_MIN && code->valuedouble <= INT32_MAX) {
        int parsed = (int)code->valuedouble;
        *valid = code->valuedouble == (double)parsed;
        return parsed;
    }
    if (cJSON_IsString(code) && code->valuestring != NULL) {
        char *end = NULL;
        long parsed = strtol(code->valuestring, &end, 10);
        *valid = end != code->valuestring && *end == '\0' &&
                 parsed >= INT32_MIN && parsed <= INT32_MAX;
        return *valid ? (int)parsed : 0;
    }
    *valid = false;
    return 0;
}

static uint16_t parse_port_or_default(const cJSON *data)
{
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(data, "port");
    long parsed = 1883;

    if (cJSON_IsNumber(port)) {
        double candidate = port->valuedouble;
        if (isfinite(candidate)) {
            if (candidate < 1.0) {
                parsed = 1;
            } else if (candidate > 65535.0) {
                parsed = 65535;
            } else {
                parsed = (long)candidate;
            }
        }
    } else if (cJSON_IsString(port) && port->valuestring != NULL) {
        char *end = NULL;
        long candidate = strtol(port->valuestring, &end, 0);
        if (end != port->valuestring && *end == '\0') {
            parsed = candidate;
        }
    }

    /* 与脚本 `_to_int(..., 1, 65535)` 的边界收敛行为保持一致。 */
    if (parsed < 1) {
        parsed = 1;
    } else if (parsed > 65535) {
        parsed = 65535;
    }
    return (uint16_t)parsed;
}

static esp_err_t copy_field(char *out, size_t out_size, const char *value)
{
    if (value == NULL) {
        out[0] = '\0';
        return ESP_OK;
    }
    size_t len = strlen(value);
    if (len + 1U > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, value, len + 1U);
    return ESP_OK;
}

static esp_err_t parse_credentials(const char *body,
                                   gw_business_mqtt_credentials_t *credentials,
                                   gw_result_t *out_result,
                                   int http_status)
{
    cJSON *root = cJSON_Parse(body);
    if (!cJSON_IsObject(root)) {
        secure_delete_json(root);
        return fail_with_result(out_result,
                                ESP_ERR_INVALID_RESPONSE,
                                GW_RESULT_INTERNAL_ERROR,
                                GW_REASON_PARSE_FAILED,
                                "mqtt register json invalid",
                                http_status,
                                0);
    }

    bool code_valid = false;
    int server_code = parse_server_code(root, &code_valid);
    if (!code_valid || server_code != 200) {
        secure_delete_json(root);
        return fail_with_result(out_result,
                                ESP_ERR_INVALID_RESPONSE,
                                GW_RESULT_INTERNAL_ERROR,
                                code_valid ? GW_REASON_HTTP_ERROR : GW_REASON_PARSE_FAILED,
                                code_valid ? "mqtt register rejected" : "mqtt register code invalid",
                                http_status,
                                server_code);
    }

    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (data == NULL) {
        data = root;
    }
    if (!cJSON_IsObject(data)) {
        secure_delete_json(root);
        return fail_with_result(out_result,
                                ESP_ERR_INVALID_RESPONSE,
                                GW_RESULT_INTERNAL_ERROR,
                                GW_REASON_PARSE_FAILED,
                                "mqtt register data missing",
                                http_status,
                                server_code);
    }

    const char *host = json_string(data, "host", NULL);
    const char *client_id = json_string(data, "clientId", "client_id");
    const char *username = json_string(data, "userName", "username");
    const char *password = json_string(data, "password", NULL);
    if (host == NULL || host[0] == '\0' || client_id == NULL || client_id[0] == '\0') {
        secure_delete_json(root);
        return fail_with_result(out_result,
                                ESP_ERR_INVALID_RESPONSE,
                                GW_RESULT_INTERNAL_ERROR,
                                GW_REASON_PARSE_FAILED,
                                "mqtt register field missing",
                                http_status,
                                server_code);
    }

    esp_err_t err;
    if (strstr(host, "://") != NULL) {
        err = copy_field(credentials->broker_uri, sizeof(credentials->broker_uri), host);
    } else {
        int written = snprintf(credentials->broker_uri,
                               sizeof(credentials->broker_uri),
                               "mqtt://%s:%u",
                               host,
                               (unsigned)parse_port_or_default(data));
        err = written > 0 && (size_t)written < sizeof(credentials->broker_uri) ?
              ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = copy_field(credentials->client_id, sizeof(credentials->client_id), client_id);
    }
    if (err == ESP_OK) {
        err = copy_field(credentials->username, sizeof(credentials->username), username);
    }
    if (err == ESP_OK) {
        err = copy_field(credentials->password, sizeof(credentials->password), password);
    }

    secure_delete_json(root);
    if (err != ESP_OK) {
        gw_business_mqtt_credentials_clear(credentials);
        return fail_with_result(out_result,
                                err,
                                GW_RESULT_PAYLOAD_TOO_LARGE,
                                GW_REASON_PAYLOAD_TOO_LARGE,
                                "mqtt register field too long",
                                http_status,
                                server_code);
    }

    if (out_result != NULL) {
        *out_result = gw_result_ok();
        out_result->http_status = http_status;
        out_result->server_code = server_code;
    }
    return ESP_OK;
}

esp_err_t gw_business_mqtt_register(
    const gw_business_mqtt_register_config_t *config,
    gw_business_mqtt_credentials_t *out_credentials,
    gw_result_t *out_result)
{
    if (out_result != NULL) {
        *out_result = gw_result_ok();
    }
    if (out_credentials != NULL) {
        gw_business_mqtt_credentials_clear(out_credentials);
    }
    if (config == NULL || out_credentials == NULL ||
        config->business_device_id == NULL || config->business_device_id[0] == '\0') {
        return fail_with_result(out_result,
                                ESP_ERR_INVALID_ARG,
                                GW_RESULT_INVALID_FIELD,
                                GW_REASON_INVALID_FIELD,
                                "mqtt register config invalid",
                                0,
                                0);
    }

    const char *register_url = config->register_url != NULL && config->register_url[0] != '\0' ?
                               config->register_url : GW_BUSINESS_MQTT_REGISTER_DEFAULT_URL;
    const char *register_group_id = config->register_group_id != NULL &&
                                    config->register_group_id[0] != '\0' ?
                                    config->register_group_id :
                                    GW_BUSINESS_MQTT_REGISTER_DEFAULT_GROUP_ID;

    char url[GW_HTTP_MAX_URL_LEN];
    esp_err_t err = build_register_url(register_url,
                                       config->business_device_id,
                                       register_group_id,
                                       url,
                                       sizeof(url));
    if (err != ESP_OK) {
        return fail_with_result(out_result,
                                err,
                                GW_RESULT_INVALID_FIELD,
                                GW_REASON_INVALID_FIELD,
                                "mqtt register url too long",
                                0,
                                0);
    }

    uint32_t timeout_ms = config->timeout_ms != 0U ?
                          config->timeout_ms : GW_BUSINESS_MQTT_REGISTER_DEFAULT_TIMEOUT_MS;
    if (timeout_ms < GW_BUSINESS_MQTT_REGISTER_MIN_TIMEOUT_MS) {
        timeout_ms = GW_BUSINESS_MQTT_REGISTER_MIN_TIMEOUT_MS;
    } else if (timeout_ms > GW_BUSINESS_MQTT_REGISTER_MAX_TIMEOUT_MS) {
        timeout_ms = GW_BUSINESS_MQTT_REGISTER_MAX_TIMEOUT_MS;
    }
    uint32_t timeout_sec = (timeout_ms + 999U) / 1000U;

    size_t max_bytes = config->max_response_bytes != 0U ?
                       config->max_response_bytes :
                       GW_BUSINESS_MQTT_REGISTER_DEFAULT_MAX_BYTES;
    if (max_bytes < GW_BUSINESS_MQTT_REGISTER_MIN_RESPONSE_BYTES) {
        max_bytes = GW_BUSINESS_MQTT_REGISTER_MIN_RESPONSE_BYTES;
    } else if (max_bytes > GW_BUSINESS_MQTT_REGISTER_MAX_RESPONSE_BYTES) {
        max_bytes = GW_BUSINESS_MQTT_REGISTER_MAX_RESPONSE_BYTES;
    }

    char *response_body = (char *)calloc(1U, max_bytes + 1U);
    if (response_body == NULL) {
        return fail_with_result(out_result,
                                ESP_ERR_NO_MEM,
                                GW_RESULT_INTERNAL_ERROR,
                                GW_REASON_INTERNAL_ERROR,
                                "mqtt register response alloc failed",
                                0,
                                0);
    }

    gw_http_request_t request = {
        .method = GW_HTTP_METHOD_GET,
        .url = url,
        .response_buf = response_body,
        .response_buf_size = max_bytes + 1U,
        .connect_timeout_sec = timeout_sec,
        .response_interval_timeout_sec = timeout_sec,
        .total_timeout_sec = timeout_sec,
        .allow_untrusted_https = config->allow_untrusted_https,
    };
    gw_http_response_t response = {0};
    err = gw_http_perform(&request, &response);
    if (err != ESP_OK) {
        if (out_result != NULL) {
            *out_result = response.result;
        }
    } else if (response.response_truncated) {
        err = fail_with_result(out_result,
                               ESP_ERR_INVALID_SIZE,
                               GW_RESULT_PAYLOAD_TOO_LARGE,
                               GW_REASON_PAYLOAD_TOO_LARGE,
                               "mqtt register response too large",
                               response.http_status,
                               0);
    } else {
        err = parse_credentials(response_body,
                                out_credentials,
                                out_result,
                                response.http_status);
    }

    secure_zero(response_body, max_bytes + 1U);
    free(response_body);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "mqtt registered: group_id=%s client_id_len=%u",
                 register_group_id,
                 (unsigned)strlen(out_credentials->client_id));
    } else {
        gw_business_mqtt_credentials_clear(out_credentials);
    }
    return err;
}
