#include "gw_log.h"

#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"

#include "gw_defaults.h"
#include "gw_fs.h"
#include "gw_redact.h"
#include "gw_time.h"

#define GW_LOG_LINE_BUFFER_SIZE       (GW_LOG_MAX_LINE_SIZE + 2U)
#define GW_LOG_QUERY_MAX_SEGMENTS     64U
#define GW_LOG_QUERY_MAX_LINES        128U
#define GW_LOG_REDACT_MESSAGE_SIZE    2049U
#define GW_LOG_REDACT_DETAIL_SIZE     513U
#define GW_LOG_REDACT_FAILED_TEXT     "<redact_failed>"

static portMUX_TYPE s_log_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_log_queue;
static uint32_t s_log_written_count;
static uint32_t s_log_dropped_count;
static uint32_t s_log_persisted_count;
static uint32_t s_log_persist_failed_count;
static uint32_t s_log_truncated_count;
static uint32_t s_log_seq;
static size_t s_log_queue_body_bytes;

typedef struct {
    uint32_t id;
    char path[GW_FS_MAX_PATH_LEN];
} gw_log_segment_ref_t;

static const char *safe_text(const char *value)
{
    return value != NULL ? value : "";
}

static const char *level_to_string(gw_log_level_t level)
{
    switch (level) {
    case GW_LOG_LEVEL_DEBUG:
        return "debug";
    case GW_LOG_LEVEL_INFO:
        return "info";
    case GW_LOG_LEVEL_WARN:
        return "warn";
    case GW_LOG_LEVEL_ERROR:
        return "error";
    default:
        return "error";
    }
}

static size_t bounded_strlen(const char *value, size_t max_len)
{
    size_t len = 0;

    if (value == NULL) {
        return 0;
    }

    while (len < max_len && value[len] != '\0') {
        ++len;
    }

    return len;
}

static bool append_fmt(char *out, size_t out_size, size_t *pos, const char *fmt, ...)
{
    if (out == NULL || pos == NULL || *pos >= out_size) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *pos, out_size - *pos, fmt, args);
    va_end(args);

    if (written < 0) {
        return false;
    }

    if ((size_t)written >= out_size - *pos) {
        *pos = out_size - 1U;
        out[*pos] = '\0';
        return false;
    }

    *pos += (size_t)written;
    return true;
}

static bool append_char(char *out, size_t out_size, size_t *pos, char value)
{
    if (out == NULL || pos == NULL || *pos + 1U >= out_size) {
        return false;
    }

    out[*pos] = value;
    ++(*pos);
    out[*pos] = '\0';
    return true;
}

static bool append_json_string_limited(char *out,
                                       size_t out_size,
                                       size_t *pos,
                                       const char *value,
                                       size_t max_input_len,
                                       bool *truncated)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)safe_text(value);
    size_t input_len = bounded_strlen((const char *)p, max_input_len);
    bool ok = append_char(out, out_size, pos, '"');

    for (size_t i = 0; ok && i < input_len; ++i) {
        unsigned char c = p[i];
        char escaped[6];
        size_t escaped_len = 0;

        if (c == '"' || c == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)c;
            escaped_len = 2;
        } else if (c == '\n') {
            escaped[0] = '\\';
            escaped[1] = 'n';
            escaped_len = 2;
        } else if (c == '\r') {
            escaped[0] = '\\';
            escaped[1] = 'r';
            escaped_len = 2;
        } else if (c == '\t') {
            escaped[0] = '\\';
            escaped[1] = 't';
            escaped_len = 2;
        } else if (c < 0x20) {
            escaped[0] = '\\';
            escaped[1] = 'u';
            escaped[2] = '0';
            escaped[3] = '0';
            escaped[4] = hex[(c >> 4) & 0x0f];
            escaped[5] = hex[c & 0x0f];
            escaped_len = 6;
        } else {
            escaped[0] = (char)c;
            escaped_len = 1;
        }

        if (*pos + escaped_len + 1U >= out_size) {
            if (truncated != NULL) {
                *truncated = true;
            }
            break;
        }

        memcpy(out + *pos, escaped, escaped_len);
        *pos += escaped_len;
        out[*pos] = '\0';
    }

    if (value != NULL && value[input_len] != '\0') {
        if (truncated != NULL) {
            *truncated = true;
        }
    }

    return ok && append_char(out, out_size, pos, '"');
}

