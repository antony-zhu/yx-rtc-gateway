#include "gw_gateway_protocol.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_business_mqtt.h"
#include "gw_gateway_timer.h"
#include "gw_ota.h"
#include "gw_time.h"

static const char *TAG = "gw_gateway";
static TaskHandle_t s_worker;
static portMUX_TYPE s_id_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_last_id_ms;
static unsigned s_id_sequence;

typedef struct {
    SemaphoreHandle_t semaphore;
    esp_err_t result;
} publication_wait_t;

static void publication_done(void *context, esp_err_t result)
{
    publication_wait_t *wait = context;
    wait->result = result;
    xSemaphoreGive(wait->semaphore);
}

static void next_event_id(char out[17])
{
    uint64_t now = gw_time_now().utc_ms;
    portENTER_CRITICAL(&s_id_lock);
    if (now > s_last_id_ms) {
        s_last_id_ms = now;
        s_id_sequence = 0;
    } else if (++s_id_sequence == 1000U) {
        ++s_last_id_ms;
        s_id_sequence = 0;
    }
    uint64_t id_ms = s_last_id_ms;
    unsigned sequence = s_id_sequence;
    portEXIT_CRITICAL(&s_id_lock);
    snprintf(out, 17, "%013" PRIu64 "%03u", id_ms % 10000000000000ULL, sequence % 1000U);
}

static esp_err_t publish_envelope(const char *mt,
                                  const char *dt,
                                  const cJSON *id,
                                  const cJSON *pd,
                                  bool wait_for_attempt)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *id_copy = id != NULL ? cJSON_Duplicate(id, true) : cJSON_CreateNull();
    cJSON *pd_copy = cJSON_Duplicate(pd, true);
    if (root == NULL || id_copy == NULL || pd_copy == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(id_copy);
        cJSON_Delete(pd_copy);
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(root, "pv", "1") == NULL ||
        cJSON_AddStringToObject(root, "dt", dt) == NULL ||
        cJSON_AddStringToObject(root, "mt", mt) == NULL ||
        !cJSON_AddItemToObject(root, "id", id_copy)) {
        cJSON_Delete(id_copy);
        cJSON_Delete(pd_copy);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToObject(root, "pd", pd_copy)) {
        cJSON_Delete(pd_copy);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;
    if (wait_for_attempt) {
        StaticSemaphore_t semaphore_storage;
        publication_wait_t wait = {
            .semaphore = xSemaphoreCreateBinaryStatic(&semaphore_storage),
            .result = ESP_FAIL,
        };
        if (wait.semaphore == NULL) {
            cJSON_free(json);
            return ESP_ERR_NO_MEM;
        }
        err = gw_business_mqtt_publish_gateway(json, strlen(json), publication_done, &wait);
        if (err == ESP_OK) {
            xSemaphoreTake(wait.semaphore, portMAX_DELAY);
            err = wait.result;
        }
        vSemaphoreDelete(wait.semaphore);
    } else {
        err = gw_business_mqtt_publish_gateway(json, strlen(json), NULL, NULL);
    }
    cJSON_free(json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s publication dropped/failed: %s", mt, esp_err_to_name(err));
    }
    return err;
}

