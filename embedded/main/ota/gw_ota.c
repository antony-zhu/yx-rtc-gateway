#include "gw_ota.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

#include "gw_config_manager.h"
#include "gw_crypto.h"
#include "gw_defaults.h"
#include "gw_http.h"
#include "gw_log.h"
#include "gw_mqtt_manager.h"
#include "gw_net.h"
#include "gw_protocol_manager.h"
#include "gw_reboot_manager.h"
#include "gw_script_manager.h"
#include "gw_state.h"
#include "gw_storage.h"
#include "gw_time.h"
#include "gw_version.h"

static const char *TAG = "gw_ota";

#define GW_OTA_FINAL_REPLY_RETRY_COUNT       5U
#define GW_OTA_FINAL_REPLY_RETRY_DELAY_MS    1000U

#ifndef OTA_SIZE_UNKNOWN
#define OTA_SIZE_UNKNOWN 0xffffffffU
#endif

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    bool handle_valid;
    bool ended;
} gw_ota_writer_t;

static portMUX_TYPE s_ota_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_ota_queue;
static TaskHandle_t s_ota_task_handle;
static gw_ota_status_t s_status;
static gw_storage_identity_t s_identity;
static bool s_pending_upgrade_valid;
static gw_ota_upgrade_request_t s_pending_upgrade;

#ifdef GW_OTA_MANAGER_ENABLE_TEST_HOOKS
#define GW_OTA_HTTP_BUFFER_ALLOC(size) malloc(size)
#define GW_OTA_HTTP_BUFFER_FREE(ptr)   free(ptr)
#else
#define GW_OTA_HTTP_BUFFER_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define GW_OTA_HTTP_BUFFER_FREE(ptr)   heap_caps_free(ptr)
#endif

