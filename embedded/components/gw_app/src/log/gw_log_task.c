#include "gw_log_task.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_fs.h"
#include "gw_health.h"
#include "gw_log.h"
#include "gw_state.h"

#define GW_LOG_MAX_SEGMENTS_SCAN      64U
#define GW_LOG_INDEX_UPDATE_EVERY     16U

static const char *TAG = "gw_log_task";
static TaskHandle_t s_log_task_handle;

typedef struct {
    uint32_t id;
    char path[GW_FS_MAX_PATH_LEN];
    size_t size;
} gw_log_segment_info_t;

typedef struct {
    bool logfs_ready;
    uint32_t current_segment_id;
    size_t current_segment_size;
    uint32_t writes_since_index;
} gw_log_writer_state_t;

static bool parse_segment_name(const char *name, uint32_t *out_id)
{
    unsigned long id = 0;
    char tail[8] = {0};

    if (name == NULL || sscanf(name, "log_%lu.%7s", &id, tail) != 2) {
        return false;
    }

    if (strcmp(tail, "jsonl") != 0 || id == 0 || id > UINT32_MAX) {
        return false;
    }

    if (out_id != NULL) {
        *out_id = (uint32_t)id;
    }

    return true;
}

static void sort_segments(gw_log_segment_info_t *segments, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1U; j < count; ++j) {
            if (segments[j].id < segments[i].id) {
                gw_log_segment_info_t temp = segments[i];
                segments[i] = segments[j];
                segments[j] = temp;
            }
        }
    }
}

static bool build_logfs_child_path(const char *name, char *out, size_t out_size)
{
    if (name == NULL || out == NULL || out_size == 0) {
        return false;
    }

    size_t base_len = strlen(GW_FS_LOGFS_BASE_PATH);
    size_t name_len = strlen(name);
    if (base_len + 1U + name_len + 1U > out_size) {
        return false;
    }

    memcpy(out, GW_FS_LOGFS_BASE_PATH, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1U, name, name_len + 1U);
    return true;
}

