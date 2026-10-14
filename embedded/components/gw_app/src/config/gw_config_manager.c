#include "gw_config_manager.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "gw_defaults.h"
#include "gw_fs.h"
#include "gw_http.h"
#include "gw_log.h"
#include "gw_mqtt_manager.h"
#include "gw_net.h"
#include "gw_param.h"
#include "gw_protocol_manager.h"
#include "gw_reboot_manager.h"
#include "gw_state.h"
#include "gw_storage.h"
#include "gw_time.h"
#include "gw_version.h"

static const char *TAG = "gw_config";

#define GW_CONFIG_FINAL_REPLY_RETRY_COUNT       5U
#define GW_CONFIG_FINAL_REPLY_RETRY_DELAY_MS    1000U

typedef struct {
    char name[GW_FS_MAX_PATH_LEN];
    uint32_t size;
    char sha256[65];
} gw_manifest_file_t;

typedef struct {
    uint32_t seq_id;
    uint32_t file_count;
    char min_product_version[16];
    bool has_config_json;
    uint32_t parsed_file_count;
    uint32_t total_file_size;
    gw_manifest_file_t files[GW_CONFIG_MANIFEST_MAX_FILES];
} gw_manifest_info_t;

#ifdef GW_CONFIG_MANAGER_ENABLE_TEST_HOOKS
typedef struct {
    uint32_t seq_id;
    uint32_t file_count;
    uint32_t parsed_file_count;
    uint32_t total_file_size;
    char min_product_version[16];
    bool has_config_json;
    char first_name[GW_FS_MAX_PATH_LEN];
    char second_name[GW_FS_MAX_PATH_LEN];
} gw_config_manager_test_manifest_summary_t;
#endif

static portMUX_TYPE s_config_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_config_queue;
static TaskHandle_t s_config_task_handle;
static gw_config_status_t s_status;
static gw_storage_identity_t s_identity;
static bool s_pending_update_valid;
static gw_config_update_request_t s_pending_update;

static bool is_nonempty_string(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static uint64_t trusted_utc_or_zero(void)
{
    gw_time_point_t now = gw_time_now();
    return now.utc_trusted ? now.utc_ms : 0;
}

static bool is_sha256_hex(const char *value)
{
    if (value == NULL || strlen(value) != 64U) {
        return false;
    }

    for (const char *p = value; *p != '\0'; ++p) {
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
    }

    return true;
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

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";

    if (out == NULL || out_size < len * 2U + 1U) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        out[i * 2U] = hex[(bytes[i] >> 4) & 0x0f];
        out[i * 2U + 1U] = hex[bytes[i] & 0x0f];
    }
    out[len * 2U] = '\0';
}

static esp_err_t sha256_buffer_hex(const void *data, size_t len, char out[65])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL || data == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[32];
    int rc = mbedtls_md(info, (const unsigned char *)data, len, digest);
    if (rc != 0) {
        return ESP_FAIL;
    }

    bytes_to_hex(digest, sizeof(digest), out, 65U);
    return ESP_OK;
}

static void set_stage_locked(gw_config_stage_t stage)
{
    s_status.stage = stage;
}

static void set_stage(gw_config_stage_t stage)
{
    portENTER_CRITICAL(&s_config_lock);
    set_stage_locked(stage);
    portEXIT_CRITICAL(&s_config_lock);
}

static void update_last_result(int32_t code,
                               const char *reason,
                               int32_t http_status,
                               int32_t esp_err)
{
    portENTER_CRITICAL(&s_config_lock);
    s_status.last_code = code;
    s_status.last_http_status = http_status;
    s_status.last_esp_err = esp_err;
    strlcpy(s_status.last_reason,
            reason != NULL ? reason : GW_REASON_INTERNAL_ERROR,
            sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_config_lock);
}

static void mark_task_started(const gw_config_update_request_t *request)
{
    portENTER_CRITICAL(&s_config_lock);
    set_stage_locked(GW_CONFIG_STAGE_ACCEPTED);
    ++s_status.accepted_count;
    strlcpy(s_status.running_request_id, request->request_id, sizeof(s_status.running_request_id));
    s_status.running_sequence = request->sequence;
    strlcpy(s_status.last_reason, GW_REASON_ACCEPTED, sizeof(s_status.last_reason));
    s_status.last_code = GW_RESULT_OK;
    s_status.last_http_status = 0;
    s_status.last_esp_err = 0;
    portEXIT_CRITICAL(&s_config_lock);
}

static void mark_task_finished(gw_config_stage_t stage, const gw_result_t *result)
{
    portENTER_CRITICAL(&s_config_lock);
    set_stage_locked(stage);
    if (stage == GW_CONFIG_STAGE_SUCCESS) {
        ++s_status.success_count;
    } else if (stage == GW_CONFIG_STAGE_FAILED) {
        ++s_status.failed_count;
    }
    s_status.running_request_id[0] = '\0';
    s_status.running_sequence = 0;
    s_status.last_code = result != NULL ? result->code : GW_RESULT_INTERNAL_ERROR;
    s_status.last_http_status = result != NULL ? result->http_status : 0;
    s_status.last_esp_err = result != NULL ? result->esp_err : 0;
    strlcpy(s_status.last_reason,
            result != NULL && result->reason_code != NULL ? result->reason_code : GW_REASON_INTERNAL_ERROR,
            sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_config_lock);
}

static gw_config_stage_t get_stage_snapshot(void)
{
    gw_config_stage_t stage;
    portENTER_CRITICAL(&s_config_lock);
    stage = s_status.stage;
    portEXIT_CRITICAL(&s_config_lock);
    return stage;
}

static void update_active_status(const gw_config_active_t *active)
{
    if (active == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_config_lock);
    s_status.active = *active;
    portEXIT_CRITICAL(&s_config_lock);
}

static esp_err_t load_u32_param(const char *key, uint32_t default_value, uint32_t *out)
{
    char value[16];
    esp_err_t err = gw_storage_secure_get_string(GW_NVS_NAMESPACE_CONFIG, key, value, sizeof(value));
    if (err != ESP_OK) {
        *out = default_value;
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > UINT32_MAX) {
        *out = default_value;
        return ESP_ERR_INVALID_ARG;
    }

    *out = (uint32_t)parsed;
    return ESP_OK;
}

static esp_err_t load_active_state_json(gw_config_active_t *out)
{
    char text[256];
    esp_err_t err = gw_storage_secure_get_string(GW_NVS_NAMESPACE_CONFIG,
                                                 GW_PARAM_KEY_CONFIG_ACTIVE_STATE,
                                                 text,
                                                 sizeof(text));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *slot = cJSON_GetObjectItemCaseSensitive(root, "active_slot");
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq_id");
    cJSON *sha = cJSON_GetObjectItemCaseSensitive(root, "manifest_sha256");
    if (!cJSON_IsString(slot) ||
        (strcmp(slot->valuestring, "cfg_a") != 0 && strcmp(slot->valuestring, "cfg_b") != 0) ||
        !cJSON_IsNumber(seq) ||
        seq->valuedouble < 0 ||
        seq->valuedouble > UINT32_MAX ||
        (double)(uint32_t)seq->valuedouble != seq->valuedouble ||
        !cJSON_IsString(sha) ||
        !is_sha256_hex(sha->valuestring)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->active_slot, slot->valuestring, sizeof(out->active_slot));
    out->seq_id = (uint32_t)seq->valuedouble;
    strlcpy(out->manifest_sha256, sha->valuestring, sizeof(out->manifest_sha256));
    normalize_sha256(out->manifest_sha256);
    out->loaded_from_nvs = true;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t load_active_state(gw_config_active_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->active_slot, "cfg_a", sizeof(out->active_slot));

    if (load_active_state_json(out) == ESP_OK) {
        return ESP_OK;
    }

    esp_err_t slot_err = gw_storage_secure_get_string(GW_NVS_NAMESPACE_CONFIG,
                                                      GW_PARAM_KEY_ACTIVE_CONFIG_SLOT,
                                                      out->active_slot,
                                                      sizeof(out->active_slot));
    if (slot_err != ESP_OK ||
        (strcmp(out->active_slot, "cfg_a") != 0 && strcmp(out->active_slot, "cfg_b") != 0)) {
        strlcpy(out->active_slot, "cfg_a", sizeof(out->active_slot));
    } else {
        out->loaded_from_nvs = true;
    }

    esp_err_t seq_err = load_u32_param(GW_PARAM_KEY_CONFIG_SEQ_ID, 0, &out->seq_id);
    if (seq_err == ESP_OK && out->seq_id != 0) {
        out->loaded_from_nvs = true;
    }

    esp_err_t sha_err = gw_storage_secure_get_string(GW_NVS_NAMESPACE_CONFIG,
                                                     GW_PARAM_KEY_CONFIG_MANIFEST_SHA256,
                                                     out->manifest_sha256,
                                                     sizeof(out->manifest_sha256));
    if (sha_err == ESP_OK) {
        out->loaded_from_nvs = true;
    } else {
        out->manifest_sha256[0] = '\0';
    }

    return ESP_OK;
}