static bool is_nonempty_string(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static uint64_t trusted_utc_or_zero(void)
{
    gw_time_point_t now = gw_time_now();
    return now.utc_trusted ? now.utc_ms : 0;
}

static void normalize_sha256(char *value)
{
    if (value == NULL) {
        return;
    }

    for (char *p = value; *p != '\0'; ++p) {
        *p = (char)tolower((unsigned char)*p);
    }
}

static bool parse_version3(const char *value, uint8_t out[3])
{
    if (!is_nonempty_string(value) || out == NULL) {
        return false;
    }

    unsigned int part[3] = {0};
    char tail = '\0';
    int matched = sscanf(value, "%u.%u.%u%c", &part[0], &part[1], &part[2], &tail);
    if (matched != 3) {
        return false;
    }

    for (size_t i = 0U; i < 3U; ++i) {
        if (part[i] > 255U) {
            return false;
        }
        out[i] = (uint8_t)part[i];
    }
    return true;
}

static int compare_version3(const uint8_t left[3], const uint8_t right[3])
{
    for (size_t i = 0U; i < 3U; ++i) {
        if (left[i] > right[i]) {
            return 1;
        }
        if (left[i] < right[i]) {
            return -1;
        }
    }
    return 0;
}

static gw_result_t result_with_details(int32_t code,
                                       const char *reason,
                                       const char *detail,
                                       int32_t http_status,
                                       int32_t esp_err)
{
    gw_result_t result = gw_result_error(code, reason, detail);
    result.http_status = http_status;
    result.esp_err = esp_err;
    return result;
}

static void set_stage_locked(gw_ota_stage_t stage)
{
    s_status.stage = stage;
}

static void set_stage(gw_ota_stage_t stage)
{
    portENTER_CRITICAL(&s_ota_lock);
    set_stage_locked(stage);
    portEXIT_CRITICAL(&s_ota_lock);
}

static void update_transfer_progress(uint32_t downloaded_size, uint32_t total_size)
{
    uint8_t progress = 0U;
    if (total_size > 0U) {
        uint64_t percent = ((uint64_t)downloaded_size * 100ULL) / (uint64_t)total_size;
        progress = (uint8_t)(percent > 100ULL ? 100U : percent);
    }

    portENTER_CRITICAL(&s_ota_lock);
    s_status.downloaded_size = downloaded_size;
    s_status.progress = progress;
    portEXIT_CRITICAL(&s_ota_lock);
}

static void update_last_result(const gw_result_t *result)
{
    portENTER_CRITICAL(&s_ota_lock);
    s_status.last_code = result != NULL ? result->code : GW_RESULT_INTERNAL_ERROR;
    s_status.last_http_status = result != NULL ? result->http_status : 0;
    s_status.last_esp_err = result != NULL ? result->esp_err : 0;
    strlcpy(s_status.last_reason,
            result != NULL && result->reason_code != NULL ? result->reason_code : GW_REASON_INTERNAL_ERROR,
            sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_ota_lock);
}

static void mark_task_started(const gw_ota_upgrade_request_t *request)
{
    portENTER_CRITICAL(&s_ota_lock);
    set_stage_locked(GW_OTA_STAGE_ACCEPTED);
    ++s_status.accepted_count;
    strlcpy(s_status.running_request_id, request->request_id, sizeof(s_status.running_request_id));
    strlcpy(s_status.target_version, request->target_version, sizeof(s_status.target_version));
    s_status.image_size = request->size;
    s_status.downloaded_size = 0;
    s_status.progress = 0;
    s_status.has_scheduled_at = request->has_scheduled_at;
    s_status.scheduled_at_ms = request->scheduled_at_ms;
    s_status.last_code = GW_RESULT_OK;
    s_status.last_http_status = 0;
    s_status.last_esp_err = 0;
    strlcpy(s_status.last_reason, GW_REASON_ACCEPTED, sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_ota_lock);
}

static void mark_task_finished(gw_ota_stage_t stage, const gw_result_t *result)
{
    portENTER_CRITICAL(&s_ota_lock);
    set_stage_locked(stage);
    if (stage == GW_OTA_STAGE_SUCCESS) {
        ++s_status.success_count;
        s_status.progress = 100U;
        s_status.downloaded_size = s_status.image_size;
    } else if (stage == GW_OTA_STAGE_FAILED) {
        ++s_status.failed_count;
    }
    s_status.running_request_id[0] = '\0';
    s_status.last_code = result != NULL ? result->code : GW_RESULT_INTERNAL_ERROR;
    s_status.last_http_status = result != NULL ? result->http_status : 0;
    s_status.last_esp_err = result != NULL ? result->esp_err : 0;
    strlcpy(s_status.last_reason,
            result != NULL && result->reason_code != NULL ? result->reason_code : GW_REASON_INTERNAL_ERROR,
            sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_ota_lock);
}

static esp_http_client_addr_type_t select_addr_type(const char *url)
{
    if (url == NULL) {
        return HTTP_ADDR_TYPE_INET;
    }

    const char *scheme = strstr(url, "://");
    const char *host = scheme != NULL ? scheme + 3 : url;
    return host[0] == '[' ? HTTP_ADDR_TYPE_INET6 : HTTP_ADDR_TYPE_INET;
}

static esp_err_t build_ota_topic(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "/%s/YxzhNode%s/thing/model/up_raw",
                           s_identity.group_id,
                           s_identity.device_id);
    if (written <= 0 || written >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t publish_progress(const gw_ota_upgrade_request_t *request,
                                  gw_ota_stage_t stage,
                                  const gw_result_t *result,
                                  uint32_t downloaded_size)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t code = result != NULL ? result->code : GW_RESULT_OK;
    const char *message = result != NULL && result->reason_code != NULL ?
                          result->reason_code :
                          GW_REASON_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    uint8_t progress = 0U;
    if (request->size > 0U) {
        uint64_t percent = ((uint64_t)downloaded_size * 100ULL) / (uint64_t)request->size;
        progress = (uint8_t)(percent > 100ULL ? 100U : percent);
    }
    if (stage == GW_OTA_STAGE_SUCCESS) {
        progress = 100U;
    }

    cJSON_AddStringToObject(root, "version", GW_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "method", "ota.progress");
    cJSON_AddStringToObject(root, "request_id", request->request_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)trusted_utc_or_zero());
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message);

    cJSON_AddStringToObject(data, "target_version", request->target_version);
    cJSON_AddStringToObject(data, "stage", gw_ota_stage_to_string(stage));
    cJSON_AddNumberToObject(data, "progress", progress);
    cJSON_AddNumberToObject(data, "downloaded_size", downloaded_size);
    cJSON_AddNumberToObject(data, "total_size", request->size);
    if (request->has_scheduled_at) {
        cJSON_AddNumberToObject(data, "scheduled_at", (double)request->scheduled_at_ms);
    }
    if (result != NULL && result->code != GW_RESULT_OK) {
        cJSON_AddStringToObject(data,
                                "reason_code",
                                result->reason_code != NULL ? result->reason_code : GW_REASON_INTERNAL_ERROR);
        cJSON_AddNumberToObject(data, "http_status", result->http_status);
        cJSON_AddNumberToObject(data, "esp_err", result->esp_err);
    }
    cJSON_AddItemToObject(root, "data", data);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_ERR_INVALID_SIZE;
    size_t payload_len = strlen(payload);
    if (payload_len <= GW_MQTT_MAX_PAYLOAD_SIZE) {
        char topic[GW_PROTOCOL_TOPIC_MAX_LEN];
        err = build_ota_topic(topic, sizeof(topic));
        if (err == ESP_OK) {
            err = gw_mqtt_manager_publish(topic, payload, payload_len);
        }
    }

    cJSON_free(payload);

    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_ota_lock);
        ++s_status.progress_publish_failed;
        portEXIT_CRITICAL(&s_ota_lock);
    }
    return err;
}