static void log_stats_reset(void)
{
    portENTER_CRITICAL(&s_log_stats_lock);
    s_log_written_count = 0;
    s_log_dropped_count = 0;
    s_log_persisted_count = 0;
    s_log_persist_failed_count = 0;
    s_log_truncated_count = 0;
    s_log_seq = 0;
    s_log_queue_body_bytes = 0;
    portEXIT_CRITICAL(&s_log_stats_lock);
}

static uint32_t log_stats_next_seq(void)
{
    uint32_t seq;

    portENTER_CRITICAL(&s_log_stats_lock);
    ++s_log_written_count;
    seq = ++s_log_seq;
    portEXIT_CRITICAL(&s_log_stats_lock);

    return seq;
}

static void log_stats_inc_dropped(void)
{
    portENTER_CRITICAL(&s_log_stats_lock);
    ++s_log_dropped_count;
    portEXIT_CRITICAL(&s_log_stats_lock);
}

static void log_stats_inc_truncated(void)
{
    portENTER_CRITICAL(&s_log_stats_lock);
    ++s_log_truncated_count;
    portEXIT_CRITICAL(&s_log_stats_lock);
}

static void log_queue_bytes_add(size_t len)
{
    portENTER_CRITICAL(&s_log_stats_lock);
    s_log_queue_body_bytes += len;
    portEXIT_CRITICAL(&s_log_stats_lock);
}

static void log_queue_bytes_sub(size_t len)
{
    portENTER_CRITICAL(&s_log_stats_lock);
    if (s_log_queue_body_bytes >= len) {
        s_log_queue_body_bytes -= len;
    } else {
        s_log_queue_body_bytes = 0;
    }
    portEXIT_CRITICAL(&s_log_stats_lock);
}

static size_t log_queue_bytes_get(void)
{
    size_t value;

    portENTER_CRITICAL(&s_log_stats_lock);
    value = s_log_queue_body_bytes;
    portEXIT_CRITICAL(&s_log_stats_lock);

    return value;
}

static void serial_log_write(gw_log_level_t level,
                             const char *module,
                             const char *event,
                             const char *message,
                             const gw_result_t *result)
{
    const char *tag = module != NULL && module[0] != '\0' ? module : "gw";
    const char *event_name = safe_text(event);
    const char *msg = safe_text(message);
    const bool has_result = result != NULL && result->reason_code != NULL;

    if (has_result) {
        switch (level) {
        case GW_LOG_LEVEL_DEBUG:
            ESP_LOGD(tag, "[%s] %s code=%ld reason=%s detail=%s", event_name, msg,
                     (long)result->code, safe_text(result->reason_code), safe_text(result->detail));
            break;
        case GW_LOG_LEVEL_INFO:
            ESP_LOGI(tag, "[%s] %s code=%ld reason=%s detail=%s", event_name, msg,
                     (long)result->code, safe_text(result->reason_code), safe_text(result->detail));
            break;
        case GW_LOG_LEVEL_WARN:
            ESP_LOGW(tag, "[%s] %s code=%ld reason=%s detail=%s", event_name, msg,
                     (long)result->code, safe_text(result->reason_code), safe_text(result->detail));
            break;
        case GW_LOG_LEVEL_ERROR:
        default:
            ESP_LOGE(tag, "[%s] %s code=%ld reason=%s detail=%s", event_name, msg,
                     (long)result->code, safe_text(result->reason_code), safe_text(result->detail));
            break;
        }
    } else {
        switch (level) {
        case GW_LOG_LEVEL_DEBUG:
            ESP_LOGD(tag, "[%s] %s", event_name, msg);
            break;
        case GW_LOG_LEVEL_INFO:
            ESP_LOGI(tag, "[%s] %s", event_name, msg);
            break;
        case GW_LOG_LEVEL_WARN:
            ESP_LOGW(tag, "[%s] %s", event_name, msg);
            break;
        case GW_LOG_LEVEL_ERROR:
        default:
            ESP_LOGE(tag, "[%s] %s", event_name, msg);
            break;
        }
    }
}