esp_err_t gw_gateway_publish_event(const char *mt,
                                   const char *dt,
                                   const cJSON *pd,
                                   bool wait_for_attempt)
{
    if (mt == NULL || dt == NULL || !cJSON_IsObject(pd)) {
        return ESP_ERR_INVALID_ARG;
    }
    char id_text[17];
    next_event_id(id_text);
    cJSON *id = cJSON_CreateString(id_text);
    if (id == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = publish_envelope(mt, dt, id, pd, wait_for_attempt);
    cJSON_Delete(id);
    return err;
}

static cJSON *result_pd(int code, const char *message)
{
    cJSON *pd = cJSON_CreateObject();
    if (pd == NULL) {
        return NULL;
    }
    if (cJSON_AddNumberToObject(pd, "code", code) == NULL ||
        cJSON_AddStringToObject(pd, "message", message) == NULL) {
        cJSON_Delete(pd);
        return NULL;
    }
    return pd;
}

static const char *json_type(const cJSON *node)
{
    if (node == NULL) return "missing";
    if (cJSON_IsNull(node)) return "null";
    if (cJSON_IsBool(node)) return "boolean";
    if (cJSON_IsNumber(node)) return "number";
    if (cJSON_IsString(node)) return "string";
    if (cJSON_IsArray(node)) return "array";
    return "object";
}

static cJSON *field_error(const char *path, const char *expected, const cJSON *actual)
{
    char message[192];
    snprintf(message, sizeof(message), "%s: expected %s; got %s", path, expected, json_type(actual));
    return result_pd(1, message);
}

static esp_err_t publish_ack(const char *mt,
                             const char *dt,
                             const cJSON *id,
                             const cJSON *pd,
                             bool wait)
{
    if (pd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = strlen(mt);
    if (len > SIZE_MAX - 5U) {
        return ESP_ERR_NO_MEM;
    }
    char *ack_mt = malloc(len + 5U);
    if (ack_mt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(ack_mt, mt, len);
    memcpy(ack_mt + len, ".ack", 5U);
    esp_err_t err = publish_envelope(ack_mt, dt, id, pd, wait);
    free(ack_mt);
    return err;
}

static bool timer_method(const char *mt)
{
    static const char *const methods[] = {
        "gateway.timer.list", "gateway.timer.get", "gateway.timer.upsert",
        "gateway.timer.enable", "gateway.timer.delete",
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (strcmp(mt, methods[i]) == 0) return true;
    }
    return false;
}

static bool json_unsigned(const cJSON *node, uint64_t min, uint64_t max, uint64_t *out)
{
    if (!cJSON_IsNumber(node) || !isfinite(node->valuedouble) ||
        node->valuedouble < (double)min || node->valuedouble > (double)max ||
        floor(node->valuedouble) != node->valuedouble) {
        return false;
    }
    *out = (uint64_t)node->valuedouble;
    return true;
}

static cJSON *parse_ota(const cJSON *pd, const char *id, gw_ota_upgrade_request_t *request)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(pd, "version");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(pd, "url");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(pd, "size");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(pd, "sha256");
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(pd, "progress_report_interval");
    uint64_t size_value, interval_value;
    if (!cJSON_IsString(version) || version->valuestring[0] == '\0') {
        return field_error("pd.version", "nonempty string", version);
    }
    if (!cJSON_IsString(url) ||
        (strncmp(url->valuestring, "http://", 7) != 0 &&
         strncmp(url->valuestring, "https://", 8) != 0)) {
        return field_error("pd.url", "HTTP or HTTPS URL string", url);
    }
    if (!json_unsigned(size, 41, UINT32_MAX, &size_value)) {
        return field_error("pd.size", "integer > 40 fitting an OTA container", size);
    }
    if (!cJSON_IsString(sha) || strlen(sha->valuestring) != 64U ||
        strspn(sha->valuestring, "0123456789abcdefABCDEF") != 64U) {
        return field_error("pd.sha256", "64 hexadecimal characters", sha);
    }
    if (!json_unsigned(interval, 0, 9007199254740991ULL, &interval_value)) {
        return field_error("pd.progress_report_interval", "nonnegative exact integer", interval);
    }
    request->gateway_protocol = true;
    request->gateway_version = version->valuestring;
    request->gateway_url = url->valuestring;
    request->gateway_progress_interval_sec = interval_value;
    memcpy(request->request_id, id, strlen(id) + 1U);
    memcpy(request->sha256, sha->valuestring, 65U);
    request->size = (uint32_t)size_value;
    request->reboot_after_upgrade = true;
    return NULL;
}

static int ota_error_code(esp_err_t err, const gw_result_t *result, bool rollback)
{
    if (result->code == GW_RESULT_DEVICE_BUSY) return 8;
    if (err == ESP_ERR_NO_MEM || result->reason_code == NULL ||
        strcmp(result->reason_code, GW_REASON_NOT_INITIALIZED) == 0) return 14;
    if (rollback) return 11;
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE ||
        result->code == GW_RESULT_INVALID_FIELD) return 1;
    return 14;
}

static cJSON *ota_failure(esp_err_t err, const gw_result_t *result, bool rollback)
{
    int code = ota_error_code(err, result, rollback);
    const char *message = code == 8 ? "OTA is busy" : result->detail;
    if (message == NULL || message[0] == '\0') {
        message = result->reason_code != NULL ? result->reason_code : "internal_error";
    }
    return result_pd(code, message);
}

static void handle_ota(const char *mt, const cJSON *id, const cJSON *pd)
{
    gw_ota_upgrade_request_t request = {0};
    cJSON *reply = parse_ota(pd, id->valuestring, &request);
    /* No error JSON can be allocated on OOM; the populated marker distinguishes success. */
    if (!request.gateway_protocol) {
        (void)publish_ack(mt, "plc", id, reply, false);
        cJSON_Delete(reply);
        return;
    }
    reply = result_pd(0, "OTA started");
    if (reply == NULL) return;
    gw_result_t result;
    esp_err_t err = gw_ota_manager_submit_upgrade(&request, &result);
    if (err != ESP_OK) {
        cJSON_Delete(reply);
        reply = ota_failure(err, &result, false);
        (void)publish_ack(mt, "plc", id, reply, false);
        cJSON_Delete(reply);
        return;
    }
    /* FIFO insertion precedes worker start. An unavailable QoS0 ACK may be dropped. */
    (void)publish_ack(mt, "plc", id, reply, false);
    cJSON_Delete(reply);
    err = gw_ota_manager_start_pending_upgrade(true, request.request_id);
    if (err != ESP_OK) {
        gw_ota_manager_cancel_pending_upgrade(true, request.request_id, GW_REASON_INTERNAL_ERROR);
        reply = result_pd(14, "OTA worker start failed");
        if (reply != NULL &&
            cJSON_AddStringToObject(reply, "request_id", id->valuestring) != NULL &&
            cJSON_AddStringToObject(reply, "version", request.gateway_version) != NULL) {
            (void)gw_gateway_publish_event("gateway.ota.result", "plc", reply, false);
        }
        cJSON_Delete(reply);
    }
}

static void handle_rollback(const char *mt, const cJSON *id, const cJSON *pd)
{
    cJSON *reply;
    if (pd->child != NULL) {
        reply = result_pd(1, "pd: expected empty object");
        (void)publish_ack(mt, "plc", id, reply, false);
        cJSON_Delete(reply);
        return;
    }
    reply = result_pd(0, "Rollback prepared; rebooting");
    if (reply == NULL) return;
    gw_result_t result;
    esp_err_t err = gw_ota_manager_prepare_rollback(&result);
    if (err != ESP_OK) {
        cJSON_Delete(reply);
        reply = ota_failure(err, &result, true);
        (void)publish_ack(mt, "plc", id, reply, false);
        cJSON_Delete(reply);
        return;
    }
    (void)publish_ack(mt, "plc", id, reply, true);
    cJSON_Delete(reply);
    err = gw_ota_manager_reboot_prepared_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prepared rollback reboot failed: %s", esp_err_to_name(err));
    }
}