static esp_err_t publish_progress_with_retry(const gw_ota_upgrade_request_t *request,
                                             gw_ota_stage_t stage,
                                             const gw_result_t *result,
                                             uint32_t downloaded_size)
{
    esp_err_t last_err = ESP_FAIL;
    for (uint32_t attempt = 1U; attempt <= GW_OTA_FINAL_REPLY_RETRY_COUNT; ++attempt) {
        last_err = publish_progress(request, stage, result, downloaded_size);
        if (last_err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG,
                 "ota progress publish failed: request_id=%s stage=%s attempt=%lu/%lu err=%s",
                 request != NULL ? request->request_id : "",
                 gw_ota_stage_to_string(stage),
                 (unsigned long)attempt,
                 (unsigned long)GW_OTA_FINAL_REPLY_RETRY_COUNT,
                 esp_err_to_name(last_err));
        if (attempt < GW_OTA_FINAL_REPLY_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(GW_OTA_FINAL_REPLY_RETRY_DELAY_MS));
        }
    }
    return last_err;
}

static esp_err_t begin_ota_writer(const gw_ota_upgrade_request_t *request, gw_ota_writer_t *writer)
{
    if (request == NULL || writer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(writer, 0, sizeof(*writer));
    writer->partition = esp_ota_get_next_update_partition(NULL);
    if (writer->partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_begin(writer->partition,
                                  request->size > 0U ? request->size : OTA_SIZE_UNKNOWN,
                                  &writer->handle);
    if (err != ESP_OK) {
        return err;
    }
    writer->handle_valid = true;
    return ESP_OK;
}

static void abort_ota_writer(gw_ota_writer_t *writer)
{
    if (writer != NULL && writer->handle_valid && !writer->ended) {
        (void)esp_ota_abort(writer->handle);
        writer->handle_valid = false;
    }
}

static gw_result_t finish_ota_writer(gw_ota_writer_t *writer)
{
    if (writer == NULL || !writer->handle_valid || writer->partition == NULL) {
        return result_with_details(GW_RESULT_INTERNAL_ERROR,
                                   GW_REASON_INTERNAL_ERROR,
                                   "ota writer invalid",
                                   0,
                                   ESP_ERR_INVALID_STATE);
    }

    set_stage(GW_OTA_STAGE_VERIFYING);
    esp_err_t err = esp_ota_end(writer->handle);
    writer->ended = true;
    writer->handle_valid = false;
    if (err != ESP_OK) {
        return result_with_details(GW_RESULT_INTERNAL_ERROR,
                                   GW_REASON_INTERNAL_ERROR,
                                   "esp_ota_end failed",
                                   0,
                                   err);
    }

    set_stage(GW_OTA_STAGE_SETTING_BOOT);
    err = esp_ota_set_boot_partition(writer->partition);
    if (err != ESP_OK) {
        return result_with_details(GW_RESULT_INTERNAL_ERROR,
                                   GW_REASON_INTERNAL_ERROR,
                                   "set boot partition failed",
                                   0,
                                   err);
    }

    return gw_result_ok();
}

static gw_result_t stop_script_before_ota_write(bool *io_script_stopped)
{
    set_stage(GW_OTA_STAGE_STOPPING_SCRIPT);

    bool script_stopped = false;
    esp_err_t stop_err = gw_script_manager_request_stop(GW_SCRIPT_STOP_REASON_OTA_UPDATING,
                                                        GW_SCRIPT_STOP_TIMEOUT_MS,
                                                        &script_stopped);
    if (io_script_stopped != NULL && script_stopped) {
        *io_script_stopped = true;
    }
    if (stop_err == ESP_ERR_TIMEOUT) {
        gw_log_write(GW_LOG_LEVEL_WARN,
                     "ota",
                     "script_stop_timeout",
                     "script stop timeout, continue ota",
                     &((gw_result_t) {
                         .code = GW_RESULT_INTERNAL_ERROR,
                         .reason_code = GW_SCRIPT_STOP_REASON_OTA_UPDATING,
                         .http_status = 0,
                         .server_code = 0,
                         .esp_err = stop_err,
                         .detail = "script stop timeout",
                     }));
        return gw_result_ok();
    }
    if (stop_err != ESP_OK) {
        return result_with_details(GW_RESULT_INTERNAL_ERROR,
                                   GW_REASON_INTERNAL_ERROR,
                                   "script stop failed",
                                   0,
                                   stop_err);
    }

    return gw_result_ok();
}

static gw_result_t download_once(const gw_ota_upgrade_request_t *request,
                                 uint32_t *out_downloaded_size,
                                 bool *io_script_stopped)
{
    if (out_downloaded_size != NULL) {
        *out_downloaded_size = 0U;
    }

    esp_http_client_handle_t client = NULL;
    uint8_t *buffer = NULL;
    gw_ota_writer_t writer = {0};
    mbedtls_md_context_t sha_ctx;
    bool sha_ready = false;
    gw_result_t result = gw_result_ok();

    mbedtls_md_init(&sha_ctx);

    esp_http_client_config_t config = {
        .url = request->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)(GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC * 1000U),
        .addr_type = select_addr_type(request->url),
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    client = esp_http_client_init(&config);
    buffer = (uint8_t *)GW_OTA_HTTP_BUFFER_ALLOC(GW_OTA_HTTP_BUF_BYTES);
    if (client == NULL || buffer == NULL) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_INTERNAL_ERROR,
                                     "ota http alloc failed",
                                     0,
                                     ESP_ERR_NO_MEM);
        goto cleanup;
    }

    char redacted[GW_HTTP_MAX_URL_LEN];
    if (gw_http_redact_url(request->url, redacted, sizeof(redacted)) != ESP_OK) {
        strlcpy(redacted, "<url-too-long>", sizeof(redacted));
    }
    ESP_LOGI(TAG, "ota http request: %s", redacted);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_HTTP_ERROR,
                                     "http open failed",
                                     0,
                                     err);
        goto cleanup;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    if (content_length < 0) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_HTTP_ERROR,
                                     "http fetch headers failed",
                                     http_status,
                                     ESP_FAIL);
        goto cleanup;
    }
    if (http_status < 200 || http_status >= 300) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_HTTP_ERROR,
                                     "http status is not 2xx",
                                     http_status,
                                     ESP_OK);
        goto cleanup;
    }
    if (content_length > 0 && (uint64_t)content_length != (uint64_t)request->size) {
        result = result_with_details(GW_RESULT_INVALID_FIELD,
                                     GW_REASON_SIZE_MISMATCH,
                                     "ota content length mismatch",
                                     http_status,
                                     ESP_OK);
        goto cleanup;
    }

    /*
     * 先完成不写 flash 的 HTTP 预检查，再停脚本。这样 size_mismatch、HTTP 状态码错误
     * 等坏包失败路径不会扰动当前业务脚本，只有真正准备写 OTA 分区时才暂停脚本。
     */
    if (io_script_stopped == NULL || !*io_script_stopped) {
        result = stop_script_before_ota_write(io_script_stopped);
        if (result.code != GW_RESULT_OK) {
            goto cleanup;
        }
    }

    (void)esp_http_client_set_timeout_ms(client,
                                         (int)(GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC * 1000U));

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL ||
        mbedtls_md_setup(&sha_ctx, info, 0) != 0 ||
        mbedtls_md_starts(&sha_ctx) != 0) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_INTERNAL_ERROR,
                                     "sha256 init failed",
                                     http_status,
                                     ESP_FAIL);
        goto cleanup;
    }
    sha_ready = true;

    err = begin_ota_writer(request, &writer);
    if (err != ESP_OK) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_INTERNAL_ERROR,
                                     "ota begin failed",
                                     http_status,
                                     err);
        goto cleanup;
    }

    set_stage(GW_OTA_STAGE_DOWNLOADING);
    uint32_t downloaded = 0U;
    TickType_t last_progress_tick = 0;
    TickType_t progress_interval = pdMS_TO_TICKS((request->progress_interval_sec > 0U ?
                                                  request->progress_interval_sec :
                                                  GW_OTA_PROGRESS_INTERVAL_SEC) * 1000U);

    for (;;) {
        int read_len = esp_http_client_read(client, (char *)buffer, GW_OTA_HTTP_BUF_BYTES);
        if (read_len < 0) {
            result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                         GW_REASON_HTTP_ERROR,
                                         "http read failed",
                                         http_status,
                                         ESP_FAIL);
            goto cleanup;
        }
        if (read_len == 0) {
            break;
        }

        err = esp_ota_write(writer.handle, buffer, (size_t)read_len);
        if (err != ESP_OK) {
            result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                         GW_REASON_INTERNAL_ERROR,
                                         "ota write failed",
                                         http_status,
                                         err);
            goto cleanup;
        }
        if (sha_ready && mbedtls_md_update(&sha_ctx, buffer, (size_t)read_len) != 0) {
            result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                         GW_REASON_INTERNAL_ERROR,
                                         "sha256 update failed",
                                         http_status,
                                         ESP_FAIL);
            goto cleanup;
        }

        downloaded += (uint32_t)read_len;
        update_transfer_progress(downloaded, request->size);
        if (out_downloaded_size != NULL) {
            *out_downloaded_size = downloaded;
        }

        TickType_t now_tick = xTaskGetTickCount();
        if (last_progress_tick == 0 || now_tick - last_progress_tick >= progress_interval) {
            (void)publish_progress(request, GW_OTA_STAGE_DOWNLOADING, NULL, downloaded);
            last_progress_tick = now_tick;
        }
    }

    if (!esp_http_client_is_complete_data_received(client)) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_HTTP_ERROR,
                                     "http body incomplete",
                                     http_status,
                                     ESP_FAIL);
        goto cleanup;
    }
    if (downloaded != request->size) {
        result = result_with_details(GW_RESULT_INVALID_FIELD,
                                     GW_REASON_SIZE_MISMATCH,
                                     "ota size mismatch",
                                     http_status,
                                     ESP_OK);
        goto cleanup;
    }

    uint8_t actual_sha256[GW_CRYPTO_SHA256_SIZE] = {0};
    uint8_t expected_sha256[GW_CRYPTO_SHA256_SIZE] = {0};
    if (mbedtls_md_finish(&sha_ctx, actual_sha256) != 0 ||
        !gw_crypto_sha256_hex_to_bytes(request->sha256, expected_sha256)) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_INTERNAL_ERROR,
                                     "sha256 finish failed",
                                     http_status,
                                     ESP_FAIL);
        goto cleanup;
    }
    sha_ready = false;
    if (memcmp(actual_sha256, expected_sha256, sizeof(actual_sha256)) != 0) {
        result = result_with_details(GW_RESULT_INTERNAL_ERROR,
                                     GW_REASON_SHA256_MISMATCH,
                                     "ota sha256 mismatch",
                                     http_status,
                                     ESP_ERR_INVALID_CRC);
        goto cleanup;
    }

    result = finish_ota_writer(&writer);
    result.http_status = http_status;