static char *build_json_line(gw_log_level_t level,
                             const char *module,
                             const char *event,
                             const char *message,
                             const gw_result_t *result,
                             size_t *out_len)
{
    char *line = (char *)malloc(GW_LOG_LINE_BUFFER_SIZE);
    if (line == NULL) {
        return NULL;
    }

    gw_time_point_t now = gw_time_now();
    uint32_t seq = log_stats_next_seq();
    bool truncated = false;
    size_t pos = 0;
    bool ok = true;

    line[0] = '\0';
    ok = ok && append_fmt(line,
                          GW_LOG_LINE_BUFFER_SIZE,
                          &pos,
                          "{\"seq\":%lu,\"uptime_ms\":%" PRIu64 ",\"ts\":%" PRIi64
                          ",\"time_synced\":%s,\"level\":\"%s\",\"module\":",
                          (unsigned long)seq,
                          now.uptime_ms,
                          now.utc_trusted ? now.utc_ms : 0,
                          now.utc_trusted ? "true" : "false",
                          level_to_string(level));
    ok = ok && append_json_string_limited(line, GW_LOG_LINE_BUFFER_SIZE, &pos, module, 64, &truncated);
    ok = ok && append_fmt(line, GW_LOG_LINE_BUFFER_SIZE, &pos, ",\"event\":");
    ok = ok && append_json_string_limited(line, GW_LOG_LINE_BUFFER_SIZE, &pos, event, 96, &truncated);
    ok = ok && append_fmt(line, GW_LOG_LINE_BUFFER_SIZE, &pos, ",\"msg\":");
    ok = ok && append_json_string_limited(line, GW_LOG_LINE_BUFFER_SIZE, &pos, message, 2048, &truncated);

    if (result != NULL) {
        ok = ok && append_fmt(line, GW_LOG_LINE_BUFFER_SIZE, &pos, ",\"code\":%ld", (long)result->code);
        ok = ok && append_fmt(line, GW_LOG_LINE_BUFFER_SIZE, &pos, ",\"reason_code\":");
        ok = ok && append_json_string_limited(line,
                                              GW_LOG_LINE_BUFFER_SIZE,
                                              &pos,
                                              result->reason_code,
                                              96,
                                              &truncated);
        ok = ok && append_fmt(line, GW_LOG_LINE_BUFFER_SIZE, &pos, ",\"detail\":");
        ok = ok && append_json_string_limited(line,
                                              GW_LOG_LINE_BUFFER_SIZE,
                                              &pos,
                                              result->detail,
                                              512,
                                              &truncated);
    }

    ok = ok && append_fmt(line,
                          GW_LOG_LINE_BUFFER_SIZE,
                          &pos,
                          ",\"truncated\":%s}\n",
                          truncated ? "true" : "false");

    if (!ok || pos == 0 || pos >= GW_LOG_LINE_BUFFER_SIZE) {
        pos = 0;
        truncated = true;
        line[0] = '\0';
        (void)append_fmt(line,
                         GW_LOG_LINE_BUFFER_SIZE,
                         &pos,
                         "{\"seq\":%lu,\"uptime_ms\":%" PRIu64
                         ",\"ts\":0,\"time_synced\":false,\"level\":\"error\","
                         "\"module\":\"log\",\"event\":\"format_failed\","
                         "\"msg\":\"log line truncated by formatter\",\"truncated\":true}\n",
                         (unsigned long)seq,
                         now.uptime_ms);
    }

    if (truncated) {
        log_stats_inc_truncated();
    }

    if (out_len != NULL) {
        *out_len = pos;
    }

    return line;
}

