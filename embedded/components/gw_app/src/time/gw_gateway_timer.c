#include "gw_gateway_timer.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "gw_dongle_mqtt_bridge.h"
#include "gw_gateway_protocol.h"
#include "gw_gs200_protocol.h"
#include "gw_scheduler.h"
#include "gw_storage.h"
#include "gw_time.h"

#define TIMER_MAX_TASKS 32U
#define TIMER_JSON_INTEGER_MAX UINT64_C(9007199254740991)
#define TIMER_DAY_MS UINT64_C(86400000)
#define TIMER_MAX_PERIOD_MS (52ULL * 7ULL * TIMER_DAY_MS)
#define TIMER_NAMESPACE "gateway_timer"
#define TIMER_KEY "table"
#define TIMER_SCHEMA_VERSION 1U
#define TIMER_MIN_TIMEZONE_OFFSET_MIN (-720)
#define TIMER_MAX_TIMEZONE_OFFSET_MIN 840

typedef enum {
    TIMER_DURATION,
    TIMER_DAILY,
    TIMER_INTERVAL,
} timer_kind_t;

typedef struct {
    cJSON *config;
    timer_kind_t kind;
    uint64_t period_ms;
    uint64_t next_utc_ms;
    uint64_t occurrence_id;
    int64_t last_day;
    bool consumed;

    /* Only persisted occurrence claims survive reboot; action progress never does. */
    uint64_t due_uptime_ms;
    bool due_uptime_valid;
    bool active;
    size_t action_index;
    bool delaying;
    uint64_t delay_until_ms;
    uint64_t scheduled_utc_ms;
    bool finished;
    esp_err_t execution_error;
} timer_task_t;

typedef struct {
    size_t count;
    uint64_t revision;
    uint64_t next_id;
    int16_t timezone_offset_min;
    timer_task_t tasks[TIMER_MAX_TASKS];
} timer_table_t;

static timer_table_t s_table;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static bool s_initialized;
static bool s_time_paused;
static bool s_clock_seen;
static gw_time_point_t s_previous_clock;
static int16_t s_previous_timezone;
static int16_t s_runtime_timezone;

static const cJSON *field(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static bool integer(const cJSON *item, uint64_t minimum, uint64_t maximum,
                    uint64_t *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < (double)minimum ||
        item->valuedouble > (double)maximum ||
        floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    if (out != NULL) {
        *out = (uint64_t)item->valuedouble;
    }
    return true;
}

static bool text_valid(const cJSON *item, bool allow_empty)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           (allow_empty || item->valuestring[0] != '\0') &&
           strlen(item->valuestring) <= 32U;
}

static bool string_is(const cJSON *item, const char *value)
{
    return cJSON_IsString(item) && strcmp(item->valuestring, value) == 0;
}