cleanup:
    if (result.code != GW_RESULT_OK) {
        abort_ota_writer(&writer);
    }
    if (sha_ready) {
        mbedtls_md_free(&sha_ctx);
    } else {
        mbedtls_md_free(&sha_ctx);
    }
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (buffer != NULL) {
        GW_OTA_HTTP_BUFFER_FREE(buffer);
    }
    return result;
}

static gw_result_t download_with_retries(const gw_ota_upgrade_request_t *request,
                                         uint32_t *out_downloaded_size,
                                         bool *io_script_stopped)
{
    static const uint32_t retry_delay_sec[] = {3U, 10U, 30U};
    gw_result_t result = gw_result_ok();

    for (uint32_t attempt = 0U; attempt < GW_HTTP_DEFAULT_RETRY_COUNT; ++attempt) {
        if (out_downloaded_size != NULL) {
            *out_downloaded_size = 0U;
        }
        result = download_once(request, out_downloaded_size, io_script_stopped);
        if (result.code == GW_RESULT_OK) {
            return result;
        }

        update_last_result(&result);
        ESP_LOGW(TAG,
                 "ota download attempt failed: request_id=%s attempt=%lu/%lu reason=%s esp_err=%ld http=%ld",
                 request->request_id,
                 (unsigned long)(attempt + 1U),
                 (unsigned long)GW_HTTP_DEFAULT_RETRY_COUNT,
                 result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR,
                 (long)result.esp_err,
                 (long)result.http_status);

        if (result.code == GW_RESULT_INVALID_FIELD) {
            break;
        }

        if (attempt + 1U < GW_HTTP_DEFAULT_RETRY_COUNT) {
            uint32_t delay_sec = retry_delay_sec[attempt < 3U ? attempt : 2U];
            vTaskDelay(pdMS_TO_TICKS(delay_sec * 1000U));
        }
    }

    return result;
}