static esp_err_t save_active_state(const gw_config_active_t *active)
{
    if (active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "active_slot", active->active_slot);
    cJSON_AddNumberToObject(root, "seq_id", active->seq_id);
    cJSON_AddStringToObject(root, "manifest_sha256", active->manifest_sha256);
    cJSON_AddNumberToObject(root, "updated_at", (double)trusted_utc_or_zero());

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = gw_storage_secure_set_string(GW_NVS_NAMESPACE_CONFIG,
                                                 GW_PARAM_KEY_CONFIG_ACTIVE_STATE,
                                                 text);
    cJSON_free(text);
    return err;
}

static const char *inactive_slot_name(const char *active_slot)
{
    return strcmp(active_slot, "cfg_a") == 0 ? "cfg_b" : "cfg_a";
}

static esp_err_t build_slot_root_path(const char *slot, char *out, size_t out_size)
{
    if (!is_nonempty_string(slot) || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out, out_size, "%s/%s", GW_FS_CFGFS_BASE_PATH, slot);
    return (written > 0 && written < (int)out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t build_slot_file_path(const char *slot,
                                      const char *name,
                                      const char *suffix,
                                      char *out,
                                      size_t out_size)
{
    if (!is_nonempty_string(slot) || !is_nonempty_string(name) || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "%s/%s/%s%s",
                           GW_FS_CFGFS_BASE_PATH,
                           slot,
                           name,
                           suffix != NULL ? suffix : "");
    return (written > 0 && written < (int)out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t mkdir_if_missing(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t ensure_parent_dirs(const char *path)
{
    if (!is_nonempty_string(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    char temp[GW_FS_MAX_PATH_LEN];
    if (strlcpy(temp, path, sizeof(temp)) >= sizeof(temp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (char *p = temp + 1; *p != '\0'; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        esp_err_t err = mkdir_if_missing(temp);
        *p = '/';
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t remove_tree_contents(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return errno == ENOENT ? mkdir_if_missing(dir_path) : ESP_FAIL;
    }

    esp_err_t final_err = ESP_OK;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child[GW_FS_MAX_PATH_LEN];
        int written = snprintf(child, sizeof(child), "%s/%s", dir_path, entry->d_name);
        if (written <= 0 || written >= (int)sizeof(child)) {
            final_err = ESP_ERR_INVALID_SIZE;
            continue;
        }

        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            esp_err_t err = remove_tree_contents(child);
            if (err != ESP_OK && final_err == ESP_OK) {
                final_err = err;
            }
        }

        if (remove(child) != 0 && final_err == ESP_OK) {
            final_err = ESP_FAIL;
        }
    }

    closedir(dir);
    return final_err;
}

static esp_err_t prepare_target_slot(const char *slot)
{
    char root[GW_FS_MAX_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_slot_root_path(slot, root, sizeof(root)), TAG, "build slot root failed");
    ESP_RETURN_ON_ERROR(mkdir_if_missing(root), TAG, "create slot root failed");
    return remove_tree_contents(root);
}

static esp_err_t write_binary_file(const char *path, const void *data, size_t len)
{
    if (!is_nonempty_string(path) || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ensure_parent_dirs(path), TAG, "ensure parent dirs failed");

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    size_t written = len > 0 ? fwrite(data, 1, len, fp) : 0;
    int close_rc = fclose(fp);
    if (written != len || close_rc != 0) {
        (void)remove(path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t commit_temp_file(const char *tmp_path, const char *final_path)
{
    if (!is_nonempty_string(tmp_path) || !is_nonempty_string(final_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)remove(final_path);
    if (rename(tmp_path, final_path) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t sha256_file_hex(const char *path, size_t expected_size, char out[65])
{
    if (!is_nonempty_string(path) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size != expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        return ESP_FAIL;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 0);
    if (rc == 0) {
        rc = mbedtls_md_starts(&ctx);
    }

    uint8_t buffer[1024];
    while (rc == 0) {
        size_t n = fread(buffer, 1, sizeof(buffer), fp);
        if (n > 0) {
            rc = mbedtls_md_update(&ctx, buffer, n);
        }
        if (n < sizeof(buffer)) {
            if (ferror(fp)) {
                rc = -1;
            }
            break;
        }
    }

    uint8_t digest[32];
    if (rc == 0) {
        rc = mbedtls_md_finish(&ctx, digest);
    }
    mbedtls_md_free(&ctx);
    fclose(fp);

    if (rc != 0) {
        return ESP_FAIL;
    }

    bytes_to_hex(digest, sizeof(digest), out, 65U);
    return ESP_OK;
}

static esp_err_t read_file_alloc(const char *path, size_t max_size, char **out, size_t *out_len)
{
    if (!is_nonempty_string(path) || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = NULL;
    *out_len = 0;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (size_t)st.st_size > max_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    char *buf = (char *)malloc((size_t)st.st_size + 1U);
    if (buf == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t got = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (got != (size_t)st.st_size) {
        free(buf);
        return ESP_FAIL;
    }

    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return ESP_OK;
}

static bool url_char_is_unreserved(char c)
{
    unsigned char ch = (unsigned char)c;
    return isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

static esp_err_t append_url_encoded(char *out, size_t out_size, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";

    if (out == NULL || text == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t used = strlen(out);
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (url_char_is_unreserved((char)*p)) {
            if (used + 2U > out_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[used++] = (char)*p;
            out[used] = '\0';
        } else {
            if (used + 4U > out_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[used++] = '%';
            out[used++] = hex[(*p >> 4) & 0x0f];
            out[used++] = hex[*p & 0x0f];
            out[used] = '\0';
        }
    }

    return ESP_OK;
}

static esp_err_t build_file_url(const char *manifest_url,
                                const char *name,
                                char *out,
                                size_t out_size)
{
    if (!is_nonempty_string(manifest_url) || !is_nonempty_string(name) || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *scheme = strstr(manifest_url, "://");
    if (scheme == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *host = scheme + 3;
    const char *path_start = strchr(host, '/');
    size_t base_len = 0;
    if (path_start == NULL) {
        base_len = strlen(manifest_url);
    } else {
        const char *last_slash = strrchr(path_start, '/');
        base_len = (size_t)(last_slash - manifest_url + 1);
    }

    if (base_len + 2U > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, manifest_url, base_len);
    out[base_len] = '\0';
    if (path_start == NULL) {
        if (strlcat(out, "/", out_size) >= out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return append_url_encoded(out, out_size, name);
}

static esp_err_t perform_http_with_retries(const gw_http_request_t *request,
                                           gw_http_response_t *response)
{
    static const uint32_t retry_delay_sec[] = {3U, 10U, 10U};

    esp_err_t err = ESP_FAIL;
    for (uint32_t attempt = 0; attempt < GW_HTTP_DEFAULT_RETRY_COUNT; ++attempt) {
        err = gw_http_perform(request, response);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (attempt + 1U < GW_HTTP_DEFAULT_RETRY_COUNT) {
            uint32_t delay_sec = retry_delay_sec[attempt < 3U ? attempt : 2U];
            vTaskDelay(pdMS_TO_TICKS(delay_sec * 1000U));
        }
    }

    return err;
}

static esp_err_t build_config_topic(char *out, size_t out_size)
{
    int written = snprintf(out,
                           out_size,
                           "%s/YxzhNode%s/thing/model/up_raw",
                           s_identity.group_id,
                           s_identity.device_id);
    if (written <= 0 || written >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t publish_final_reply(const gw_config_update_request_t *request,
                                     const gw_result_t *result,
                                     const char *stage,
                                     gw_config_stage_t failed_stage)
{
    if (request == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "version", GW_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "method", "config.update.reply");
    cJSON_AddStringToObject(root, "request_id", request->request_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)trusted_utc_or_zero());
    cJSON_AddNumberToObject(root, "code", result->code);
    cJSON_AddStringToObject(root,
                            "message",
                            result->reason_code != NULL ? result->reason_code : GW_REASON_INTERNAL_ERROR);

    cJSON_AddStringToObject(data, "stage", stage != NULL ? stage : "failed");
    if (result->code != GW_RESULT_OK) {
        cJSON_AddStringToObject(data, "failed_stage", gw_config_stage_to_string(failed_stage));
    }
    cJSON_AddNumberToObject(data, "sequence", request->sequence);
    cJSON_AddStringToObject(data,
                            "reason_code",
                            result->reason_code != NULL ? result->reason_code : GW_REASON_INTERNAL_ERROR);
    cJSON_AddNumberToObject(data, "http_status", result->http_status);
    cJSON_AddNumberToObject(data, "esp_err", result->esp_err);
    cJSON_AddItemToObject(root, "data", data);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char topic[GW_PROTOCOL_TOPIC_MAX_LEN];
    esp_err_t err = build_config_topic(topic, sizeof(topic));
    if (err == ESP_OK) {
        err = gw_mqtt_manager_publish(topic, payload, strlen(payload));
    }

    cJSON_free(payload);

    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_config_lock);
        ++s_status.final_publish_failed;
        portEXIT_CRITICAL(&s_config_lock);
    }
    return err;
}

static esp_err_t publish_final_reply_with_retry(const gw_config_update_request_t *request,
                                                const gw_result_t *result,
                                                const char *stage,
                                                gw_config_stage_t failed_stage)
{
    esp_err_t last_err = ESP_FAIL;
    for (uint32_t attempt = 1; attempt <= GW_CONFIG_FINAL_REPLY_RETRY_COUNT; ++attempt) {
        last_err = publish_final_reply(request, result, stage, failed_stage);
        if (last_err == ESP_OK) {
            if (attempt > 1U) {
                ESP_LOGI(TAG,
                         "config final reply published after retry: request_id=%s attempt=%lu",
                         request->request_id,
                         (unsigned long)attempt);
            }
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "config final reply publish failed: request_id=%s attempt=%lu/%lu err=%s",
                 request->request_id,
                 (unsigned long)attempt,
                 (unsigned long)GW_CONFIG_FINAL_REPLY_RETRY_COUNT,
                 esp_err_to_name(last_err));
        if (attempt < GW_CONFIG_FINAL_REPLY_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(GW_CONFIG_FINAL_REPLY_RETRY_DELAY_MS));
        }
    }

    return last_err;
}

static void log_config_update_result(const gw_config_update_request_t *request,
                                     const gw_result_t *result,
                                     gw_config_stage_t final_stage)
{
    if (request == NULL || result == NULL) {
        return;
    }

    char message[128];
    snprintf(message,
             sizeof(message),
             "config update %s, request_id=%s, sequence=%" PRIu32,
             final_stage == GW_CONFIG_STAGE_SUCCESS ? "success" : "failed",
             request->request_id,
             request->sequence);
    gw_log_write(final_stage == GW_CONFIG_STAGE_SUCCESS ? GW_LOG_LEVEL_INFO : GW_LOG_LEVEL_WARN,
                 "config",
                 final_stage == GW_CONFIG_STAGE_SUCCESS ? "update_success" : "update_failed",
                 message,
                 result);
}

static bool version_parse3(const char *value, uint32_t out[3])
{
    if (value == NULL || out == NULL) {
        return false;
    }

    int major = -1;
    int minor = -1;
    int patch = -1;
    char tail = '\0';
    if (sscanf(value, "%d.%d.%d%c", &major, &minor, &patch, &tail) != 3) {
        return false;
    }

    if (major < 0 || major > 255 || minor < 0 || minor > 255 || patch < 0 || patch > 255) {
        return false;
    }

    out[0] = (uint32_t)major;
    out[1] = (uint32_t)minor;
    out[2] = (uint32_t)patch;
    return true;
}

static bool min_product_version_met(const char *min_version)
{
    uint32_t min_v[3];
    uint32_t cur_v[3];
    if (!version_parse3(min_version, min_v) || !version_parse3(gw_version_get_string(), cur_v)) {
        return false;
    }

    for (size_t i = 0; i < 3; ++i) {
        if (cur_v[i] > min_v[i]) {
            return true;
        }
        if (cur_v[i] < min_v[i]) {
            return false;
        }
    }

    return true;
}

static esp_err_t normalize_manifest_name(const char *name, char *out, size_t out_size)
{
    if (!is_nonempty_string(name) || out == NULL || out_size == 0 ||
        name[0] == '/' || strchr(name, '\\') != NULL || strchr(name, ':') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int depth = 0;
    const char *segments[32] = {0};
    char temp[GW_FS_MAX_PATH_LEN];
    if (strlcpy(temp, name, sizeof(temp)) >= sizeof(temp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *saveptr = NULL;
    char *token = strtok_r(temp, "/", &saveptr);
    while (token != NULL) {
        if (token[0] == '\0' || strcmp(token, ".") == 0) {
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (depth == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            --depth;
        } else {
            for (const unsigned char *p = (const unsigned char *)token; *p != '\0'; ++p) {
                if (*p < 0x20) {
                    return ESP_ERR_INVALID_ARG;
                }
            }
            if ((size_t)depth >= sizeof(segments) / sizeof(segments[0])) {
                return ESP_ERR_INVALID_SIZE;
            }
            segments[depth] = token;
            ++depth;
        }
        token = strtok_r(NULL, "/", &saveptr);
    }

    if (depth <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    for (int i = 0; i < depth; ++i) {
        if (i > 0 && strlcat(out, "/", out_size) >= out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (strlcat(out, segments[i], out_size) >= out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return ESP_OK;
}

static bool manifest_name_duplicate(const gw_manifest_info_t *info, const char *name)
{
    if (info == NULL || name == NULL) {
        return false;
    }

    for (uint32_t i = 0; i < info->parsed_file_count; ++i) {
        if (strcmp(info->files[i].name, name) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t parse_manifest_text(const char *text,
                                     size_t text_len,
                                     gw_manifest_info_t *out,
                                     gw_result_t *out_result)
{
    if (text == NULL || out == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                  GW_REASON_INVALID_MANIFEST,
                                  "invalid manifest");

    char *copy = (char *)malloc(text_len + 1U);
    if (copy == NULL) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_INTERNAL_ERROR,
                                      "manifest parse no memory");
        return ESP_ERR_NO_MEM;
    }

    memcpy(copy, text, text_len);
    copy[text_len] = '\0';

    bool desc_seen = false;
    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);
    while (line != NULL) {
        while (*line == '\r' || *line == ' ' || *line == '\t') {
            ++line;
        }

        size_t line_len = strlen(line);
        while (line_len > 0 && (line[line_len - 1U] == '\r' ||
                                line[line_len - 1U] == ' ' ||
                                line[line_len - 1U] == '\t')) {
            line[--line_len] = '\0';
        }

        if (line[0] == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        cJSON *obj = cJSON_Parse(line);
        if (!cJSON_IsObject(obj)) {
            cJSON_Delete(obj);
            free(copy);
            return ESP_ERR_INVALID_RESPONSE;
        }

        cJSON *type = cJSON_GetObjectItemCaseSensitive(obj, "type");
        if (!cJSON_IsString(type) || type->valuestring == NULL) {
            cJSON_Delete(obj);
            free(copy);
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (strcmp(type->valuestring, "desc") == 0) {
            cJSON *seq = cJSON_GetObjectItemCaseSensitive(obj, "seq_id");
            cJSON *min_ver = cJSON_GetObjectItemCaseSensitive(obj, "min_product_version");
            cJSON *file_count = cJSON_GetObjectItemCaseSensitive(obj, "file_count");
            if (desc_seen || !cJSON_IsNumber(seq) || !cJSON_IsString(min_ver) ||
                !cJSON_IsNumber(file_count) ||
                seq->valuedouble < 0 ||
                seq->valuedouble > UINT32_MAX ||
                file_count->valuedouble < 0 ||
                file_count->valuedouble > GW_CONFIG_MANIFEST_MAX_FILES ||
                (double)(uint32_t)seq->valuedouble != seq->valuedouble ||
                (double)(uint32_t)file_count->valuedouble != file_count->valuedouble) {
                cJSON_Delete(obj);
                free(copy);
                return ESP_ERR_INVALID_RESPONSE;
            }
            desc_seen = true;
            out->seq_id = (uint32_t)seq->valuedouble;
            out->file_count = (uint32_t)file_count->valuedouble;
            strlcpy(out->min_product_version, min_ver->valuestring, sizeof(out->min_product_version));
        } else if (strcmp(type->valuestring, "file") == 0) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(obj, "name");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(obj, "size");
            cJSON *sha = cJSON_GetObjectItemCaseSensitive(obj, "sha256");
            char normalized_name[GW_FS_MAX_PATH_LEN];
            if (!cJSON_IsString(name) || !cJSON_IsNumber(size) ||
                !cJSON_IsString(sha) ||
                normalize_manifest_name(name->valuestring, normalized_name, sizeof(normalized_name)) != ESP_OK ||
                manifest_name_duplicate(out, normalized_name) ||
                !is_sha256_hex(sha->valuestring) ||
                size->valuedouble < 0 ||
                size->valuedouble > GW_CONFIG_SLOT_MAX_BYTES ||
                (double)(uint32_t)size->valuedouble != size->valuedouble ||
                out->parsed_file_count >= GW_CONFIG_MANIFEST_MAX_FILES) {
                cJSON_Delete(obj);
                free(copy);
                return ESP_ERR_INVALID_RESPONSE;
            }

            if (strcmp(normalized_name, "config.json") == 0) {
                out->has_config_json = true;
            }
            gw_manifest_file_t *file = &out->files[out->parsed_file_count];
            strlcpy(file->name, normalized_name, sizeof(file->name));
            file->size = (uint32_t)size->valuedouble;
            strlcpy(file->sha256, sha->valuestring, sizeof(file->sha256));
            normalize_sha256(file->sha256);

            ++out->parsed_file_count;
            out->total_file_size += (uint32_t)size->valuedouble;
            if (out->total_file_size > GW_CONFIG_SLOT_MAX_BYTES) {
                cJSON_Delete(obj);
                free(copy);
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            cJSON_Delete(obj);
            free(copy);
            return ESP_ERR_INVALID_RESPONSE;
        }

        cJSON_Delete(obj);
        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(copy);

    if (!desc_seen ||
        out->file_count != out->parsed_file_count ||
        !out->has_config_json ||
        out->total_file_size > GW_CONFIG_SLOT_MAX_BYTES) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!min_product_version_met(out->min_product_version)) {
        *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                      GW_REASON_MIN_PRODUCT_VERSION,
                                      "min product version not met");
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_result = gw_result_ok();
    return ESP_OK;
}

static gw_result_t result_from_http_response(const gw_http_response_t *response, esp_err_t err)
{
    if (response != NULL && response->result.code != GW_RESULT_OK) {
        gw_result_t result = response->result;
        if (result.reason_code == NULL) {
            result.reason_code = GW_REASON_HTTP_ERROR;
        }
        result.esp_err = err;
        return result;
    }

    gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                         GW_REASON_HTTP_ERROR,
                                         "http request failed");
    result.esp_err = err;
    return result;
}

static gw_result_t result_from_fs_error(esp_err_t err, const char *detail)
{
    bool bad_manifest = (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE);
    gw_result_t result = gw_result_error(bad_manifest ? GW_RESULT_INVALID_FIELD : GW_RESULT_STORAGE_FULL,
                                         bad_manifest ? GW_REASON_INVALID_MANIFEST : GW_REASON_STORAGE_FULL,
                                         detail);
    result.esp_err = err;
    return result;
}

static gw_result_t download_buffer_checked(const char *url,
                                           size_t expected_size,
                                           const char *expected_sha256,
                                           char **out_buf,
                                           size_t *out_len)
{
    if (!is_nonempty_string(url) || !is_nonempty_string(expected_sha256) ||
        out_buf == NULL || out_len == NULL) {
        return gw_result_error(GW_RESULT_INVALID_FIELD,
                               GW_REASON_INVALID_FIELD,
                               "invalid download args");
    }

    *out_buf = NULL;
    *out_len = 0;

    char *buffer = (char *)malloc(expected_size + 1U);
    if (buffer == NULL) {
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_INTERNAL_ERROR,
                               "download buffer no memory");
    }

    gw_http_response_t response;
    gw_http_request_t http_req = {
        .method = GW_HTTP_METHOD_GET,
        .url = url,
        .response_buf = buffer,
        .response_buf_size = expected_size + 1U,
        .connect_timeout_sec = GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC,
        .response_interval_timeout_sec = GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC,
    };

    esp_err_t err = perform_http_with_retries(&http_req, &response);
    if (err != ESP_OK) {
        gw_result_t result = result_from_http_response(&response, err);
        free(buffer);
        return result;
    }

    if (response.response_len != expected_size || response.response_truncated) {
        gw_result_t result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                             GW_REASON_SIZE_MISMATCH,
                                             "download size mismatch");
        result.http_status = response.http_status;
        free(buffer);
        return result;
    }

    char calculated_sha[65];
    err = sha256_buffer_hex(buffer, response.response_len, calculated_sha);
    if (err != ESP_OK) {
        gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                             GW_REASON_INTERNAL_ERROR,
                                             "download sha failed");
        result.esp_err = err;
        free(buffer);
        return result;
    }

    if (strcmp(calculated_sha, expected_sha256) != 0) {
        gw_result_t result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                             GW_REASON_SHA256_MISMATCH,
                                             "download sha mismatch");
        result.http_status = response.http_status;
        free(buffer);
        return result;
    }

    *out_buf = buffer;
    *out_len = response.response_len;
    return gw_result_ok();
}

static gw_result_t download_config_files(const gw_config_update_request_t *request,
                                         const char *target_slot,
                                         const gw_manifest_info_t *manifest)
{
    if (request == NULL || !is_nonempty_string(target_slot) || manifest == NULL) {
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_INTERNAL_ERROR,
                               "invalid config file download args");
    }

    for (uint32_t i = 0; i < manifest->parsed_file_count; ++i) {
        const gw_manifest_file_t *file = &manifest->files[i];
        char url[GW_HTTP_MAX_URL_LEN];
        esp_err_t err = build_file_url(request->manifest_url, file->name, url, sizeof(url));
        if (err != ESP_OK) {
            gw_result_t result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                                 GW_REASON_INVALID_MANIFEST,
                                                 "config file url too long");
            result.esp_err = err;
            return result;
        }

        char redacted_url[GW_HTTP_MAX_URL_LEN];
        if (gw_http_redact_url(url, redacted_url, sizeof(redacted_url)) != ESP_OK) {
            strlcpy(redacted_url, "<url-too-long>", sizeof(redacted_url));
        }
        ESP_LOGI(TAG,
                 "config file download start: request_id=%s name=%s size=%lu url=%s",
                 request->request_id,
                 file->name,
                 (unsigned long)file->size,
                 redacted_url);

        char *payload = NULL;
        size_t payload_len = 0;
        gw_result_t result = download_buffer_checked(url,
                                                     file->size,
                                                     file->sha256,
                                                     &payload,
                                                     &payload_len);
        if (result.code != GW_RESULT_OK) {
            ESP_LOGW(TAG,
                     "config file download failed: request_id=%s name=%s code=%ld reason=%s detail=%s",
                     request->request_id,
                     file->name,
                     (long)result.code,
                     result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR,
                     result.detail != NULL ? result.detail : "");
            return result;
        }

        char tmp_path[GW_FS_MAX_PATH_LEN];
        err = build_slot_file_path(target_slot, file->name, ".tmp", tmp_path, sizeof(tmp_path));
        if (err == ESP_OK) {
            err = write_binary_file(tmp_path, payload, payload_len);
        }
        free(payload);
        if (err != ESP_OK) {
            return result_from_fs_error(err, "write config temp file failed");
        }

        ESP_LOGI(TAG,
                 "config file download done: request_id=%s name=%s size=%lu",
                 request->request_id,
                 file->name,
                 (unsigned long)payload_len);
    }

    return gw_result_ok();
}

static gw_result_t commit_config_files(const char *target_slot,
                                       const gw_manifest_info_t *manifest,
                                       const char *manifest_text,
                                       size_t manifest_len)
{
    if (!is_nonempty_string(target_slot) || manifest == NULL || manifest_text == NULL) {
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_INTERNAL_ERROR,
                               "invalid config commit args");
    }

    for (uint32_t i = 0; i < manifest->parsed_file_count; ++i) {
        const gw_manifest_file_t *file = &manifest->files[i];
        char tmp_path[GW_FS_MAX_PATH_LEN];
        char final_path[GW_FS_MAX_PATH_LEN];
        esp_err_t err = build_slot_file_path(target_slot, file->name, ".tmp", tmp_path, sizeof(tmp_path));
        if (err == ESP_OK) {
            err = build_slot_file_path(target_slot, file->name, NULL, final_path, sizeof(final_path));
        }
        if (err == ESP_OK) {
            err = commit_temp_file(tmp_path, final_path);
        }
        if (err != ESP_OK) {
            return result_from_fs_error(err, "commit config file failed");
        }
    }

    char manifest_tmp[GW_FS_MAX_PATH_LEN];
    char manifest_final[GW_FS_MAX_PATH_LEN];
    esp_err_t err = build_slot_file_path(target_slot,
                                         "manifest.jsonl",
                                         ".tmp",
                                         manifest_tmp,
                                         sizeof(manifest_tmp));
    if (err == ESP_OK) {
        err = build_slot_file_path(target_slot,
                                   "manifest.jsonl",
                                   NULL,
                                   manifest_final,
                                   sizeof(manifest_final));
    }
    if (err == ESP_OK) {
        err = write_binary_file(manifest_tmp, manifest_text, manifest_len);
    }
    if (err == ESP_OK) {
        err = commit_temp_file(manifest_tmp, manifest_final);
    }
    if (err != ESP_OK) {
        return result_from_fs_error(err, "commit manifest failed");
    }

    return gw_result_ok();
}

static void cleanup_config_temps(const char *target_slot, const gw_manifest_info_t *manifest)
{
    if (!is_nonempty_string(target_slot)) {
        return;
    }

    char path[GW_FS_MAX_PATH_LEN];
    if (build_slot_file_path(target_slot, "manifest.jsonl", ".tmp", path, sizeof(path)) == ESP_OK) {
        (void)remove(path);
    }

    if (manifest == NULL) {
        return;
    }

    for (uint32_t i = 0; i < manifest->parsed_file_count; ++i) {
        if (build_slot_file_path(target_slot, manifest->files[i].name, ".tmp", path, sizeof(path)) == ESP_OK) {
            (void)remove(path);
        }
    }
}

static gw_result_t validate_slot_files(const char *target_slot,
                                       uint32_t expected_sequence,
                                       const char *expected_manifest_sha256,
                                       gw_manifest_info_t *out_manifest)
{
    if (!is_nonempty_string(target_slot) || !is_nonempty_string(expected_manifest_sha256) ||
        out_manifest == NULL) {
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_INTERNAL_ERROR,
                               "invalid slot validation args");
    }

    char manifest_path[GW_FS_MAX_PATH_LEN];
    esp_err_t err = build_slot_file_path(target_slot,
                                         "manifest.jsonl",
                                         NULL,
                                         manifest_path,
                                         sizeof(manifest_path));
    if (err != ESP_OK) {
        return result_from_fs_error(err, "manifest path invalid");
    }

    char *manifest_text = NULL;
    size_t manifest_len = 0;
    err = read_file_alloc(manifest_path, GW_CONFIG_MANIFEST_MAX_BYTES, &manifest_text, &manifest_len);
    if (err != ESP_OK) {
        return result_from_fs_error(err, "read committed manifest failed");
    }

    char manifest_sha[65];
    err = sha256_buffer_hex(manifest_text, manifest_len, manifest_sha);
    if (err != ESP_OK) {
        free(manifest_text);
        gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                             GW_REASON_INTERNAL_ERROR,
                                             "manifest file sha failed");
        result.esp_err = err;
        return result;
    }

    if (strcmp(manifest_sha, expected_manifest_sha256) != 0) {
        free(manifest_text);
        return gw_result_error(GW_RESULT_INVALID_FIELD,
                               GW_REASON_SHA256_MISMATCH,
                               "manifest file sha mismatch");
    }

    gw_result_t parse_result;
    err = parse_manifest_text(manifest_text, manifest_len, out_manifest, &parse_result);
    free(manifest_text);
    if (err != ESP_OK) {
        return parse_result;
    }

    if (out_manifest->seq_id != expected_sequence) {
        return gw_result_error(GW_RESULT_INVALID_FIELD,
                               GW_REASON_INVALID_MANIFEST,
                               "committed manifest sequence mismatch");
    }

    for (uint32_t i = 0; i < out_manifest->parsed_file_count; ++i) {
        const gw_manifest_file_t *file = &out_manifest->files[i];
        char file_path[GW_FS_MAX_PATH_LEN];
        err = build_slot_file_path(target_slot, file->name, NULL, file_path, sizeof(file_path));
        if (err != ESP_OK) {
            return result_from_fs_error(err, "committed file path invalid");
        }

        char file_sha[65];
        err = sha256_file_hex(file_path, file->size, file_sha);
        if (err == ESP_ERR_INVALID_SIZE) {
            return gw_result_error(GW_RESULT_INVALID_FIELD,
                                   GW_REASON_SIZE_MISMATCH,
                                   "committed file size mismatch");
        }
        if (err != ESP_OK) {
            return result_from_fs_error(err, "committed file sha failed");
        }
        if (strcmp(file_sha, file->sha256) != 0) {
            return gw_result_error(GW_RESULT_INVALID_FIELD,
                                   GW_REASON_SHA256_MISMATCH,
                                   "committed file sha mismatch");
        }
    }

    return gw_result_ok();
}

static gw_result_t apply_config_slot(const char *target_slot,
                                     uint32_t sequence,
                                     const char *manifest_sha256)
{
    gw_config_active_t active = {
        .seq_id = sequence,
        .loaded_from_nvs = true,
    };
    strlcpy(active.active_slot, target_slot, sizeof(active.active_slot));
    strlcpy(active.manifest_sha256, manifest_sha256, sizeof(active.manifest_sha256));

    esp_err_t err = save_active_state(&active);
    if (err != ESP_OK) {
        gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                             GW_REASON_INTERNAL_ERROR,
                                             "save active config failed");
        result.esp_err = err;
        return result;
    }

    update_active_status(&active);
    return gw_result_ok();
}

static gw_result_t build_test_manifest(uint32_t sequence,
                                       const gw_config_test_file_t *files,
                                       size_t file_count,
                                       char **out_manifest,
                                       size_t *out_manifest_len,
                                       char out_manifest_sha256[65])
{
    if (files == NULL || file_count == 0 || file_count > GW_CONFIG_MANIFEST_MAX_FILES ||
        out_manifest == NULL || out_manifest_len == NULL || out_manifest_sha256 == NULL) {
        return gw_result_error(GW_RESULT_INVALID_FIELD,
                               GW_REASON_INVALID_FIELD,
                               "invalid test bundle args");
    }

    *out_manifest = NULL;
    *out_manifest_len = 0;
    out_manifest_sha256[0] = '\0';

    char *manifest = (char *)calloc(1, GW_CONFIG_MANIFEST_MAX_BYTES);
    if (manifest == NULL) {
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_INTERNAL_ERROR,
                               "test manifest no memory");
    }

    int written = snprintf(manifest,
                           GW_CONFIG_MANIFEST_MAX_BYTES,
                           "{\"type\":\"desc\",\"seq_id\":%lu,"
                           "\"min_product_version\":\"0.0.0\",\"file_count\":%lu}\n",
                           (unsigned long)sequence,
                           (unsigned long)file_count);
    if (written <= 0 || written >= (int)GW_CONFIG_MANIFEST_MAX_BYTES) {
        free(manifest);
        return gw_result_error(GW_RESULT_INVALID_FIELD,
                               GW_REASON_INVALID_MANIFEST,
                               "test manifest header too large");
    }
    size_t used = (size_t)written;

    for (size_t i = 0; i < file_count; ++i) {
        char normalized_name[GW_FS_MAX_PATH_LEN];
        if (files[i].data == NULL ||
            files[i].size > GW_CONFIG_SLOT_MAX_BYTES ||
            normalize_manifest_name(files[i].name, normalized_name, sizeof(normalized_name)) != ESP_OK ||
            strchr(normalized_name, '"') != NULL) {
            free(manifest);
            return gw_result_error(GW_RESULT_INVALID_FIELD,
                                   GW_REASON_INVALID_MANIFEST,
                                   "invalid test bundle file");
        }

        char file_sha[65];
        esp_err_t err = sha256_buffer_hex(files[i].data, files[i].size, file_sha);
        if (err != ESP_OK) {
            free(manifest);
            gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                                 GW_REASON_INTERNAL_ERROR,
                                                 "test file sha failed");
            result.esp_err = err;
            return result;
        }

        written = snprintf(manifest + used,
                           GW_CONFIG_MANIFEST_MAX_BYTES - used,
                           "{\"type\":\"file\",\"name\":\"%s\",\"size\":%lu,\"sha256\":\"%s\"}\n",
                           normalized_name,
                           (unsigned long)files[i].size,
                           file_sha);
        if (written <= 0 || written >= (int)(GW_CONFIG_MANIFEST_MAX_BYTES - used)) {
            free(manifest);
            return gw_result_error(GW_RESULT_INVALID_FIELD,
                                   GW_REASON_INVALID_MANIFEST,
                                   "test manifest too large");
        }
        used += (size_t)written;
    }

    esp_err_t err = sha256_buffer_hex(manifest, used, out_manifest_sha256);
    if (err != ESP_OK) {
        free(manifest);
        gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                             GW_REASON_INTERNAL_ERROR,
                                             "test manifest sha failed");
        result.esp_err = err;
        return result;
    }

    *out_manifest = manifest;
    *out_manifest_len = used;
    return gw_result_ok();
}

static gw_result_t write_test_bundle_temps(const char *target_slot,
                                           const gw_config_test_file_t *files,
                                           size_t file_count)
{
    if (!is_nonempty_string(target_slot) || files == NULL) {
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_INTERNAL_ERROR,
                               "invalid test temp args");
    }

    for (size_t i = 0; i < file_count; ++i) {
        char normalized_name[GW_FS_MAX_PATH_LEN];
        esp_err_t err = normalize_manifest_name(files[i].name,
                                                normalized_name,
                                                sizeof(normalized_name));
        if (err != ESP_OK) {
            return result_from_fs_error(err, "test file path invalid");
        }

        char tmp_path[GW_FS_MAX_PATH_LEN];
        err = build_slot_file_path(target_slot,
                                   normalized_name,
                                   ".tmp",
                                   tmp_path,
                                   sizeof(tmp_path));
        if (err == ESP_OK) {
            err = write_binary_file(tmp_path, files[i].data, files[i].size);
        }
        if (err != ESP_OK) {
            return result_from_fs_error(err, "write test temp file failed");
        }
    }

    return gw_result_ok();
}

static gw_result_t run_config_update(const gw_config_update_request_t *request)
{
    gw_net_status_snapshot_t net_status;
    if (gw_net_get_status(&net_status) != ESP_OK ||
        net_status.public_status != GW_NETWORK_STATUS_CONNECTED) {
        set_stage(GW_CONFIG_STAGE_WAIT_NETWORK);
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_NETWORK_UNAVAILABLE,
                               "network unavailable");
    }

    set_stage(GW_CONFIG_STAGE_DOWNLOADING_MANIFEST);
    char *manifest_text = NULL;
    size_t manifest_len = 0;
    gw_result_t result = download_buffer_checked(request->manifest_url,
                                                 request->manifest_size,
                                                 request->manifest_sha256,
                                                 &manifest_text,
                                                 &manifest_len);
    if (result.code != GW_RESULT_OK) {
        return result;
    }
    ESP_LOGI(TAG,
             "config manifest downloaded: request_id=%s sequence=%lu size=%lu",
             request->request_id,
             (unsigned long)request->sequence,
             (unsigned long)manifest_len);

    gw_manifest_info_t *manifest = (gw_manifest_info_t *)calloc(1, sizeof(*manifest));
    if (manifest == NULL) {
        free(manifest_text);
        return gw_result_error(GW_RESULT_INTERNAL_ERROR,
                               GW_REASON_INTERNAL_ERROR,
                               "manifest info no memory");
    }

    set_stage(GW_CONFIG_STAGE_VALIDATING_MANIFEST);
    esp_err_t err = parse_manifest_text(manifest_text, manifest_len, manifest, &result);
    if (err != ESP_OK) {
        free(manifest_text);
        free(manifest);
        return result;
    }

    ESP_LOGI(TAG,
             "config manifest parsed: request_id=%s sequence=%lu file_count=%lu total_size=%lu",
             request->request_id,
             (unsigned long)manifest->seq_id,
             (unsigned long)manifest->parsed_file_count,
             (unsigned long)manifest->total_file_size);

    if (manifest->seq_id != request->sequence) {
        free(manifest_text);
        free(manifest);
        return gw_result_error(GW_RESULT_INVALID_FIELD,
                               GW_REASON_INVALID_MANIFEST,
                               "manifest sequence mismatch");
    }

    gw_config_active_t active;
    err = load_active_state(&active);
    if (err != ESP_OK) {
        free(manifest_text);
        free(manifest);
        gw_result_t active_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                                    GW_REASON_INTERNAL_ERROR,
                                                    "load active config failed");
        active_result.esp_err = err;
        return active_result;
    }

    const char *target_slot = inactive_slot_name(active.active_slot);
    ESP_LOGI(TAG,
             "config target slot selected: request_id=%s active=%s target=%s",
             request->request_id,
             active.active_slot,
             target_slot);
    err = prepare_target_slot(target_slot);
    if (err != ESP_OK) {
        free(manifest_text);
        free(manifest);
        return result_from_fs_error(err, "prepare target config slot failed");
    }

    set_stage(GW_CONFIG_STAGE_DOWNLOADING_FILES);
    result = download_config_files(request, target_slot, manifest);
    if (result.code != GW_RESULT_OK) {
        cleanup_config_temps(target_slot, manifest);
        free(manifest_text);
        free(manifest);
        return result;
    }

    set_stage(GW_CONFIG_STAGE_VALIDATING_SLOT);
    result = commit_config_files(target_slot, manifest, manifest_text, manifest_len);
    free(manifest_text);
    if (result.code != GW_RESULT_OK) {
        cleanup_config_temps(target_slot, manifest);
        free(manifest);
        return result;
    }

    memset(manifest, 0, sizeof(*manifest));
    result = validate_slot_files(target_slot,
                                 request->sequence,
                                 request->manifest_sha256,
                                 manifest);
    if (result.code != GW_RESULT_OK) {
        cleanup_config_temps(target_slot, manifest);
        free(manifest);
        return result;
    }

    set_stage(GW_CONFIG_STAGE_SWITCHING_SLOT);
    result = apply_config_slot(target_slot, request->sequence, request->manifest_sha256);
    free(manifest);
    return result;
}

static void config_worker_task(void *arg)
{
    (void)arg;
    gw_config_update_request_t request;

    for (;;) {
        if (xQueueReceive(s_config_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        gw_state_set_task_status(GW_TASK_STATUS_CONFIG_UPDATING);
        gw_result_t result = run_config_update(&request);
        ESP_LOGI(TAG,
                 "config_worker stack high water after update: %lu bytes",
                 (unsigned long)uxTaskGetStackHighWaterMark(NULL));
        gw_config_stage_t result_stage = get_stage_snapshot();
        bool reboot_after_success = result.code == GW_RESULT_OK && request.apply_after_download;
        gw_config_stage_t final_stage = result.code == GW_RESULT_OK ?
                                        GW_CONFIG_STAGE_SUCCESS :
                                        GW_CONFIG_STAGE_FAILED;

        log_config_update_result(&request, &result, final_stage);
        (void)publish_final_reply_with_retry(&request,
                                             &result,
                                             final_stage == GW_CONFIG_STAGE_SUCCESS ? "success" : "failed",
                                             result_stage);
        gw_protocol_task_end(request.request_id,
                             result.code,
                             result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR);
        mark_task_finished(final_stage, &result);

        if (reboot_after_success) {
            gw_reboot_request_t reboot_request = {
                .delay_ms = GW_REBOOT_DEFAULT_DELAY_MS,
                .execute = true,
            };
            strlcpy(reboot_request.request_id, request.request_id, sizeof(reboot_request.request_id));
            strlcpy(reboot_request.reason,
                    GW_REBOOT_REASON_CONFIG_UPDATED,
                    sizeof(reboot_request.reason));

            gw_result_t reboot_result;
            esp_err_t reboot_err = gw_reboot_manager_request(&reboot_request, &reboot_result);
            if (reboot_err != ESP_OK) {
                gw_log_write(GW_LOG_LEVEL_WARN,
                             "config",
                             "reboot_request_failed",
                             "config update succeeded but reboot request failed",
                             &reboot_result);
            }
        }
    }
}

static esp_err_t validate_update_request(const gw_config_update_request_t *request,
                                         gw_result_t *out_result,
                                         gw_config_active_t *active)
{
    if (request == NULL || out_result == NULL || active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                  GW_REASON_INVALID_FIELD,
                                  "invalid config update request");

    if (!is_nonempty_string(request->request_id) ||
        strlen(request->request_id) > GW_REQUEST_ID_MAX_LEN ||
        !is_nonempty_string(request->manifest_url) ||
        !gw_http_url_scheme_allowed(request->manifest_url) ||
        !is_sha256_hex(request->manifest_sha256) ||
        request->manifest_size == 0 ||
        request->manifest_size > GW_CONFIG_MANIFEST_MAX_BYTES ||
        request->sequence == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_fs_status_t fs_status;
    if (gw_fs_get_status(&fs_status) != ESP_OK || !fs_status.cfgfs.mounted) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_INTERNAL_ERROR,
                                      "cfgfs unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(load_active_state(active), TAG, "load active config failed");
    if (request->sequence <= active->seq_id) {
        *out_result = gw_result_error(GW_RESULT_VERSION_NOT_INCREASED,
                                      GW_REASON_VERSION_NOT_INCREASED,
                                      "config sequence not increased");
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = gw_result_ok();
    return ESP_OK;
}

esp_err_t gw_config_manager_init(void)
{
    if (s_config_task_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gw_storage_get_identity(&s_identity), TAG, "load identity failed");

    gw_config_active_t active;
    ESP_RETURN_ON_ERROR(load_active_state(&active), TAG, "load active config failed");

    if (s_config_queue == NULL) {
        s_config_queue = xQueueCreate(GW_CONFIG_UPDATE_QUEUE_LEN, sizeof(gw_config_update_request_t));
        if (s_config_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.stage = GW_CONFIG_STAGE_IDLE;
    s_status.active = active;
    strlcpy(s_status.last_reason, GW_REASON_OK, sizeof(s_status.last_reason));
    portENTER_CRITICAL(&s_config_lock);
    s_pending_update_valid = false;
    memset(&s_pending_update, 0, sizeof(s_pending_update));
    portEXIT_CRITICAL(&s_config_lock);

    BaseType_t ok = xTaskCreate(config_worker_task,
                                "config_task",
                                GW_TASK_STACK_CONFIG_BYTES,
                                NULL,
                                7,
                                &s_config_task_handle);
    if (ok != pdPASS) {
        s_config_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_config_lock);
    s_status.worker_started = true;
    portEXIT_CRITICAL(&s_config_lock);

    return ESP_OK;
}

esp_err_t gw_config_manager_submit_update(const gw_config_update_request_t *request,
                                          gw_result_t *out_result)
{
    if (request == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_config_queue == NULL || s_config_task_handle == NULL) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_NOT_INITIALIZED,
                                      "config manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    gw_config_update_request_t copy = *request;
    normalize_sha256(copy.manifest_sha256);

    gw_config_active_t active;
    esp_err_t err = validate_update_request(&copy, out_result, &active);
    if (err != ESP_OK) {
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return err;
    }

    err = gw_protocol_task_begin("config.update",
                                 copy.request_id,
                                 GW_TASK_STATUS_CONFIG_UPDATING);
    if (err != ESP_OK) {
        *out_result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                      GW_REASON_DEVICE_BUSY,
                                      "config task busy");
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return err;
    }

    mark_task_started(&copy);

    portENTER_CRITICAL(&s_config_lock);
    if (s_pending_update_valid) {
        portEXIT_CRITICAL(&s_config_lock);
        gw_protocol_task_end(copy.request_id, GW_RESULT_DEVICE_BUSY, GW_REASON_QUEUE_FULL);
        *out_result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                      GW_REASON_QUEUE_FULL,
                                      "config queue full");
        mark_task_finished(GW_CONFIG_STAGE_FAILED, out_result);
        return ESP_ERR_INVALID_STATE;
    }
    s_pending_update = copy;
    s_pending_update_valid = true;
    portEXIT_CRITICAL(&s_config_lock);

    *out_result = gw_result_error(GW_RESULT_OK, GW_REASON_ACCEPTED, "accepted");
    return ESP_OK;
}

bool gw_config_manager_has_pending_update(void)
{
    bool pending = false;
    portENTER_CRITICAL(&s_config_lock);
    pending = s_pending_update_valid;
    portEXIT_CRITICAL(&s_config_lock);
    return pending;
}

esp_err_t gw_config_manager_start_pending_update(void)
{
    if (s_config_queue == NULL || s_config_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_config_update_request_t request;
    portENTER_CRITICAL(&s_config_lock);
    if (!s_pending_update_valid) {
        portEXIT_CRITICAL(&s_config_lock);
        return ESP_ERR_NOT_FOUND;
    }
    request = s_pending_update;
    memset(&s_pending_update, 0, sizeof(s_pending_update));
    s_pending_update_valid = false;
    portEXIT_CRITICAL(&s_config_lock);

    if (xQueueSend(s_config_queue, &request, 0) == pdPASS) {
        return ESP_OK;
    }

    gw_result_t result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                         GW_REASON_QUEUE_FULL,
                                         "config queue full");
    gw_protocol_task_end(request.request_id,
                         result.code,
                         result.reason_code != NULL ? result.reason_code : GW_REASON_QUEUE_FULL);
    mark_task_finished(GW_CONFIG_STAGE_FAILED, &result);
    (void)publish_final_reply_with_retry(&request,
                                         &result,
                                         "failed",
                                         GW_CONFIG_STAGE_ACCEPTED);
    return ESP_ERR_TIMEOUT;
}

void gw_config_manager_cancel_pending_update(const char *reason)
{
    gw_config_update_request_t request;
    bool had_pending = false;

    portENTER_CRITICAL(&s_config_lock);
    if (s_pending_update_valid) {
        request = s_pending_update;
        memset(&s_pending_update, 0, sizeof(s_pending_update));
        s_pending_update_valid = false;
        had_pending = true;
    }
    portEXIT_CRITICAL(&s_config_lock);

    if (!had_pending) {
        return;
    }

    const char *final_reason = reason != NULL ? reason : GW_REASON_INTERNAL_ERROR;
    gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                         final_reason,
                                         "accepted reply publish failed");
    gw_protocol_task_end(request.request_id, result.code, result.reason_code);
    mark_task_finished(GW_CONFIG_STAGE_FAILED, &result);
}

esp_err_t gw_config_manager_get_active(gw_config_active_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return load_active_state(out);
}

esp_err_t gw_config_manager_get_status(gw_config_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_config_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_config_lock);
    return ESP_OK;
}

esp_err_t gw_config_manager_validate_active(gw_result_t *out_result)
{
    if (out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                  GW_REASON_INTERNAL_ERROR,
                                  "active config validation failed");

    gw_config_active_t active;
    esp_err_t err = load_active_state(&active);
    if (err != ESP_OK) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_INTERNAL_ERROR,
                                      "load active config failed");
        out_result->esp_err = err;
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return err;
    }

    if (active.seq_id == 0 || !is_sha256_hex(active.manifest_sha256)) {
        *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                      GW_REASON_INVALID_MANIFEST,
                                      "active config is empty");
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_INVALID_STATE;
    }

    gw_manifest_info_t *manifest = (gw_manifest_info_t *)calloc(1, sizeof(*manifest));
    if (manifest == NULL) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_INTERNAL_ERROR,
                                      "manifest info no memory");
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_NO_MEM;
    }

    *out_result = validate_slot_files(active.active_slot,
                                      active.seq_id,
                                      active.manifest_sha256,
                                      manifest);
    free(manifest);

    update_last_result(out_result->code,
                       out_result->reason_code,
                       out_result->http_status,
                       out_result->esp_err);
    return out_result->code == GW_RESULT_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_config_manager_install_test_bundle(uint32_t sequence,
                                                const gw_config_test_file_t *files,
                                                size_t file_count,
                                                gw_result_t *out_result)
{
    if (out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                  GW_REASON_INVALID_FIELD,
                                  "invalid test bundle request");

    if (files == NULL || file_count == 0 || sequence == 0) {
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_config_task_handle == NULL) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_NOT_INITIALIZED,
                                      "config manager not initialized");
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_INVALID_STATE;
    }

    gw_fs_status_t fs_status;
    if (gw_fs_get_status(&fs_status) != ESP_OK || !fs_status.cfgfs.mounted) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_INTERNAL_ERROR,
                                      "cfgfs unavailable");
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_INVALID_STATE;
    }

    bool busy = false;
    portENTER_CRITICAL(&s_config_lock);
    busy = s_status.running_request_id[0] != '\0';
    portEXIT_CRITICAL(&s_config_lock);
    if (busy) {
        *out_result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                      GW_REASON_DEVICE_BUSY,
                                      "config task busy");
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_INVALID_STATE;
    }

    gw_config_active_t active;
    esp_err_t err = load_active_state(&active);
    if (err != ESP_OK) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_INTERNAL_ERROR,
                                      "load active config failed");
        out_result->esp_err = err;
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return err;
    }

    if (sequence <= active.seq_id) {
        *out_result = gw_result_error(GW_RESULT_VERSION_NOT_INCREASED,
                                      GW_REASON_VERSION_NOT_INCREASED,
                                      "config sequence not increased");
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_INVALID_ARG;
    }

    char *manifest_text = NULL;
    size_t manifest_len = 0;
    char manifest_sha[65];
    gw_result_t result = build_test_manifest(sequence,
                                             files,
                                             file_count,
                                             &manifest_text,
                                             &manifest_len,
                                             manifest_sha);
    if (result.code != GW_RESULT_OK) {
        *out_result = result;
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_INVALID_ARG;
    }

    gw_config_update_request_t request = {
        .sequence = sequence,
        .manifest_size = (uint32_t)manifest_len,
        .apply_after_download = false,
    };
    strlcpy(request.request_id, "p12-local-config", sizeof(request.request_id));
    strlcpy(request.manifest_sha256, manifest_sha, sizeof(request.manifest_sha256));
    strlcpy(request.manifest_url, "self-test://manifest.jsonl", sizeof(request.manifest_url));

    gw_manifest_info_t *manifest = (gw_manifest_info_t *)calloc(1, sizeof(*manifest));
    if (manifest == NULL) {
        free(manifest_text);
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_INTERNAL_ERROR,
                                      "manifest info no memory");
        update_last_result(out_result->code,
                           out_result->reason_code,
                           out_result->http_status,
                           out_result->esp_err);
        return ESP_ERR_NO_MEM;
    }

    mark_task_started(&request);
    set_stage(GW_CONFIG_STAGE_VALIDATING_MANIFEST);
    err = parse_manifest_text(manifest_text, manifest_len, manifest, &result);
    if (err == ESP_OK && manifest->seq_id != sequence) {
        result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                 GW_REASON_INVALID_MANIFEST,
                                 "test manifest sequence mismatch");
        err = ESP_ERR_INVALID_RESPONSE;
    }

    const char *target_slot = inactive_slot_name(active.active_slot);
    if (err == ESP_OK) {
        err = prepare_target_slot(target_slot);
        if (err != ESP_OK) {
            result = result_from_fs_error(err, "prepare target config slot failed");
        }
    }

    if (err == ESP_OK) {
        set_stage(GW_CONFIG_STAGE_DOWNLOADING_FILES);
        result = write_test_bundle_temps(target_slot, files, file_count);
        if (result.code != GW_RESULT_OK) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        set_stage(GW_CONFIG_STAGE_VALIDATING_SLOT);
        result = commit_config_files(target_slot, manifest, manifest_text, manifest_len);
        if (result.code != GW_RESULT_OK) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        memset(manifest, 0, sizeof(*manifest));
        result = validate_slot_files(target_slot, sequence, manifest_sha, manifest);
        if (result.code != GW_RESULT_OK) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        set_stage(GW_CONFIG_STAGE_SWITCHING_SLOT);
        result = apply_config_slot(target_slot, sequence, manifest_sha);
        if (result.code != GW_RESULT_OK) {
            err = ESP_FAIL;
        }
    }

    if (err != ESP_OK) {
        cleanup_config_temps(target_slot, manifest);
    }

    free(manifest_text);
    free(manifest);

    gw_config_stage_t final_stage = result.code == GW_RESULT_OK ?
                                    GW_CONFIG_STAGE_SUCCESS :
                                    GW_CONFIG_STAGE_FAILED;
    mark_task_finished(final_stage, &result);
    *out_result = result;

    return result.code == GW_RESULT_OK ? ESP_OK : ESP_FAIL;
}

#ifdef GW_CONFIG_MANAGER_ENABLE_TEST_HOOKS
void gw_config_manager_test_reset(void)
{
    /*
     * 单元测试需要在同一进程内反复初始化模块。正式固件不会编译该入口，
     * 测试侧通过它清理 worker、队列和静态状态，避免用例之间互相污染。
     */
    if (s_config_task_handle != NULL) {
        vTaskDelete(s_config_task_handle);
        s_config_task_handle = NULL;
    }

    if (s_config_queue != NULL) {
        vQueueDelete(s_config_queue);
        s_config_queue = NULL;
    }

    portENTER_CRITICAL(&s_config_lock);
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_identity, 0, sizeof(s_identity));
    memset(&s_pending_update, 0, sizeof(s_pending_update));
    s_pending_update_valid = false;
    portEXIT_CRITICAL(&s_config_lock);
}

esp_err_t gw_config_manager_test_normalize_manifest_name(const char *name,
                                                         char *out,
                                                         size_t out_size)
{
    return normalize_manifest_name(name, out, out_size);
}

esp_err_t gw_config_manager_test_build_file_url(const char *manifest_url,
                                                const char *name,
                                                char *out,
                                                size_t out_size)
{
    return build_file_url(manifest_url, name, out, out_size);
}

esp_err_t gw_config_manager_test_parse_manifest_summary(
    const char *text,
    size_t text_len,
    gw_config_manager_test_manifest_summary_t *out,
    gw_result_t *out_result)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_manifest_info_t *manifest = (gw_manifest_info_t *)calloc(1, sizeof(*manifest));
    if (manifest == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = parse_manifest_text(text, text_len, manifest, out_result);
    if (err != ESP_OK) {
        free(manifest);
        return err;
    }

    memset(out, 0, sizeof(*out));
    out->seq_id = manifest->seq_id;
    out->file_count = manifest->file_count;
    out->parsed_file_count = manifest->parsed_file_count;
    out->total_file_size = manifest->total_file_size;
    out->has_config_json = manifest->has_config_json;
    strlcpy(out->min_product_version,
            manifest->min_product_version,
            sizeof(out->min_product_version));
    if (manifest->parsed_file_count > 0U) {
        strlcpy(out->first_name, manifest->files[0].name, sizeof(out->first_name));
    }
    if (manifest->parsed_file_count > 1U) {
        strlcpy(out->second_name, manifest->files[1].name, sizeof(out->second_name));
    }
    free(manifest);
    return ESP_OK;
}
#endif

const char *gw_config_stage_to_string(gw_config_stage_t stage)
{
    switch (stage) {
    case GW_CONFIG_STAGE_IDLE:
        return "idle";
    case GW_CONFIG_STAGE_ACCEPTED:
        return "accepted";
    case GW_CONFIG_STAGE_WAIT_NETWORK:
        return "wait_network";
    case GW_CONFIG_STAGE_DOWNLOADING_MANIFEST:
        return "downloading_manifest";
    case GW_CONFIG_STAGE_VALIDATING_MANIFEST:
        return "validating_manifest";
    case GW_CONFIG_STAGE_DOWNLOADING_FILES:
        return "downloading_files";
    case GW_CONFIG_STAGE_VALIDATING_SLOT:
        return "validating_slot";
    case GW_CONFIG_STAGE_SWITCHING_SLOT:
        return "switching_slot";
    case GW_CONFIG_STAGE_SUCCESS:
        return "success";
    case GW_CONFIG_STAGE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