static void handle_message(const gw_business_mqtt_owned_message_t *message)
{
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)message->payload,
                                           message->payload_len + 1U, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }
    const cJSON *mt_node = cJSON_GetObjectItemCaseSensitive(root, "mt");
    if (!cJSON_IsString(mt_node) || mt_node->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return;
    }
    const char *mt = mt_node->valuestring;
    bool timer = timer_method(mt);
    bool ota = strcmp(mt, "gateway.ota") == 0;
    bool rollback = strcmp(mt, "gateway.ota.rollback") == 0;
    if (rollback && message->retained) {
        cJSON_Delete(root);
        return;
    }
    const char *dt_text = timer ? "gateway" : "plc";
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(root, "pv");
    const cJSON *dt = cJSON_GetObjectItemCaseSensitive(root, "dt");
    const cJSON *pd = cJSON_GetObjectItemCaseSensitive(root, "pd");
    cJSON *reply = NULL;
    bool invalid = true;
    if (!timer && !ota && !rollback) {
        reply = result_pd(3, "unknown_message");
    } else if (!cJSON_IsString(id) || strlen(id->valuestring) >= 32U) {
        reply = field_error("id", "string shorter than 32 UTF-8 bytes", id);
    } else if (!cJSON_IsString(pv)) {
        reply = field_error("pv", "string \"1\"", pv);
    } else if (strcmp(pv->valuestring, "1") != 0) {
        reply = result_pd(2, "unsupported_version");
    } else if (!cJSON_IsString(dt) || strcmp(dt->valuestring, dt_text) != 0) {
        reply = field_error("dt", dt_text, dt);
    } else if (!cJSON_IsObject(pd)) {
        reply = field_error("pd", "object", pd);
    } else {
        invalid = false;
    }
    if (invalid) {
        (void)publish_ack(mt, dt_text, id, reply, false);
        cJSON_Delete(reply);
    } else if (timer) {
        reply = gw_gateway_timer_handle(mt, pd);
        (void)publish_ack(mt, dt_text, id, reply, false);
        cJSON_Delete(reply);
    } else if (ota) {
        handle_ota(mt, id, pd);
    } else {
        handle_rollback(mt, id, pd);
    }
    cJSON_Delete(root);
}

static void gateway_worker(void *arg)
{
    (void)arg;
    for (;;) {
        gw_business_mqtt_owned_message_t message = {0};
        if (gw_business_mqtt_recv_gateway(&message, 1000U) == ESP_OK) {
            handle_message(&message);
            gw_business_mqtt_message_release(&message);
        }
    }
}

esp_err_t gw_gateway_protocol_init(void)
{
    if (s_worker != NULL) return ESP_OK;
    esp_err_t err = gw_gateway_timer_init();
    if (err != ESP_OK) return err;
    return xTaskCreate(gateway_worker, "gateway_cmd", 8192U, NULL, 6, &s_worker) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
