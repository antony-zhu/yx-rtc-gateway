/**
 * @file gw_meter_reporter.c
 * @brief DTSU666 周期 HTTP 上报应用实现。
 */

#include "gw_meter_reporter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_dtsu666.h"
#include "gw_http.h"
#include "gw_storage.h"

static const char *TAG = "gw_meter";

static const char s_upload_url[] =
    "http://adminapi.yxzhhz.net/yxzl/electricity/ts/push";
static const char s_content_type[] = "application/json";
static const char s_authorization[] = "Bearer yxzlAdmin2023";

static const uint32_t s_upload_interval_ms = 300000U;
static const uint32_t s_read_retry_delay_ms = 300U;
static const uint32_t s_read_retry_count = 3U;
static const uint32_t s_http_timeout_sec = 15U;
/* ESP-IDF's ESP32 port defines StackType_t as uint8_t, so this is an 8 KiB stack. */
static const uint32_t s_task_stack_bytes = 8U * 1024U;
static const UBaseType_t s_task_priority = 5U;

enum {
    GW_METER_PAYLOAD_BUFFER_SIZE = 1024,
    GW_METER_RESPONSE_BUFFER_SIZE = 256,
};

static TaskHandle_t s_task;

static esp_err_t format_local_time(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t seconds = time(NULL);
    if (seconds == (time_t)-1) {
        return ESP_FAIL;
    }

    struct tm local_tm;
    if (localtime_r(&seconds, &local_tm) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "%02d/%02d/%02d,%02d:%02d:%02d",
                           (local_tm.tm_year + 1900) % 100,
                           local_tm.tm_mon + 1,
                           local_tm.tm_mday,
                           local_tm.tm_hour,
                           local_tm.tm_min,
                           local_tm.tm_sec);
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t add_device_id(cJSON *root, const char *device_id)
{
    return cJSON_AddStringToObject(root,
                                   "deviceId",
                                   device_id != NULL ? device_id : "") != NULL
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

static esp_err_t add_meter_number(cJSON *root,
                                  const char *key,
                                  double value)
{
    if (root == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char number[48];
    int written = snprintf(number, sizeof(number), "%.2f", value);
    if (written <= 0 || (size_t)written >= sizeof(number)) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Raw JSON value preserves the Lua contract's fixed two decimal places. */
    return cJSON_AddRawToObject(root, key, number) != NULL
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

static esp_err_t add_meter_data(cJSON *root,
                                const gw_dtsu666_meter_data_t *meter)
{
    if (root == NULL || meter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[24];
#define ADD_METER_VALUE(prefix, field)                                                    \
    do {                                                                                   \
        int key_len = snprintf(key, sizeof(key), "%s_%u", prefix, (unsigned)meter->address); \
        if (key_len <= 0 || (size_t)key_len >= sizeof(key) ||                             \
            add_meter_number(root, key, meter->field) != ESP_OK) {                         \
            return ESP_ERR_NO_MEM;                                                        \
        }                                                                                  \
    } while (0)

    ADD_METER_VALUE("Ep", ep);
    ADD_METER_VALUE("U", u);
    ADD_METER_VALUE("I", i);
    ADD_METER_VALUE("P", p);
    ADD_METER_VALUE("Q", q);
    ADD_METER_VALUE("Pf", pf);
    ADD_METER_VALUE("Freq", freq);

#undef ADD_METER_VALUE
    return ESP_OK;
}

static esp_err_t build_upload_payload(const gw_dtsu666_snapshot_t *snapshot,
                                      const gw_storage_identity_t *identity,
                                      char *out,
                                      size_t out_size,
                                      size_t *out_len)
{
    if (snapshot == NULL || identity == NULL || out == NULL || out_len == NULL ||
        out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;

    char time_text[24];
    esp_err_t err = format_local_time(time_text, sizeof(time_text));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* The HTTP contract uses the same raw eFuse MAC as the MQTT identity. */
    err = add_device_id(root, identity->mac);
    if (err == ESP_OK && cJSON_AddStringToObject(root, "time", time_text) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    for (size_t i = 0U;
         err == ESP_OK && i < GW_DTSU666_METER_COUNT;
         ++i) {
        err = add_meter_data(root, &snapshot->meter[i]);
    }

    if (err == ESP_OK &&
        !cJSON_PrintPreallocated(root, out, (int)out_size, false)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        *out_len = strlen(out);
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t read_meter_with_retry(uint8_t address,
                                       gw_dtsu666_meter_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t last_err = ESP_FAIL;
    for (uint32_t attempt = 1U; attempt <= s_read_retry_count; ++attempt) {
        gw_dtsu666_meter_data_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        last_err = gw_dtsu666_read_meter(address, &candidate);
        if (last_err == ESP_OK) {
            *out = candidate;
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "read attempt %" PRIu32 "/%" PRIu32 " failed: %s",
                 attempt,
                 s_read_retry_count,
                 esp_err_to_name(last_err));
        if (attempt < s_read_retry_count) {
            vTaskDelay(pdMS_TO_TICKS(s_read_retry_delay_ms));
        }
    }

    memset(out, 0, sizeof(*out));
    out->address = address;
    return last_err;
}

static esp_err_t read_snapshot(gw_dtsu666_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint8_t meter_addresses[GW_DTSU666_METER_COUNT] = {10U, 11U};
    memset(out, 0, sizeof(*out));
    bool any_meter_ok = false;
    for (size_t i = 0U; i < GW_DTSU666_METER_COUNT; ++i) {
        out->meter[i].address = meter_addresses[i];
        if (read_meter_with_retry(meter_addresses[i], &out->meter[i]) == ESP_OK) {
            any_meter_ok = true;
        }
    }

    return any_meter_ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t report_once(void)
{
    gw_dtsu666_snapshot_t snapshot;
    esp_err_t err = read_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }

    gw_storage_identity_t identity;
    err = gw_storage_get_identity(&identity);
    if (err != ESP_OK) {
        return err;
    }

    char payload[GW_METER_PAYLOAD_BUFFER_SIZE];
    size_t payload_len = 0U;
    err = build_upload_payload(&snapshot,
                               &identity,
                               payload,
                               sizeof(payload),
                               &payload_len);
    if (err != ESP_OK) {
        return err;
    }

    char response_body[GW_METER_RESPONSE_BUFFER_SIZE] = {0};
    gw_http_request_t request = {
        .method = GW_HTTP_METHOD_POST,
        .url = s_upload_url,
        .content_type = s_content_type,
        .authorization = s_authorization,
        .body = payload,
        .body_len = payload_len,
        .response_buf = response_body,
        .response_buf_size = sizeof(response_body),
        .connect_timeout_sec = s_http_timeout_sec,
        .response_interval_timeout_sec = s_http_timeout_sec,
        .total_timeout_sec = s_http_timeout_sec,
        .allow_untrusted_https = false,
    };
    gw_http_response_t response = {0};
    err = gw_http_perform(&request, &response);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "HTTP upload failed: status=%d err=%s reason=%s",
                 response.http_status,
                 esp_err_to_name(err),
                 response.result.reason_code != NULL ? response.result.reason_code : "-");
        return err;
    }

    ESP_LOGI(TAG,
             "HTTP upload ok: status=%d payload_len=%u",
             response.http_status,
             (unsigned)payload_len);
    return ESP_OK;
}

static void meter_reporter_task(void *arg)
{
    (void)arg;
    bool driver_ready = false;

    for (;;) {
        if (!driver_ready) {
            esp_err_t init_err = gw_dtsu666_init();
            if (init_err != ESP_OK) {
                ESP_LOGW(TAG, "DTSU666 driver init failed: %s", esp_err_to_name(init_err));
                vTaskDelay(pdMS_TO_TICKS(s_upload_interval_ms));
                continue;
            }
            driver_ready = true;
        }

        esp_err_t err = report_once();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "meter report cycle failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(s_upload_interval_ms));
    }
}

esp_err_t gw_meter_reporter_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    BaseType_t result = xTaskCreate(meter_reporter_task,
                                    "meter_reporter",
                                    s_task_stack_bytes,
                                    NULL,
                                    s_task_priority,
                                    &s_task);
    if (result != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool gw_meter_reporter_is_started(void)
{
    return s_task != NULL;
}