static gw_result_t wait_for_schedule(const gw_ota_upgrade_request_t *request)
{
    if (request == NULL || !request->has_scheduled_at) {
        return gw_result_ok();
    }

    TickType_t last_publish_tick = 0;
    gw_ota_stage_t last_stage = GW_OTA_STAGE_IDLE;
    TickType_t interval = pdMS_TO_TICKS((request->progress_interval_sec > 0U ?
                                         request->progress_interval_sec :
                                         GW_OTA_PROGRESS_INTERVAL_SEC) * 1000U);

    for (;;) {
        gw_time_point_t now = gw_time_now();
        if (now.utc_trusted && now.utc_ms >= request->scheduled_at_ms) {
            return gw_result_ok();
        }

        gw_ota_stage_t stage = now.utc_trusted ?
                               GW_OTA_STAGE_WAITING_SCHEDULE :
                               GW_OTA_STAGE_WAITING_TIME_SYNC;
        set_stage(stage);
        TickType_t now_tick = xTaskGetTickCount();
        if (stage != last_stage ||
            last_publish_tick == 0 ||
            now_tick - last_publish_tick >= interval) {
            (void)publish_progress(request, stage, NULL, 0);
            last_stage = stage;
            last_publish_tick = now_tick;
        }

        uint32_t sleep_ms = 1000U;
        if (now.utc_trusted && request->scheduled_at_ms > now.utc_ms) {
            uint64_t remain_ms = request->scheduled_at_ms - now.utc_ms;
            if (remain_ms < sleep_ms) {
                sleep_ms = (uint32_t)remain_ms;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(sleep_ms > 0U ? sleep_ms : 1U));
    }
}

static esp_err_t restart_active_script_after_ota_failed(void)
{
#if !GW_SCRIPT_AUTOSTART_ENABLED
    /* 纯 C 业务模式下 OTA 失败也不得重新执行历史脚本。 */
    return ESP_OK;
#else
    gw_config_active_t active;
    esp_err_t err = gw_config_manager_get_active(&active);
    if (err != ESP_OK) {
        return err;
    }
    return gw_script_manager_restart(active.active_slot);
#endif
}

static gw_result_t run_ota_upgrade(const gw_ota_upgrade_request_t *request, bool *out_script_stopped)
{
    if (out_script_stopped != NULL) {
        *out_script_stopped = false;
    }

    gw_result_t result = wait_for_schedule(request);
    if (result.code != GW_RESULT_OK) {
        return result;
    }

    gw_net_status_snapshot_t net_status;
    if (gw_net_get_status(&net_status) != ESP_OK ||
        net_status.public_status != GW_NETWORK_STATUS_CONNECTED) {
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_NETWORK_UNAVAILABLE,
                               "network unavailable");
    }

    uint32_t downloaded_size = 0U;
    result = download_with_retries(request, &downloaded_size, out_script_stopped);
    update_transfer_progress(downloaded_size, request->size);
    if (result.code == GW_RESULT_OK) {
        (void)gw_time_maybe_save_last_trusted(true);
    }
    return result;
}