static esp_err_t enqueue_line(gw_log_level_t level, char *line, size_t len)
{
    if (line == NULL || len == 0) {
        free(line);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_log_queue == NULL || level == GW_LOG_LEVEL_DEBUG) {
        free(line);
        return ESP_OK;
    }

    if (log_queue_bytes_get() + len > GW_LOG_QUEUE_MAX_BODY_BYTES) {
        free(line);
        log_stats_inc_dropped();
        return ESP_ERR_NO_MEM;
    }

    gw_log_entry_t entry = {
        .line = line,
        .len = len,
        .level = level,
    };

    log_queue_bytes_add(len);
    if (xQueueSend(s_log_queue, &entry, 0) != pdPASS) {
        log_queue_bytes_sub(len);
        free(line);
        log_stats_inc_dropped();
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t gw_log_init(void)
{
    log_stats_reset();

    if (s_log_queue == NULL) {
        s_log_queue = xQueueCreate(GW_EVENT_QUEUE_LOG_LEN, sizeof(gw_log_entry_t));
        if (s_log_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        gw_log_entry_t entry;
        while (xQueueReceive(s_log_queue, &entry, 0) == pdPASS) {
            free(entry.line);
        }
    }

    return ESP_OK;
}

esp_err_t gw_log_write(gw_log_level_t level,
                       const char *module,
                       const char *event,
                       const char *message,
                       const gw_result_t *result)
{
    size_t line_len = 0;
    char *redacted_message = NULL;
    char *redacted_detail = NULL;
    const char *message_for_log = message;
    const gw_result_t *result_for_log = result;
    gw_result_t result_copy;

    if (message != NULL && message[0] != '\0') {
        redacted_message = (char *)malloc(GW_LOG_REDACT_MESSAGE_SIZE);
        if (redacted_message != NULL &&
            gw_redact_sensitive_text(message, redacted_message, GW_LOG_REDACT_MESSAGE_SIZE) == ESP_OK) {
            message_for_log = redacted_message;
        } else {
            free(redacted_message);
            redacted_message = NULL;
            message_for_log = GW_LOG_REDACT_FAILED_TEXT;
        }
    }

    if (result != NULL) {
        result_copy = *result;
        if (result->detail != NULL && result->detail[0] != '\0') {
            redacted_detail = (char *)malloc(GW_LOG_REDACT_DETAIL_SIZE);
            if (redacted_detail != NULL &&
                gw_redact_sensitive_text(result->detail, redacted_detail, GW_LOG_REDACT_DETAIL_SIZE) == ESP_OK) {
                result_copy.detail = redacted_detail;
            } else {
                free(redacted_detail);
                redacted_detail = NULL;
                result_copy.detail = GW_LOG_REDACT_FAILED_TEXT;
            }
        }
        result_for_log = &result_copy;
    }

    char *line = build_json_line(level, module, event, message_for_log, result_for_log, &line_len);

    serial_log_write(level, module, event, message_for_log, result_for_log);

    if (line == NULL) {
        log_stats_inc_dropped();
        free(redacted_message);
        free(redacted_detail);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = enqueue_line(level, line, line_len);
    free(redacted_message);
    free(redacted_detail);
    return err == ESP_OK ? ESP_OK : err;
}

uint32_t gw_log_get_written_count(void)
{
    uint32_t value;

    portENTER_CRITICAL(&s_log_stats_lock);
    value = s_log_written_count;
    portEXIT_CRITICAL(&s_log_stats_lock);

    return value;
}

uint32_t gw_log_get_dropped_count(void)
{
    uint32_t value;

    portENTER_CRITICAL(&s_log_stats_lock);
    value = s_log_dropped_count;
    portEXIT_CRITICAL(&s_log_stats_lock);

    return value;
}

esp_err_t gw_log_get_stats(gw_log_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_log_stats_lock);
    out->written_count = s_log_written_count;
    out->dropped_count = s_log_dropped_count;
    out->persisted_count = s_log_persisted_count;
    out->persist_failed_count = s_log_persist_failed_count;
    out->truncated_count = s_log_truncated_count;
    out->queue_body_bytes = s_log_queue_body_bytes;
    portEXIT_CRITICAL(&s_log_stats_lock);

    out->queue_pending_count = s_log_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_log_queue) : 0;
    return ESP_OK;
}

esp_err_t gw_log_dequeue(gw_log_entry_t *out, uint32_t timeout_ms)
{
    if (out == NULL || s_log_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueReceive(s_log_queue, out, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }

    log_queue_bytes_sub(out->len);
    return ESP_OK;
}

void gw_log_release_entry(gw_log_entry_t *entry)
{
    if (entry == NULL) {
        return;
    }

    free(entry->line);
    entry->line = NULL;
    entry->len = 0;
}

void gw_log_mark_persisted(void)
{
    portENTER_CRITICAL(&s_log_stats_lock);
    ++s_log_persisted_count;
    portEXIT_CRITICAL(&s_log_stats_lock);
}

void gw_log_mark_persist_failed(void)
{
    portENTER_CRITICAL(&s_log_stats_lock);
    ++s_log_persist_failed_count;
    portEXIT_CRITICAL(&s_log_stats_lock);
}

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

static void sort_segments(gw_log_segment_ref_t *segments, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1U; j < count; ++j) {
            if (segments[j].id < segments[i].id) {
                gw_log_segment_ref_t temp = segments[i];
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

static esp_err_t scan_recent_segments(gw_log_segment_ref_t *segments, size_t *out_count)
{
    if (segments == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(GW_FS_LOGFS_BASE_PATH);
    if (dir == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        uint32_t id = 0;
        if (!parse_segment_name(entry->d_name, &id)) {
            continue;
        }

        if (count < GW_LOG_QUERY_MAX_SEGMENTS) {
            segments[count].id = id;
            if (!build_logfs_child_path(entry->d_name,
                                        segments[count].path,
                                        sizeof(segments[count].path))) {
                continue;
            }
            ++count;
            sort_segments(segments, count);
            continue;
        }

        if (id > segments[0].id) {
            segments[0].id = id;
            if (!build_logfs_child_path(entry->d_name,
                                        segments[0].path,
                                        sizeof(segments[0].path))) {
                continue;
            }
            sort_segments(segments, count);
        }
    }

    closedir(dir);
    *out_count = count;
    return ESP_OK;
}

esp_err_t gw_log_read_recent_lines(uint32_t max_lines,
                                   char *out,
                                   size_t out_size,
                                   size_t *out_len,
                                   uint32_t *out_lines)
{
    if (max_lines == 0 || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (max_lines > GW_LOG_QUERY_MAX_LINES) {
        max_lines = GW_LOG_QUERY_MAX_LINES;
    }

    gw_log_segment_ref_t *segments = (gw_log_segment_ref_t *)calloc(GW_LOG_QUERY_MAX_SEGMENTS,
                                                                    sizeof(gw_log_segment_ref_t));
    if (segments == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t segment_count = 0;
    esp_err_t err = scan_recent_segments(segments, &segment_count);
    if (err != ESP_OK) {
        free(segments);
        return err;
    }

    char **ring = (char **)calloc(max_lines, sizeof(char *));
    char *line = (char *)malloc(GW_LOG_LINE_BUFFER_SIZE);
    if (ring == NULL || line == NULL) {
        free(segments);
        free(ring);
        free(line);
        return ESP_ERR_NO_MEM;
    }

    uint32_t seen = 0;
    for (size_t i = 0; i < segment_count; ++i) {
        FILE *fp = fopen(segments[i].path, "r");
        if (fp == NULL) {
            continue;
        }

        while (fgets(line, GW_LOG_LINE_BUFFER_SIZE, fp) != NULL) {
            size_t len = strlen(line);
            char *copy = (char *)malloc(len + 1U);
            if (copy == NULL) {
                fclose(fp);
                for (uint32_t j = 0; j < max_lines; ++j) {
                    free(ring[j]);
                }
                free(segments);
                free(ring);
                free(line);
                return ESP_ERR_NO_MEM;
            }

            memcpy(copy, line, len + 1U);
            uint32_t slot = seen % max_lines;
            free(ring[slot]);
            ring[slot] = copy;
            ++seen;
        }

        fclose(fp);
    }

    uint32_t count = seen < max_lines ? seen : max_lines;
    uint32_t start = seen >= max_lines ? (seen % max_lines) : 0;
    size_t pos = 0;
    esp_err_t result = ESP_OK;

    out[0] = '\0';
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t slot = (start + i) % max_lines;
        size_t len = ring[slot] != NULL ? strlen(ring[slot]) : 0;
        if (pos + len >= out_size) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        memcpy(out + pos, ring[slot], len);
        pos += len;
        out[pos] = '\0';
    }

    for (uint32_t i = 0; i < max_lines; ++i) {
        free(ring[i]);
    }
    free(segments);
    free(ring);
    free(line);

    if (out_len != NULL) {
        *out_len = pos;
    }
    if (out_lines != NULL) {
        *out_lines = result == ESP_OK ? count : 0;
    }

    return result;
}