static esp_err_t build_segment_path(uint32_t segment_id, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "%s/log_%06lu.jsonl",
                           GW_FS_LOGFS_BASE_PATH,
                           (unsigned long)segment_id);
    return (written > 0 && written < (int)out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t scan_segments(gw_log_segment_info_t *segments,
                               size_t max_segments,
                               size_t *out_count,
                               size_t *out_total_size)
{
    if (segments == NULL || out_count == NULL || out_total_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(GW_FS_LOGFS_BASE_PATH);
    if (dir == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t count = 0;
    size_t total_size = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        uint32_t id = 0;
        if (!parse_segment_name(entry->d_name, &id)) {
            continue;
        }

        char path[GW_FS_MAX_PATH_LEN];
        if (!build_logfs_child_path(entry->d_name, path, sizeof(path))) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        total_size += (size_t)st.st_size;
        if (count < max_segments) {
            segments[count].id = id;
            segments[count].size = (size_t)st.st_size;
            strlcpy(segments[count].path, path, sizeof(segments[count].path));
            ++count;
        }
    }

    closedir(dir);
    sort_segments(segments, count);
    *out_count = count;
    *out_total_size = total_size;
    return ESP_OK;
}

static esp_err_t write_index(const gw_log_writer_state_t *state)
{
    if (state == NULL || !state->logfs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total = 0;
    size_t used = 0;
    (void)esp_littlefs_info(GW_FS_LOGFS_LABEL, &total, &used);

    const char *tmp_path = GW_FS_LOGFS_BASE_PATH "/index.tmp";
    const char *index_path = GW_FS_LOGFS_BASE_PATH "/index.json";
    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    int rc = fprintf(fp,
                     "{\"current_segment_id\":%lu,\"current_segment_size\":%lu,"
                     "\"fs_total\":%lu,\"fs_used\":%lu}\n",
                     (unsigned long)state->current_segment_id,
                     (unsigned long)state->current_segment_size,
                     (unsigned long)total,
                     (unsigned long)used);
    int close_rc = fclose(fp);
    if (rc < 0 || close_rc != 0) {
        (void)unlink(tmp_path);
        return ESP_FAIL;
    }

    (void)unlink(index_path);
    if (rename(tmp_path, index_path) != 0) {
        (void)unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cleanup_logs_to_target(gw_log_writer_state_t *state, bool force)
{
    if (state == NULL || !state->logfs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_log_segment_info_t *segments = (gw_log_segment_info_t *)calloc(GW_LOG_MAX_SEGMENTS_SCAN,
                                                                      sizeof(gw_log_segment_info_t));
    if (segments == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    size_t log_size = 0;
    size_t fs_total = 0;
    size_t fs_used = 0;

    esp_err_t err = scan_segments(segments, GW_LOG_MAX_SEGMENTS_SCAN, &count, &log_size);
    if (err != ESP_OK) {
        free(segments);
        return err;
    }

    err = esp_littlefs_info(GW_FS_LOGFS_LABEL, &fs_total, &fs_used);
    if (err != ESP_OK) {
        free(segments);
        return err;
    }

    bool over_high_watermark = fs_total > 0 &&
                               fs_used * 100U > fs_total * GW_LOG_HIGH_WATERMARK_PERCENT;
    if (!force && !over_high_watermark && log_size <= GW_LOG_TARGET_SIZE) {
        free(segments);
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "log cleanup start: count=%lu log_size=%lu fs_used=%lu fs_total=%lu",
             (unsigned long)count,
             (unsigned long)log_size,
             (unsigned long)fs_used,
             (unsigned long)fs_total);

    for (size_t i = 0; i < count; ++i) {
        bool fs_over_target = fs_total > 0 &&
                              fs_used * 100U > fs_total * GW_LOG_CLEAN_TO_PERCENT;
        if (!fs_over_target && log_size <= GW_LOG_TARGET_SIZE) {
            break;
        }

        if (unlink(segments[i].path) != 0) {
            ESP_LOGW(TAG, "delete old log segment failed: %s", segments[i].path);
            continue;
        }

        if (segments[i].id == state->current_segment_id) {
            state->current_segment_id++;
            state->current_segment_size = 0;
        }

        if (log_size >= segments[i].size) {
            log_size -= segments[i].size;
        } else {
            log_size = 0;
        }
        (void)esp_littlefs_info(GW_FS_LOGFS_LABEL, &fs_total, &fs_used);
    }

    ESP_LOGI(TAG,
             "log cleanup done: log_size=%lu fs_used=%lu fs_total=%lu",
             (unsigned long)log_size,
             (unsigned long)fs_used,
             (unsigned long)fs_total);
    free(segments);
    return write_index(state);
}

static esp_err_t writer_init(gw_log_writer_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));

    gw_fs_status_t fs_status;
    esp_err_t err = gw_fs_get_status(&fs_status);
    if (err != ESP_OK || !fs_status.logfs.mounted) {
        ESP_LOGW(TAG, "logfs is not mounted; persistent log disabled");
        return ESP_ERR_INVALID_STATE;
    }

    gw_log_segment_info_t *segments = (gw_log_segment_info_t *)calloc(GW_LOG_MAX_SEGMENTS_SCAN,
                                                                      sizeof(gw_log_segment_info_t));
    if (segments == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    size_t total_size = 0;
    esp_err_t scan_err = scan_segments(segments, GW_LOG_MAX_SEGMENTS_SCAN, &count, &total_size);
    if (scan_err != ESP_OK) {
        free(segments);
        return scan_err;
    }

    state->logfs_ready = true;
    if (count == 0) {
        state->current_segment_id = 1;
        state->current_segment_size = 0;
    } else {
        state->current_segment_id = segments[count - 1U].id;
        state->current_segment_size = segments[count - 1U].size;
        if (state->current_segment_size >= GW_LOG_SEGMENT_SIZE) {
            state->current_segment_id++;
            state->current_segment_size = 0;
        }
    }

    ESP_LOGI(TAG,
             "log writer ready: segment=%lu size=%lu total_log_size=%lu",
             (unsigned long)state->current_segment_id,
             (unsigned long)state->current_segment_size,
             (unsigned long)total_size);
    free(segments);
    (void)cleanup_logs_to_target(state, false);
    return write_index(state);
}

static esp_err_t append_log_entry(gw_log_writer_state_t *state, const gw_log_entry_t *entry)
{
    if (state == NULL || entry == NULL || entry->line == NULL || entry->len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!state->logfs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (state->current_segment_size > 0 &&
        state->current_segment_size + entry->len > GW_LOG_SEGMENT_SIZE) {
        state->current_segment_id++;
        state->current_segment_size = 0;
        state->writes_since_index = 0;
        (void)write_index(state);
        (void)cleanup_logs_to_target(state, false);
    }

    char path[GW_FS_MAX_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_segment_path(state->current_segment_id, path, sizeof(path)),
                        TAG,
                        "build log segment path failed");

    FILE *fp = fopen(path, "a");
    if (fp == NULL) {
        ESP_RETURN_ON_ERROR(cleanup_logs_to_target(state, true), TAG, "cleanup before retry failed");
        fp = fopen(path, "a");
        if (fp == NULL) {
            return ESP_FAIL;
        }
    }

    size_t written = fwrite(entry->line, 1, entry->len, fp);
    int close_rc = fclose(fp);
    if (written != entry->len || close_rc != 0) {
        (void)cleanup_logs_to_target(state, true);
        return ESP_FAIL;
    }

    state->current_segment_size += entry->len;
    state->writes_since_index++;
    gw_log_mark_persisted();

    if (state->writes_since_index >= GW_LOG_INDEX_UPDATE_EVERY) {
        state->writes_since_index = 0;
        (void)write_index(state);
    }

    return ESP_OK;
}

static void log_task(void *arg)
{
    (void)arg;

    gw_log_writer_state_t writer;
    esp_err_t init_err = writer_init(&writer);
    if (init_err != ESP_OK) {
        memset(&writer, 0, sizeof(writer));
    }

    ESP_LOGI(TAG, "log_task started");

    for (;;) {
        gw_log_entry_t entry = {0};
        esp_err_t err = gw_log_dequeue(&entry, 1000);
        if (err == ESP_OK) {
            if (append_log_entry(&writer, &entry) != ESP_OK) {
                gw_log_mark_persist_failed();
            }
            gw_log_release_entry(&entry);
        }

        (void)gw_health_report_heartbeat("log_task", writer.logfs_ready ? "persist_loop" : "degraded");
        gw_state_update_stack_high_watermark("log_task",
                                             (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    }
}

esp_err_t gw_log_task_start(void)
{
    if (s_log_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(log_task,
                                "log_task",
                                GW_TASK_STACK_LOG_BYTES,
                                NULL,
                                4,
                                &s_log_task_handle);
    if (ok != pdPASS) {
        s_log_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool gw_log_task_is_started(void)
{
    return s_log_task_handle != NULL;
}