static bool only_fields(const cJSON *object, const char *const *names,
                        size_t count)
{
    if (!cJSON_IsObject(object)) {
        return false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (item->string != NULL && strcmp(item->string, names[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        for (const cJSON *earlier = object->child; earlier != item;
             earlier = earlier->next) {
            if (strcmp(earlier->string, item->string) == 0) {
                return false;
            }
        }
    }
    return true;
}

static bool add_item(cJSON *parent, const char *name, cJSON *item)
{
    if (item == NULL) {
        return false;
    }
    bool added = name == NULL ? cJSON_AddItemToArray(parent, item)
                              : cJSON_AddItemToObject(parent, name, item);
    if (!added) {
        cJSON_Delete(item);
    }
    return added;
}

static bool replace_item(cJSON *object, const char *name, cJSON *item)
{
    if (item == NULL) {
        return false;
    }
    bool replaced = cJSON_ReplaceItemInObjectCaseSensitive(object, name, item);
    if (!replaced) {
        cJSON_Delete(item);
    }
    return replaced;
}

static cJSON *ack(int code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    if (result == NULL ||
        !add_item(result, "code", cJSON_CreateNumber(code)) ||
        !add_item(result, "message", cJSON_CreateString(message))) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static cJSON *storage_error(esp_err_t err)
{
    if (err == ESP_ERR_NO_MEM || err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return ack(10, "no_memory");
    }
    return err == ESP_ERR_TIMEOUT ? ack(9, "timeout")
                                  : ack(14, "internal_error");
}

static bool task_enabled(const timer_task_t *task)
{
    return cJSON_IsTrue(field(task->config, "enabled"));
}

static const char *task_id(const timer_task_t *task)
{
    return field(task->config, "task_id")->valuestring;
}

static uint64_t task_revision(const timer_task_t *task)
{
    return (uint64_t)field(task->config, "revision")->valuedouble;
}

static int find_task(const timer_table_t *table, const char *id)
{
    for (size_t i = 0; i < table->count; ++i) {
        if (strcmp(task_id(&table->tasks[i]), id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void free_table(timer_table_t *table)
{
    for (size_t i = 0; i < table->count; ++i) {
        cJSON_Delete(table->tasks[i].config);
    }
    memset(table, 0, sizeof(*table));
}

static timer_table_t *copy_table(void)
{
    timer_table_t *copy = calloc(1, sizeof(*copy));
    if (copy == NULL) {
        return NULL;
    }
    copy->revision = s_table.revision;
    copy->next_id = s_table.next_id;
    copy->timezone_offset_min = s_table.timezone_offset_min;
    for (size_t i = 0; i < s_table.count; ++i) {
        copy->tasks[i] = s_table.tasks[i];
        copy->tasks[i].config = cJSON_Duplicate(s_table.tasks[i].config, true);
        if (copy->tasks[i].config == NULL) {
            free_table(copy);
            free(copy);
            return NULL;
        }
        ++copy->count;
    }
    return copy;
}

static bool validate_task(const cJSON *config, timer_kind_t *kind,
                           uint64_t *period_ms)
{
    static const char *const task_fields[] = {
        "task_id", "revision", "enabled", "name", "type", "schedule", "action",
    };
    if (!only_fields(config, task_fields, 7U) ||
        !cJSON_IsBool(field(config, "enabled")) ||
        !text_valid(field(config, "name"), true)) {
        return false;
    }
    const cJSON *id = field(config, "task_id");
    const cJSON *revision = field(config, "revision");
    if ((id == NULL) != (revision == NULL) ||
        (id != NULL && (!text_valid(id, false) ||
                       !integer(revision, 1U, TIMER_JSON_INTEGER_MAX, NULL)))) {
        return false;
    }
    const cJSON *type = field(config, "type");
    const cJSON *schedule = field(config, "schedule");
    *period_ms = 0U;
    if (string_is(type, "daily")) {
        static const char *const names[] = {"hour", "minute"};
        if (!only_fields(schedule, names, 2U) ||
            !integer(field(schedule, "hour"), 0U, 23U, NULL) ||
            !integer(field(schedule, "minute"), 0U, 59U, NULL)) {
            return false;
        }
        *kind = TIMER_DAILY;
    } else {
        static const char *const names[] = {"value", "unit"};
        if (!only_fields(schedule, names, 2U)) {
            return false;
        }
        if (string_is(type, "duration")) {
            *kind = TIMER_DURATION;
        } else if (string_is(type, "interval")) {
            *kind = TIMER_INTERVAL;
        } else {
            return false;
        }
        const cJSON *unit = field(schedule, "unit");
        uint64_t multiplier;
        if (string_is(unit, "minute")) {
            multiplier = 60000U;
        } else if (string_is(unit, "hour")) {
            multiplier = 3600000U;
        } else if (*kind == TIMER_DURATION && string_is(unit, "day")) {
            multiplier = TIMER_DAY_MS;
        } else if (*kind == TIMER_DURATION && string_is(unit, "week")) {
            multiplier = 7U * TIMER_DAY_MS;
        } else {
            return false;
        }
        uint64_t value;
        if (!integer(field(schedule, "value"), 1U,
                     TIMER_MAX_PERIOD_MS / multiplier, &value)) {
            return false;
        }
        *period_ms = value * multiplier;
    }
    const cJSON *actions = field(config, "action");
    if (!cJSON_IsArray(actions) || actions->child == NULL) {
        return false;
    }
    const cJSON *action;
    cJSON_ArrayForEach(action, actions) {
        const cJSON *action_type = field(action, "type");
        if (string_is(action_type, "delay")) {
            static const char *const names[] = {"type", "duration_ms"};
            /* Bound each wait by exact JSON integer representation and uint64 math. */
            if (!only_fields(action, names, 2U) ||
                !integer(field(action, "duration_ms"), 0U,
                         TIMER_JSON_INTEGER_MAX, NULL)) {
                return false;
            }
            continue;
        }
        bool scene = string_is(action_type, "scene_switch");
        if (!scene && !string_is(action_type, "light_on") &&
            !string_is(action_type, "light_off")) {
            return false;
        }
        static const char *const control_names[] = {"type", "target", "scene_id"};
        if (!only_fields(action, control_names, scene ? 3U : 2U) ||
            (scene && !integer(field(action, "scene_id"), 1U, 16U, NULL))) {
            return false;
        }
        const cJSON *target = field(action, "target");
        static const char *const target_names[] = {
            "area_addr", "group_addr", "unicast_addr",
        };
        if (!only_fields(target, target_names, 3U) ||
            !integer(field(target, "area_addr"), 0U, 65535U, NULL) ||
            !integer(field(target, "group_addr"), 0U, 0x3FFEU, NULL) ||
            !integer(field(target, "unicast_addr"), 0U, 0x7FFFU, NULL)) {
            return false;
        }
    }
    return true;
}

static esp_err_t save_table(const timer_table_t *table)
{
    cJSON *record = cJSON_CreateObject();
    cJSON *tasks = cJSON_CreateArray();
    if (record == NULL || tasks == NULL) {
        cJSON_Delete(record);
        cJSON_Delete(tasks);
        return ESP_ERR_NO_MEM;
    }
    if (!add_item(record, "tasks", tasks) ||
        !add_item(record, "version", cJSON_CreateNumber(TIMER_SCHEMA_VERSION)) ||
        !add_item(record, "table_revision", cJSON_CreateNumber((double)table->revision)) ||
        !add_item(record, "tz_offset_min", cJSON_CreateNumber(table->timezone_offset_min)) ||
        !add_item(record, "next_id", cJSON_CreateNumber((double)table->next_id))) {
        cJSON_Delete(record);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < table->count; ++i) {
        const timer_task_t *task = &table->tasks[i];
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL ||
            !add_item(entry, "config", cJSON_Duplicate(task->config, true)) ||
            !add_item(entry, "next_utc_ms", cJSON_CreateNumber((double)task->next_utc_ms)) ||
            !add_item(entry, "occurrence_id", cJSON_CreateNumber((double)task->occurrence_id)) ||
            !add_item(entry, "last_day", cJSON_CreateNumber((double)task->last_day)) ||
            !add_item(entry, "consumed", cJSON_CreateBool(task->consumed))) {
            cJSON_Delete(entry);
            cJSON_Delete(record);
            return ESP_ERR_NO_MEM;
        }
        if (!add_item(tasks, NULL, entry)) {
            cJSON_Delete(record);
            return ESP_ERR_NO_MEM;
        }
    }
    char *serialized = cJSON_PrintUnformatted(record);
    cJSON_Delete(record);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = gw_storage_secure_set_blob(TIMER_NAMESPACE, TIMER_KEY,
                                                GW_STORAGE_VALUE_JSON,
                                                serialized, strlen(serialized) + 1U);
    cJSON_free(serialized);
    return err;
}

static esp_err_t commit_table(timer_table_t *replacement)
{
    esp_err_t err = save_table(replacement);
    if (err == ESP_OK) {
        free_table(&s_table);
        s_table = *replacement;
        memset(replacement, 0, sizeof(*replacement));
    }
    return err;
}

static esp_err_t load_table(void)
{
    size_t capacity = 1024U;
    char *serialized = NULL;
    size_t length = 0U;
    esp_err_t err;
    for (;;) {
        serialized = malloc(capacity);
        if (serialized == NULL) {
            return ESP_ERR_NO_MEM;
        }
        err = gw_storage_secure_get_blob(TIMER_NAMESPACE, TIMER_KEY,
                                          GW_STORAGE_VALUE_JSON, serialized,
                                          capacity, &length);
        if (err != ESP_ERR_INVALID_SIZE) {
            break;
        }
        free(serialized);
        if (capacity > SIZE_MAX / 2U) {
            return ESP_ERR_NO_MEM;
        }
        capacity *= 2U;
    }
    if (err != ESP_OK) {
        free(serialized);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    const char *end = NULL;
    cJSON *record = cJSON_ParseWithLengthOpts(serialized, length, &end, true);
    bool valid = record != NULL && length > 0U &&
                 end == serialized + length - 1U &&
                 integer(field(record, "version"), TIMER_SCHEMA_VERSION,
                         TIMER_SCHEMA_VERSION, NULL) &&
                 integer(field(record, "table_revision"), 0U,
                         TIMER_JSON_INTEGER_MAX, &s_table.revision) &&
                 integer(field(record, "next_id"), 1U,
                         TIMER_JSON_INTEGER_MAX, &s_table.next_id);
    const cJSON *tasks = field(record, "tasks");
    const cJSON *offset = field(record, "tz_offset_min");
    valid = valid && cJSON_IsArray(tasks) &&
            cJSON_GetArraySize(tasks) <= (int)TIMER_MAX_TASKS &&
            cJSON_IsNumber(offset) && isfinite(offset->valuedouble) &&
            floor(offset->valuedouble) == offset->valuedouble &&
            offset->valuedouble >= TIMER_MIN_TIMEZONE_OFFSET_MIN &&
            offset->valuedouble <= TIMER_MAX_TIMEZONE_OFFSET_MIN;
    if (valid) {
        s_table.timezone_offset_min = (int16_t)offset->valuedouble;
    }
    const cJSON *entry;
    if (valid) {
        cJSON_ArrayForEach(entry, tasks) {
            timer_task_t *task = &s_table.tasks[s_table.count];
            const cJSON *config = field(entry, "config");
            const cJSON *last_day = field(entry, "last_day");
            if (!validate_task(config, &task->kind, &task->period_ms) ||
                !text_valid(field(config, "task_id"), false) ||
                find_task(&s_table, field(config, "task_id")->valuestring) >= 0 ||
                !integer(field(entry, "next_utc_ms"), 0U,
                         TIMER_JSON_INTEGER_MAX, &task->next_utc_ms) ||
                !integer(field(entry, "occurrence_id"), 0U,
                         TIMER_JSON_INTEGER_MAX, &task->occurrence_id) ||
                !(integer(last_day, 0U, TIMER_JSON_INTEGER_MAX / TIMER_DAY_MS - 1U, NULL) ||
                  (cJSON_IsNumber(last_day) && last_day->valuedouble == -1.0)) ||
                !cJSON_IsBool(field(entry, "consumed"))) {
                valid = false;
                break;
            }
            task->last_day = (int64_t)last_day->valuedouble;
            task->consumed = cJSON_IsTrue(field(entry, "consumed"));
            task->config = cJSON_Duplicate(config, true);
            if (task->config == NULL) {
                err = ESP_ERR_NO_MEM;
                valid = false;
                break;
            }
            ++s_table.count;
        }
    }
    cJSON_Delete(record);
    free(serialized);
    if (!valid) {
        free_table(&s_table);
        return err == ESP_ERR_NO_MEM ? err : ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static int16_t timezone_offset(void)
{
    gw_scheduler_status_t status;
    if (gw_scheduler_get_status(&status) == ESP_OK &&
        status.timezone_offset_min != s_runtime_timezone) {
        return status.timezone_offset_min;
    }
    return s_table.timezone_offset_min;
}

static uint64_t daily_next(const timer_task_t *task, uint64_t utc_ms,
                           int16_t offset_min)
{
    int64_t offset = (int64_t)offset_min * 60000;
    int64_t local_ms = (int64_t)utc_ms + offset;
    int64_t day = local_ms / (int64_t)TIMER_DAY_MS;
    if (day <= task->last_day) {
        day = task->last_day + 1;
    }
    const cJSON *schedule = field(task->config, "schedule");
    int64_t minute = (int64_t)field(schedule, "hour")->valuedouble * 60 +
                     (int64_t)field(schedule, "minute")->valuedouble;
    int64_t deadline = day * (int64_t)TIMER_DAY_MS + minute * 60000 - offset;
    if (deadline <= (int64_t)utc_ms) {
        deadline += (int64_t)TIMER_DAY_MS;
    }
    return (uint64_t)deadline;
}

static void reset_schedule(timer_task_t *task, const gw_time_point_t *now,
                            int16_t offset)
{
    task->consumed = false;
    task->active = false;
    task->finished = false;
    task->delaying = false;
    task->action_index = 0U;
    task->next_utc_ms = 0U;
    task->due_uptime_valid = false;
    if (!task_enabled(task)) {
        return;
    }
    if (task->kind == TIMER_DAILY) {
        if (now->utc_trusted) {
            task->next_utc_ms = daily_next(task, now->utc_ms, offset);
        }
    } else {
        task->due_uptime_ms = now->uptime_ms + task->period_ms;
        task->due_uptime_valid = true;
        if (now->utc_trusted) {
            task->next_utc_ms = now->utc_ms + task->period_ms;
        }
    }
}

static bool busy(const timer_task_t *task, const gw_time_point_t *now)
{
    if (task->active) {
        return true;
    }
    if (!task_enabled(task) || !now->utc_trusted) {
        return false;
    }
    if (task->kind == TIMER_DAILY) {
        return task->next_utc_ms != 0U && task->next_utc_ms <= now->utc_ms;
    }
    return task->due_uptime_valid && task->due_uptime_ms <= now->uptime_ms;
}

static cJSON *task_response(const timer_table_t *table, const size_t *indices,
                             size_t count)
{
    cJSON *result = ack(0, "ok");
    cJSON *tasks = cJSON_CreateArray();
    if (result == NULL || tasks == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(tasks);
        return NULL;
    }
    if (!add_item(result, "task", tasks)) {
        cJSON_Delete(result);
        return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!add_item(tasks, NULL,
                      cJSON_Duplicate(table->tasks[indices[i]].config, true))) {
            cJSON_Delete(result);
            return NULL;
        }
    }
    return result;
}

static cJSON *list_tasks(const cJSON *pd)
{
    uint64_t cursor, limit;
    if (!integer(field(pd, "cursor"), 0U, TIMER_MAX_TASKS, &cursor) ||
        !integer(field(pd, "limit"), 1U, 3U, &limit) ||
        cursor > s_table.count) {
        return ack(1, "invalid_param");
    }
    size_t end = (size_t)(cursor + limit);
    if (end > s_table.count) {
        end = s_table.count;
    }
    bool done = end == s_table.count;
    cJSON *result = ack(0, "ok");
    cJSON *items = cJSON_CreateArray();
    if (result == NULL || items == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(items);
        return NULL;
    }
    if (!add_item(result, "items", items) ||
        !add_item(result, "next_cursor", cJSON_CreateNumber(done ? 0 : (double)end)) ||
        !add_item(result, "done", cJSON_CreateBool(done)) ||
        !add_item(result, "total", cJSON_CreateNumber((double)s_table.count)) ||
        !add_item(result, "table_revision", cJSON_CreateNumber((double)s_table.revision)) ||
        !add_item(result, "tz_offset_min", cJSON_CreateNumber(timezone_offset()))) {
        cJSON_Delete(result);
        return NULL;
    }
    static const char *const summary[] = {
        "task_id", "revision", "enabled", "name", "type",
    };
    for (size_t i = (size_t)cursor; i < end; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            cJSON_Delete(result);
            return NULL;
        }
        for (size_t j = 0; j < 5U; ++j) {
            if (!add_item(item, summary[j],
                          cJSON_Duplicate(field(s_table.tasks[i].config,
                                                summary[j]), true))) {
                cJSON_Delete(item);
                cJSON_Delete(result);
                return NULL;
            }
        }
        if (!add_item(items, NULL, item)) {
            cJSON_Delete(result);
            return NULL;
        }
    }
    return result;
}

static cJSON *upsert_tasks(const cJSON *pd, const gw_time_point_t *now)
{
    const cJSON *tasks = field(pd, "task");
    int count = cJSON_GetArraySize(tasks);
    if (!cJSON_IsArray(tasks) || count < 1 || count > (int)TIMER_MAX_TASKS) {
        return ack(1, "invalid_param");
    }
    size_t indices[TIMER_MAX_TASKS];
    timer_table_t *replacement = copy_table();
    if (replacement == NULL) {
        return ack(10, "no_memory");
    }
    cJSON *result = NULL;
    int16_t offset = timezone_offset();
    size_t position = 0U;
    const cJSON *config;
    cJSON_ArrayForEach(config, tasks) {
        timer_kind_t kind;
        uint64_t period;
        if (!validate_task(config, &kind, &period)) {
            result = ack(1, "invalid_param");
            goto finish;
        }
        const cJSON *id = field(config, "task_id");
        int index = id == NULL ? (int)replacement->count
                               : find_task(&s_table, id->valuestring);
        if (id != NULL && index < 0) {
            result = ack(6, "not_found");
            goto finish;
        }
        for (size_t i = 0; i < position; ++i) {
            if (indices[i] == (size_t)index) {
                result = ack(1, "invalid_param");
                goto finish;
            }
        }
        uint64_t revision = 1U;
        if (id != NULL) {
            const timer_task_t *old = &s_table.tasks[index];
            if ((uint64_t)field(config, "revision")->valuedouble != task_revision(old)) {
                result = ack(5, "revision_conflict");
                goto finish;
            }
            if (busy(old, now)) {
                result = ack(8, "task_busy");
                goto finish;
            }
            revision = task_revision(old) + 1U;
        } else if (replacement->count >= TIMER_MAX_TASKS) {
            result = ack(10, "limit_reached");
            goto finish;
        }
        if (cJSON_IsTrue(field(config, "enabled")) && !now->utc_trusted &&
            (id == NULL || !task_enabled(&s_table.tasks[index]))) {
            result = ack(7, "time_untrusted");
            goto finish;
        }
        if (revision > TIMER_JSON_INTEGER_MAX ||
            replacement->next_id >= TIMER_JSON_INTEGER_MAX ||
            replacement->revision >= TIMER_JSON_INTEGER_MAX) {
            result = ack(14, "internal_error");
            goto finish;
        }
        cJSON *stored = cJSON_Duplicate(config, true);
        if (stored == NULL) {
            result = ack(10, "no_memory");
            goto finish;
        }
        char allocated_id[33];
        if (id == NULL) {
            do {
                snprintf(allocated_id, sizeof(allocated_id), "%" PRIu64,
                         replacement->next_id++);
            } while (find_task(replacement, allocated_id) >= 0 &&
                     replacement->next_id < TIMER_JSON_INTEGER_MAX);
            if (!add_item(stored, "task_id", cJSON_CreateString(allocated_id)) ||
                !add_item(stored, "revision", cJSON_CreateNumber(1))) {
                cJSON_Delete(stored);
                result = ack(10, "no_memory");
                goto finish;
            }
            ++replacement->count;
        } else if (!replace_item(stored, "revision",
                                  cJSON_CreateNumber((double)revision))) {
            cJSON_Delete(stored);
            result = ack(10, "no_memory");
            goto finish;
        }
        timer_task_t *task = &replacement->tasks[index];
        cJSON_Delete(task->config);
        task->config = stored;
        task->kind = kind;
        task->period_ms = period;
        if (id == NULL) {
            task->last_day = -1;
        }
        reset_schedule(task, now, offset);
        indices[position++] = (size_t)index;
    }
    ++replacement->revision;
    result = task_response(replacement, indices, position);
    if (result != NULL) {
        esp_err_t err = commit_table(replacement);
        if (err != ESP_OK) {
            cJSON_Delete(result);
            result = storage_error(err);
        }
    }
finish:
    free_table(replacement);
    free(replacement);
    return result;
}

static cJSON *single_task(const char *mt, const cJSON *pd,
                           const gw_time_point_t *now)
{
    const cJSON *id = field(pd, "task_id");
    if (!text_valid(id, false)) {
        return ack(1, "invalid_param");
    }
    int index = find_task(&s_table, id->valuestring);
    if (index < 0) {
        return ack(6, "not_found");
    }
    size_t selected = (size_t)index;
    if (strcmp(mt, "gateway.timer.get") == 0) {
        return task_response(&s_table, &selected, 1U);
    }
    uint64_t expected;
    if (!integer(field(pd, "revision"), 1U, TIMER_JSON_INTEGER_MAX, &expected)) {
        return ack(1, "invalid_param");
    }
    timer_task_t *old = &s_table.tasks[index];
    if (expected != task_revision(old)) {
        return ack(5, "revision_conflict");
    }
    if (busy(old, now)) {
        return ack(8, "task_busy");
    }
    bool deleting = strcmp(mt, "gateway.timer.delete") == 0;
    const cJSON *enabled = field(pd, "enabled");
    if (!deleting && !cJSON_IsBool(enabled)) {
        return ack(1, "invalid_param");
    }
    if (!deleting && cJSON_IsTrue(enabled) && !task_enabled(old) &&
        !now->utc_trusted) {
        return ack(7, "time_untrusted");
    }
    if (s_table.revision >= TIMER_JSON_INTEGER_MAX ||
        (!deleting && task_revision(old) >= TIMER_JSON_INTEGER_MAX)) {
        return ack(14, "internal_error");
    }
    timer_table_t *replacement = copy_table();
    if (replacement == NULL) {
        return ack(10, "no_memory");
    }
    cJSON *result = NULL;
    if (deleting) {
        cJSON_Delete(replacement->tasks[index].config);
        memmove(&replacement->tasks[index], &replacement->tasks[index + 1],
                (replacement->count - selected - 1U) * sizeof(timer_task_t));
        --replacement->count;
        memset(&replacement->tasks[replacement->count], 0, sizeof(timer_task_t));
        result = ack(0, "ok");
        if (result != NULL &&
            !add_item(result, "task_id", cJSON_CreateString(id->valuestring))) {
            cJSON_Delete(result);
            result = NULL;
        }
    } else {
        timer_task_t *task = &replacement->tasks[index];
        bool changed = cJSON_IsTrue(enabled) != task_enabled(old);
        if (!replace_item(task->config, "enabled", cJSON_CreateBool(cJSON_IsTrue(enabled))) ||
            !replace_item(task->config, "revision",
                          cJSON_CreateNumber((double)(expected + 1U)))) {
            result = ack(10, "no_memory");
            goto finish;
        }
        if (changed) {
            reset_schedule(task, now, timezone_offset());
        }
        result = task_response(replacement, &selected, 1U);
    }
    ++replacement->revision;
    if (result != NULL) {
        esp_err_t err = commit_table(replacement);
        if (err != ESP_OK) {
            cJSON_Delete(result);
            result = storage_error(err);
        }
    }
finish:
    free_table(replacement);
    free(replacement);
    return result;
}

cJSON *gw_gateway_timer_handle(const char *mt, const cJSON *pd)
{
    if (mt == NULL || !cJSON_IsObject(pd)) {
        return ack(1, "invalid_param");
    }
    if (s_lock == NULL || !s_initialized) {
        return ack(14, "internal_error");
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        return ack(9, "timeout");
    }
    cJSON *result;
    gw_time_point_t now = gw_time_now();
    if (s_time_paused && strcmp(mt, "gateway.timer.get") != 0 &&
        strcmp(mt, "gateway.timer.list") != 0) {
        result = ack(8, "task_busy");
    } else if (strcmp(mt, "gateway.timer.list") == 0) {
        result = list_tasks(pd);
    } else if (strcmp(mt, "gateway.timer.upsert") == 0) {
        result = upsert_tasks(pd, &now);
    } else if (strcmp(mt, "gateway.timer.get") == 0 ||
               strcmp(mt, "gateway.timer.enable") == 0 ||
               strcmp(mt, "gateway.timer.delete") == 0) {
        result = single_task(mt, pd, &now);
    } else {
        result = ack(3, "unknown_message");
    }
    xSemaphoreGive(s_lock);
    return result;
}

void gw_gateway_timer_time_change_begin(void)
{
    if (s_initialized && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_time_paused = true;
        xSemaphoreGive(s_lock);
    }
}

void gw_gateway_timer_time_change_finish(bool rebase)
{
    if (s_initialized && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        if (rebase) {
            s_clock_seen = false;
        }
        s_time_paused = false;
        xSemaphoreGive(s_lock);
    }
}

static bool disable_duration(timer_table_t *table, timer_task_t *task)
{
    uint64_t revision = task_revision(task);
    if (revision >= TIMER_JSON_INTEGER_MAX || table->revision >= TIMER_JSON_INTEGER_MAX ||
        !replace_item(task->config, "enabled", cJSON_CreateBool(false)) ||
        !replace_item(task->config, "revision", cJSON_CreateNumber((double)(revision + 1U)))) {
        return false;
    }
    ++table->revision;
    task->due_uptime_valid = false;
    task->next_utc_ms = 0U;
    return true;
}

static esp_err_t reconcile_clock(const gw_time_point_t *now, int16_t offset,
                                  bool rebase)
{
    timer_table_t *replacement = copy_table();
    if (replacement == NULL) {
        return ESP_ERR_NO_MEM;
    }
    replacement->timezone_offset_min = offset;
    for (size_t i = 0; i < replacement->count; ++i) {
        timer_task_t *task = &replacement->tasks[i];
        if (!task_enabled(task)) {
            continue;
        }
        if (task->kind == TIMER_DURATION && task->consumed && !task->active) {
            if (!disable_duration(replacement, task)) {
                free_table(replacement);
                free(replacement);
                return ESP_ERR_NO_MEM;
            }
            continue;
        }
        if (!now->utc_trusted) {
            continue;
        }
        if (task->kind == TIMER_DAILY) {
            if (rebase) {
                task->next_utc_ms = daily_next(task, now->utc_ms, offset);
            }
            continue;
        }
        if (task->kind == TIMER_DURATION && task->active) {
            continue;
        }
        if (!task->due_uptime_valid) {
            if (task->next_utc_ms == 0U) {
                /* A restarted duration without a UTC anchor must never restart old work. */
                if (task->kind == TIMER_DURATION) {
                    if (!disable_duration(replacement, task)) {
                        free_table(replacement);
                        free(replacement);
                        return ESP_ERR_NO_MEM;
                    }
                    continue;
                }
                task->next_utc_ms = now->utc_ms + task->period_ms;
            }
            if (task->next_utc_ms <= now->utc_ms) {
                if (task->kind == TIMER_DURATION) {
                    if (!disable_duration(replacement, task)) {
                        free_table(replacement);
                        free(replacement);
                        return ESP_ERR_NO_MEM;
                    }
                    continue;
                }
                uint64_t missed = (now->utc_ms - task->next_utc_ms) /
                                  task->period_ms + 1U;
                task->next_utc_ms += missed * task->period_ms;
            }
            task->due_uptime_ms = now->uptime_ms + task->next_utc_ms - now->utc_ms;
            task->due_uptime_valid = true;
        } else {
            /* Small corrections preserve occurrences crossed while polling paused. */
            if (rebase && task->due_uptime_ms <= now->uptime_ms) {
                if (task->kind == TIMER_DURATION) {
                    if (!disable_duration(replacement, task)) {
                        free_table(replacement);
                        free(replacement);
                        return ESP_ERR_NO_MEM;
                    }
                    continue;
                }
                uint64_t missed = (now->uptime_ms - task->due_uptime_ms) /
                                  task->period_ms + 1U;
                task->due_uptime_ms += missed * task->period_ms;
            }
            task->next_utc_ms = now->utc_ms + task->due_uptime_ms - now->uptime_ms;
        }
    }
    esp_err_t err = commit_table(replacement);
    free_table(replacement);
    free(replacement);
    if (err == ESP_OK) {
        s_clock_seen = true;
        s_previous_clock = *now;
        s_previous_timezone = offset;
        gw_scheduler_status_t status;
        if (gw_scheduler_get_status(&status) == ESP_OK) {
            s_runtime_timezone = status.timezone_offset_min;
        }
    }
    return err;
}

static esp_err_t claim_occurrence(size_t index, const gw_time_point_t *now,
                                  int16_t offset)
{
    timer_table_t *replacement = copy_table();
    if (replacement == NULL) {
        return ESP_ERR_NO_MEM;
    }
    timer_task_t *task = &replacement->tasks[index];
    if (task->occurrence_id >= TIMER_JSON_INTEGER_MAX) {
        free_table(replacement);
        free(replacement);
        return ESP_ERR_INVALID_STATE;
    }
    ++task->occurrence_id;
    task->scheduled_utc_ms = task->next_utc_ms;
    task->active = true;
    task->finished = false;
    task->execution_error = ESP_OK;
    task->action_index = 0U;
    task->delaying = false;
    if (task->kind == TIMER_DURATION) {
        task->consumed = true;
    } else if (task->kind == TIMER_DAILY) {
        task->last_day = ((int64_t)task->scheduled_utc_ms +
                          (int64_t)offset * 60000) / (int64_t)TIMER_DAY_MS;
        task->next_utc_ms = daily_next(task, now->utc_ms, offset);
    } else {
        uint64_t missed = (now->uptime_ms - task->due_uptime_ms) /
                          task->period_ms + 1U;
        task->due_uptime_ms += missed * task->period_ms;
        task->next_utc_ms = now->utc_ms + task->due_uptime_ms - now->uptime_ms;
    }
    esp_err_t err = commit_table(replacement);
    free_table(replacement);
    free(replacement);
    return err;
}

static esp_err_t execute_control(const cJSON *action)
{
    const cJSON *type = field(action, "type");
    const cJSON *target = field(action, "target");
    uint16_t unicast = (uint16_t)field(target, "unicast_addr")->valuedouble;
    uint16_t group = (uint16_t)field(target, "group_addr")->valuedouble;
    gw_gs200_action_t command = {
        .gateway_addr = GW_GS200_DEFAULT_GATEWAY_ADDR,
        .device_addr = unicast != 0U ? unicast :
                       group != 0U ? (uint16_t)(0xC000U | group) : 0xFFFFU,
        .network_id = (uint16_t)field(target, "area_addr")->valuedouble,
        .control = GW_GS200_DEFAULT_CONTROL,
    };
    if (string_is(type, "light_on")) {
        command.kind = GW_GS200_ACTION_LIGHT_ON;
    } else if (string_is(type, "light_off")) {
        command.kind = GW_GS200_ACTION_LIGHT_OFF;
    } else {
        command.kind = GW_GS200_ACTION_SCENE_SWITCH;
        command.scene_no = (uint8_t)field(action, "scene_id")->valuedouble;
    }
    uint8_t frame[GW_GS200_ACTION_FRAME_LEN];
    size_t length = 0U;
    esp_err_t err = gw_gs200_build_action_frame(&command, frame, sizeof(frame), &length);
    return err == ESP_OK ? gw_dongle_mqtt_bridge_send_frame(frame, length, 500U) : err;
}

static void publish_fired(const timer_task_t *task)
{
    cJSON *pd = cJSON_CreateObject();
    if (pd != NULL &&
        add_item(pd, "task_id", cJSON_CreateString(task_id(task))) &&
        add_item(pd, "occurrence_id", cJSON_CreateNumber((double)task->occurrence_id)) &&
        add_item(pd, "scheduled_utc_ms", cJSON_CreateNumber((double)task->scheduled_utc_ms)) &&
        add_item(pd, "success", cJSON_CreateBool(task->execution_error == ESP_OK)) &&
        add_item(pd, "esp_err", cJSON_CreateNumber(task->execution_error))) {
        (void)gw_gateway_publish_event("gateway.timer.fired", "gateway", pd, false);
    }
    cJSON_Delete(pd);
}

static void finish_occurrence(size_t index)
{
    timer_task_t *task = &s_table.tasks[index];
    if (task->kind == TIMER_DURATION) {
        timer_table_t *replacement = copy_table();
        if (replacement == NULL) {
            return;
        }
        bool disabled = disable_duration(replacement, &replacement->tasks[index]);
        esp_err_t err = disabled ? commit_table(replacement) : ESP_ERR_NO_MEM;
        free_table(replacement);
        free(replacement);
        if (err != ESP_OK) {
            return;
        }
        task = &s_table.tasks[index];
    } else {
        gw_time_point_t now = gw_time_now();
        bool missed = task->kind == TIMER_INTERVAL
                          ? task->due_uptime_valid && task->due_uptime_ms <= now.uptime_ms
                          : now.utc_trusted && task->next_utc_ms <= now.utc_ms;
        if (missed) {
            timer_table_t *replacement = copy_table();
            if (replacement == NULL) {
                return;
            }
            timer_task_t *updated = &replacement->tasks[index];
            if (updated->kind == TIMER_INTERVAL) {
                uint64_t points = (now.uptime_ms - updated->due_uptime_ms) /
                                  updated->period_ms + 1U;
                updated->due_uptime_ms += points * updated->period_ms;
                if (now.utc_trusted) {
                    updated->next_utc_ms = now.utc_ms + updated->due_uptime_ms - now.uptime_ms;
                }
            } else {
                updated->next_utc_ms = daily_next(updated, now.utc_ms, timezone_offset());
            }
            esp_err_t err = commit_table(replacement);
            free_table(replacement);
            free(replacement);
            if (err != ESP_OK) {
                return;
            }
            task = &s_table.tasks[index];
        }
    }
    publish_fired(task);
    task->active = false;
    task->finished = false;
    task->delaying = false;
}

static void advance_action(size_t index, const gw_time_point_t *now)
{
    timer_task_t *task = &s_table.tasks[index];
    if (task->finished) {
        finish_occurrence(index);
        return;
    }
    if (task->delaying) {
        if (now->uptime_ms < task->delay_until_ms) {
            return;
        }
        task->delaying = false;
        ++task->action_index;
    }
    const cJSON *actions = field(task->config, "action");
    const cJSON *action = actions->child;
    for (size_t i = 0; action != NULL && i < task->action_index; ++i) {
        action = action->next;
    }
    while (action != NULL) {
        if (string_is(field(action, "type"), "delay")) {
            uint64_t delay = (uint64_t)field(action, "duration_ms")->valuedouble;
            uint64_t started_ms = gw_time_now().uptime_ms;
            if (delay > UINT64_MAX - started_ms) {
                task->execution_error = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (delay != 0U) {
                task->delaying = true;
                task->delay_until_ms = started_ms + delay;
                return;
            }
        } else {
            task->execution_error = execute_control(action);
            if (task->execution_error != ESP_OK) {
                break;
            }
        }
        ++task->action_index;
        action = action->next;
    }
    task->finished = true;
    finish_occurrence(index);
}

static void timer_poll(void)
{
    gw_time_point_t now = gw_time_now();
    int16_t offset = timezone_offset();
    bool rebase = !s_clock_seen ||
                  now.utc_trusted != s_previous_clock.utc_trusted ||
                  offset != s_previous_timezone;
    uint64_t expected_utc_ms = s_previous_clock.utc_ms +
                               (now.uptime_ms - s_previous_clock.uptime_ms);
    bool changed = rebase || (now.utc_trusted && now.utc_ms != expected_utc_ms);
    bool clock_ready = !s_time_paused &&
                       (!changed || reconcile_clock(&now, offset, rebase) == ESP_OK);
    if (clock_ready) {
        s_previous_clock = now;
    }
    for (size_t i = 0; i < s_table.count; ++i) {
        now = gw_time_now();
        timer_task_t *task = &s_table.tasks[i];
        if (task->active) {
            advance_action(i, &now);
            continue;
        }
        if (!clock_ready || !task_enabled(task) || !now.utc_trusted || task->consumed) {
            continue;
        }
        bool due = task->kind == TIMER_DAILY
                       ? task->next_utc_ms != 0U && task->next_utc_ms <= now.utc_ms
                       : task->due_uptime_valid && task->due_uptime_ms <= now.uptime_ms;
        if (due && claim_occurrence(i, &now, offset) == ESP_OK) {
            advance_action(i, &now);
        }
    }
}

static void timer_worker(void *argument)
{
    (void)argument;
    for (;;) {
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            timer_poll();
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
}

esp_err_t gw_gateway_timer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_table.next_id = 1U;
    gw_scheduler_status_t status;
    if (gw_scheduler_get_status(&status) == ESP_OK) {
        s_runtime_timezone = status.timezone_offset_min;
        s_table.timezone_offset_min = s_runtime_timezone;
    }
    esp_err_t err = load_table();
    if (err == ESP_OK) {
        gw_time_point_t now = gw_time_now();
        err = reconcile_clock(&now, timezone_offset(), true);
    }
    if (err == ESP_OK &&
        xTaskCreate(timer_worker, "gateway_timer", 6144U, NULL, 5U, &s_worker) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        s_initialized = true;
    } else {
        free_table(&s_table);
        s_clock_seen = false;
    }
    xSemaphoreGive(s_lock);
    return err;
}