static void log_ota_result(const gw_ota_upgrade_request_t *request,
                           const gw_result_t *result,
                           gw_ota_stage_t final_stage)
{
    if (request == NULL || result == NULL) {
        return;
    }

    char message[160];
    snprintf(message,
             sizeof(message),
             "ota %s, request_id=%s, target_version=%s",
             final_stage == GW_OTA_STAGE_SUCCESS ? "success" : "failed",
             request->request_id,
             request->target_version);
    gw_log_write(final_stage == GW_OTA_STAGE_SUCCESS ? GW_LOG_LEVEL_INFO : GW_LOG_LEVEL_WARN,
                 "ota",
                 final_stage == GW_OTA_STAGE_SUCCESS ? "upgrade_success" : "upgrade_failed",
                 message,
                 result);
}

static void ota_worker_task(void *arg)
{
    (void)arg;
    gw_ota_upgrade_request_t request;

    for (;;) {
        if (xQueueReceive(s_ota_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        gw_device_status_t restore_device_status = GW_DEVICE_STATUS_ONLINE;
        gw_state_snapshot_t state_before_ota = {0};
        if (gw_state_get_snapshot(&state_before_ota) == ESP_OK) {
            restore_device_status = state_before_ota.device_status;
        }
        gw_state_set_device_status(GW_DEVICE_STATUS_OTA_UPGRADING);
        gw_state_set_task_status(GW_TASK_STATUS_OTA_UPGRADING);

        bool script_stopped = false;
        gw_result_t result = run_ota_upgrade(&request, &script_stopped);
        gw_ota_stage_t final_stage = result.code == GW_RESULT_OK ?
                                     GW_OTA_STAGE_SUCCESS :
                                     GW_OTA_STAGE_FAILED;
        uint32_t downloaded_size = final_stage == GW_OTA_STAGE_SUCCESS ?
                                   request.size :
                                   s_status.downloaded_size;

        log_ota_result(&request, &result, final_stage);
        (void)publish_progress_with_retry(&request, final_stage, &result, downloaded_size);
        gw_protocol_task_end(request.request_id,
                             result.code,
                             result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR);
        mark_task_finished(final_stage, &result);

        if (final_stage == GW_OTA_STAGE_SUCCESS) {
            gw_reboot_request_t reboot_request = {
                .delay_ms = GW_REBOOT_DEFAULT_DELAY_MS,
                .execute = true,
            };
            strlcpy(reboot_request.request_id, request.request_id, sizeof(reboot_request.request_id));
            strlcpy(reboot_request.reason, GW_REBOOT_REASON_OTA_UPDATED, sizeof(reboot_request.reason));

            gw_result_t reboot_result;
            esp_err_t reboot_err = gw_reboot_manager_request(&reboot_request, &reboot_result);
            if (reboot_err != ESP_OK) {
                gw_log_write(GW_LOG_LEVEL_WARN,
                             "ota",
                             "reboot_request_failed",
                             "ota succeeded but reboot request failed",
                             &reboot_result);
            }
        } else {
            if (script_stopped) {
                esp_err_t restart_err = restart_active_script_after_ota_failed();
                if (restart_err != ESP_OK) {
                    gw_log_write(GW_LOG_LEVEL_WARN,
                                 "ota",
                                 "script_restart_failed",
                                 "ota failed and script restart failed",
                                 &((gw_result_t) {
                                     .code = GW_RESULT_INTERNAL_ERROR,
                                     .reason_code = GW_REASON_INTERNAL_ERROR,
                                     .http_status = 0,
                                     .server_code = 0,
                                     .esp_err = restart_err,
                                     .detail = "script restart failed",
                                 }));
                }
            }
            gw_state_set_device_status(restore_device_status);
        }
    }
}

static esp_err_t validate_upgrade_request(const gw_ota_upgrade_request_t *request,
                                          gw_result_t *out_result)
{
    if (request == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                  GW_REASON_INVALID_FIELD,
                                  "invalid ota upgrade request");

    uint8_t target_version[3] = {0};
    uint8_t current_version[3] = {
        (uint8_t)GW_FIRMWARE_VERSION_MAJOR,
        (uint8_t)GW_FIRMWARE_VERSION_MINOR,
        (uint8_t)GW_FIRMWARE_VERSION_PATCH,
    };

    if (!is_nonempty_string(request->request_id) ||
        strlen(request->request_id) > GW_REQUEST_ID_MAX_LEN ||
        !parse_version3(request->target_version, target_version) ||
        !is_nonempty_string(request->url) ||
        !gw_http_url_scheme_allowed(request->url) ||
        !gw_crypto_sha256_hex_to_bytes(request->sha256, (uint8_t[GW_CRYPTO_SHA256_SIZE]){0}) ||
        request->size == 0U ||
        (request->has_scheduled_at && request->scheduled_at_ms == 0ULL) ||
        request->progress_interval_sec > 3600U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (compare_version3(target_version, current_version) <= 0) {
        *out_result = gw_result_error(GW_RESULT_VERSION_NOT_INCREASED,
                                      GW_REASON_VERSION_NOT_INCREASED,
                                      "ota target version not increased");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_INTERNAL_ERROR,
                                      "no ota partition");
        return ESP_ERR_NOT_FOUND;
    }
    if ((uint64_t)request->size > (uint64_t)partition->size) {
        *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                      GW_REASON_INVALID_FIELD,
                                      "ota image exceeds partition");
        return ESP_ERR_INVALID_SIZE;
    }

    *out_result = gw_result_ok();
    return ESP_OK;
}

esp_err_t gw_ota_manager_init(void)
{
    if (s_ota_task_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gw_storage_get_identity(&s_identity), TAG, "load identity failed");

    if (s_ota_queue == NULL) {
        s_ota_queue = xQueueCreate(1, sizeof(gw_ota_upgrade_request_t));
        if (s_ota_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.stage = GW_OTA_STAGE_IDLE;
    strlcpy(s_status.last_reason, GW_REASON_OK, sizeof(s_status.last_reason));
    portENTER_CRITICAL(&s_ota_lock);
    s_pending_upgrade_valid = false;
    memset(&s_pending_upgrade, 0, sizeof(s_pending_upgrade));
    portEXIT_CRITICAL(&s_ota_lock);

    BaseType_t ok = xTaskCreate(ota_worker_task,
                                "ota_task",
                                GW_TASK_STACK_OTA_BYTES,
                                NULL,
                                7,
                                &s_ota_task_handle);
    if (ok != pdPASS) {
        s_ota_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_ota_lock);
    s_status.worker_started = true;
    portEXIT_CRITICAL(&s_ota_lock);
    return ESP_OK;
}

esp_err_t gw_ota_manager_submit_upgrade(const gw_ota_upgrade_request_t *request,
                                        gw_result_t *out_result)
{
    if (request == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_queue == NULL || s_ota_task_handle == NULL) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_NOT_INITIALIZED,
                                      "ota manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    gw_ota_upgrade_request_t copy = *request;
    normalize_sha256(copy.sha256);
    copy.reboot_after_upgrade = true;
    if (copy.progress_interval_sec == 0U) {
        copy.progress_interval_sec = GW_OTA_PROGRESS_INTERVAL_SEC;
    }

    esp_err_t err = validate_upgrade_request(&copy, out_result);
    if (err != ESP_OK) {
        update_last_result(out_result);
        return err;
    }

    err = gw_protocol_task_begin("ota.upgrade",
                                 copy.request_id,
                                 GW_TASK_STATUS_OTA_UPGRADING);
    if (err != ESP_OK) {
        *out_result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                      GW_REASON_DEVICE_BUSY,
                                      "ota task busy");
        update_last_result(out_result);
        return err;
    }

    mark_task_started(&copy);

    portENTER_CRITICAL(&s_ota_lock);
    if (s_pending_upgrade_valid) {
        portEXIT_CRITICAL(&s_ota_lock);
        gw_protocol_task_end(copy.request_id, GW_RESULT_DEVICE_BUSY, GW_REASON_QUEUE_FULL);
        *out_result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                      GW_REASON_QUEUE_FULL,
                                      "ota queue full");
        mark_task_finished(GW_OTA_STAGE_FAILED, out_result);
        return ESP_ERR_INVALID_STATE;
    }
    s_pending_upgrade = copy;
    s_pending_upgrade_valid = true;
    portEXIT_CRITICAL(&s_ota_lock);

    *out_result = gw_result_error(GW_RESULT_OK, GW_REASON_ACCEPTED, "accepted");
    return ESP_OK;
}

bool gw_ota_manager_has_pending_upgrade(void)
{
    bool pending = false;
    portENTER_CRITICAL(&s_ota_lock);
    pending = s_pending_upgrade_valid;
    portEXIT_CRITICAL(&s_ota_lock);
    return pending;
}

esp_err_t gw_ota_manager_start_pending_upgrade(void)
{
    if (s_ota_queue == NULL || s_ota_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_ota_upgrade_request_t request;
    portENTER_CRITICAL(&s_ota_lock);
    if (!s_pending_upgrade_valid) {
        portEXIT_CRITICAL(&s_ota_lock);
        return ESP_ERR_NOT_FOUND;
    }
    request = s_pending_upgrade;
    memset(&s_pending_upgrade, 0, sizeof(s_pending_upgrade));
    s_pending_upgrade_valid = false;
    portEXIT_CRITICAL(&s_ota_lock);

    if (xQueueSend(s_ota_queue, &request, 0) == pdPASS) {
        return ESP_OK;
    }

    gw_result_t result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                         GW_REASON_QUEUE_FULL,
                                         "ota queue full");
    gw_protocol_task_end(request.request_id,
                         result.code,
                         result.reason_code != NULL ? result.reason_code : GW_REASON_QUEUE_FULL);
    mark_task_finished(GW_OTA_STAGE_FAILED, &result);
    (void)publish_progress_with_retry(&request, GW_OTA_STAGE_FAILED, &result, 0);
    return ESP_ERR_TIMEOUT;
}

void gw_ota_manager_cancel_pending_upgrade(const char *reason)
{
    gw_ota_upgrade_request_t request;
    bool had_pending = false;

    portENTER_CRITICAL(&s_ota_lock);
    if (s_pending_upgrade_valid) {
        request = s_pending_upgrade;
        memset(&s_pending_upgrade, 0, sizeof(s_pending_upgrade));
        s_pending_upgrade_valid = false;
        had_pending = true;
    }
    portEXIT_CRITICAL(&s_ota_lock);

    if (!had_pending) {
        return;
    }

    const char *final_reason = reason != NULL ? reason : GW_REASON_INTERNAL_ERROR;
    gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                         final_reason,
                                         "accepted reply publish failed");
    gw_protocol_task_end(request.request_id, result.code, result.reason_code);
    mark_task_finished(GW_OTA_STAGE_FAILED, &result);
}

esp_err_t gw_ota_manager_get_status(gw_ota_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_ota_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_ota_lock);
    return ESP_OK;
}

#ifdef GW_OTA_MANAGER_ENABLE_TEST_HOOKS
void gw_ota_manager_test_reset(void)
{
    /*
     * 单元测试会在同一固件进程内反复初始化 OTA 管理器。正式固件不会编译该入口，
     * 测试侧用它清理 worker、队列和 pending 状态，确保用例之间互不影响。
     */
    if (s_ota_task_handle != NULL) {
        vTaskDelete(s_ota_task_handle);
        s_ota_task_handle = NULL;
    }

    if (s_ota_queue != NULL) {
        vQueueDelete(s_ota_queue);
        s_ota_queue = NULL;
    }

    portENTER_CRITICAL(&s_ota_lock);
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_identity, 0, sizeof(s_identity));
    memset(&s_pending_upgrade, 0, sizeof(s_pending_upgrade));
    s_pending_upgrade_valid = false;
    portEXIT_CRITICAL(&s_ota_lock);
}

gw_result_t gw_ota_manager_test_download_once(const gw_ota_upgrade_request_t *request,
                                              uint32_t *out_downloaded_size)
{
    return download_once(request, out_downloaded_size, NULL);
}
#endif

const char *gw_ota_stage_to_string(gw_ota_stage_t stage)
{
    switch (stage) {
    case GW_OTA_STAGE_IDLE:
        return "idle";
    case GW_OTA_STAGE_ACCEPTED:
        return "accepted";
    case GW_OTA_STAGE_WAITING_TIME_SYNC:
        return "waiting_time_sync";
    case GW_OTA_STAGE_WAITING_SCHEDULE:
        return "waiting_schedule";
    case GW_OTA_STAGE_STOPPING_SCRIPT:
        return "stopping_script";
    case GW_OTA_STAGE_DOWNLOADING:
        return "downloading";
    case GW_OTA_STAGE_VERIFYING:
        return "verifying";
    case GW_OTA_STAGE_SETTING_BOOT:
        return "setting_boot_partition";
    case GW_OTA_STAGE_SUCCESS:
        return "success";
    case GW_OTA_STAGE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
