#include "gw_scheduler.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "gw_event.h"
#include "gw_command_dispatcher.h"
#include "gw_defaults.h"
#include "gw_gs200_protocol.h"
#include "gw_param.h"
#include "gw_result.h"
#include "gw_storage.h"
#include "gw_time.h"
#include "gw_ymzl_protocol.h"

/*
 * 这两个参数应最终登记到 gw_param.h。先提供回退值，使调度核心可以独立集成；
 * key="table" 为加密存储预留 `_bak` 后缀后仍满足 NVS 的 15 字符限制。
 */
#ifndef GW_NVS_NAMESPACE_SCHEDULE
#define GW_NVS_NAMESPACE_SCHEDULE "schedule"
#endif

#ifndef GW_PARAM_KEY_SCHEDULE_TABLE
#define GW_PARAM_KEY_SCHEDULE_TABLE "table"
#endif

#define GW_SCHEDULER_TABLE_VERSION             2U
#define GW_SCHEDULER_RUNTIME_RECORD_VERSION    2U
#define GW_SCHEDULER_MIN_TIMEZONE_OFFSET_MIN   (-720)
#define GW_SCHEDULER_MAX_TIMEZONE_OFFSET_MIN   840
#define GW_SCHEDULER_LOCAL_DAY_NEVER           INT32_MIN
#define GW_SCHEDULER_LOCAL_MINUTE_NEVER        INT64_MIN
#define GW_SCHEDULER_MS_PER_MINUTE             (60ULL * 1000ULL)
#define GW_SCHEDULER_MS_PER_HOUR               (60ULL * GW_SCHEDULER_MS_PER_MINUTE)
#define GW_SCHEDULER_MS_PER_DAY                (24ULL * GW_SCHEDULER_MS_PER_HOUR)
#define GW_SCHEDULER_MS_PER_WEEK               (7ULL * GW_SCHEDULER_MS_PER_DAY)
#define GW_SCHEDULER_MAX_DURATION_MS            (52ULL * GW_SCHEDULER_MS_PER_WEEK)

static const char *TAG = "gw_scheduler";

/** @brief NVS 中单个任务的固定版本化表示，不直接暴露给配置入口。 */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t revision;
    uint8_t enabled;
    uint8_t type;
    uint8_t duration_unit;
    uint8_t daily_hour;
    uint8_t daily_minute;
    uint8_t action_type;
    uint8_t action_len;
    uint8_t reserved0;
    uint32_t duration_value;
    uint64_t duration_deadline_utc_ms;
    int32_t last_fired_local_day;
    int64_t last_fired_local_minute;
    char name[GW_SCHEDULER_NAME_MAX_BYTES + 1U];
    uint8_t action_data[GW_SCHEDULER_ACTION_DATA_MAX_BYTES];
    char cron_expression[GW_CRON_EXPRESSION_MAX_BYTES + 1U];
    char command_name[GW_SCHEDULER_COMMAND_NAME_MAX_BYTES + 1U];
    uint16_t command_version;
    uint16_t command_args_len;
    char command_args_json[GW_SCHEDULER_COMMAND_ARGS_MAX_BYTES + 1U];
} gw_scheduler_persist_entry_t;

/** @brief 整张任务表的加密 NVS 明文格式。 */
typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint16_t version;
    uint16_t table_size;
    uint32_t table_revision;
    int16_t timezone_offset_min;
    uint16_t task_count;
    uint32_t next_task_id;
    uint64_t next_occurrence_id;
    uint8_t reserved[8];
    gw_scheduler_persist_entry_t entries[GW_SCHEDULER_MAX_TASKS];
    uint32_t crc32;
} gw_scheduler_persist_table_t;

/* V1 仅用于启动时迁移，字段布局必须与已发布固件完全一致。 */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t revision;
    uint8_t enabled;
    uint8_t type;
    uint8_t duration_unit;
    uint8_t daily_hour;
    uint8_t daily_minute;
    uint8_t action_type;
    uint8_t action_len;
    uint8_t reserved0;
    uint32_t duration_value;
    uint64_t duration_deadline_utc_ms;
    int32_t last_fired_local_day;
    char name[GW_SCHEDULER_NAME_MAX_BYTES + 1U];
    uint8_t action_data[GW_SCHEDULER_ACTION_DATA_MAX_BYTES];
} gw_scheduler_persist_entry_v1_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint16_t version;
    uint16_t table_size;
    uint32_t table_revision;
    int16_t timezone_offset_min;
    uint16_t task_count;
    uint32_t next_task_id;
    uint64_t next_occurrence_id;
    uint8_t reserved[8];
    gw_scheduler_persist_entry_v1_t entries[GW_SCHEDULER_MAX_TASKS];
    uint32_t crc32;
} gw_scheduler_persist_table_v1_t;

/**
 * @brief 单任务执行游标的紧凑持久化记录。
 *
 * 配置表只在 CRUD、时区或时间轴重基准时保存；高频 interval 到期时仅更新本记录，
 * 避免为几十字节的执行进度反复重写约 2 KiB 的完整任务表。table_revision 和
 * task_revision 共同隔离旧配置留下的游标，只有与当前配置完全匹配的记录才会恢复。
 */
typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version;
    uint8_t type;
    uint8_t enabled;
    uint8_t reserved;
    uint32_t task_id;
    uint32_t task_revision;
    uint32_t table_revision;
    uint32_t runtime_epoch;
    uint64_t occurrence_id;
    uint64_t duration_deadline_utc_ms;
    int32_t last_fired_local_day;
    int64_t last_fired_local_minute;
} gw_scheduler_runtime_record_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version;
    uint8_t type;
    uint8_t enabled;
    uint8_t reserved;
    uint32_t task_id;
    uint32_t task_revision;
    uint32_t table_revision;
    uint32_t runtime_epoch;
    uint64_t occurrence_id;
    uint64_t duration_deadline_utc_ms;
    int32_t last_fired_local_day;
} gw_scheduler_runtime_record_v1_t;

/** @brief 不对外持久化为公共结构的任务执行状态。 */
typedef struct {
    uint64_t duration_deadline_utc_ms;
    uint64_t duration_due_uptime_ms;
    uint64_t reserved_occurrence_id;
    uint64_t pending_scheduled_utc_ms;
    int32_t last_fired_local_day;
    int64_t last_fired_local_minute;
    gw_cron_schedule_t cron_schedule;
    bool cron_schedule_valid;
    bool duration_uptime_valid;
    bool daily_needs_reconcile;
    bool cron_needs_reconcile;
    bool dispatch_pending;
    bool dispatch_progress_persisted;
} gw_scheduler_runtime_t;

/** @brief CRUD 保存失败时用于恢复内存原状态。 */
typedef struct {
    gw_scheduler_task_t tasks[GW_SCHEDULER_MAX_TASKS];
    gw_scheduler_runtime_t runtime[GW_SCHEDULER_MAX_TASKS];
    size_t task_count;
    uint32_t table_revision;
    uint32_t next_task_id;
    uint64_t next_occurrence_id;
    uint32_t runtime_epoch;
    int16_t timezone_offset_min;
    bool persistence_dirty;
} gw_scheduler_state_backup_t;

_Static_assert(GW_SCHEDULER_MAX_TASKS == 16U, "scheduler persistent format requires 16 slots");
_Static_assert(sizeof(gw_scheduler_persist_table_t) <= UINT16_MAX,
               "scheduler persistent table too large");
_Static_assert(sizeof(gw_scheduler_runtime_record_t) == 52U,
               "scheduler runtime record layout changed");
_Static_assert(sizeof(gw_scheduler_runtime_record_v1_t) == 44U,
               "scheduler V1 runtime record layout changed");

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_time_change_lock;
static bool s_initialized;
static bool s_persistence_dirty;
static bool s_time_change_paused;
static TaskHandle_t s_time_change_owner;
#if GW_SCHEDULER_SERVICE_ENABLED
static gw_scheduler_task_t s_tasks[GW_SCHEDULER_MAX_TASKS];
static gw_scheduler_runtime_t s_runtime[GW_SCHEDULER_MAX_TASKS];
#endif
static size_t s_task_count;
static uint32_t s_table_revision;
static uint32_t s_next_task_id = 1U;
static uint64_t s_next_occurrence_id = 1ULL;
static uint32_t s_runtime_epoch;
static int16_t s_timezone_offset_min;
static esp_err_t s_last_error = ESP_OK;

static esp_err_t scheduler_lock(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void scheduler_unlock(void)
{
    (void)xSemaphoreGive(s_lock);
}

#if GW_SCHEDULER_SERVICE_ENABLED

/** @brief 授时暂停期间只允许持有事务的任务修改调度表。调用方已持有 s_lock。 */
static bool scheduler_mutation_allowed_locked(void)
{
    return !s_time_change_paused ||
           s_time_change_owner == xTaskGetCurrentTaskHandle();
}

static uint32_t increment_u32_nonzero(uint32_t value)
{
    return value == UINT32_MAX ? 1U : value + 1U;
}

static uint64_t increment_u64_nonzero(uint64_t value)
{
    return value == UINT64_MAX ? 1ULL : value + 1ULL;
}

/** @brief 生成一次任务表生命周期标识，隔离删表/损坏恢复前遗留的动态游标。 */
static uint32_t make_runtime_epoch(void)
{
    uint32_t epoch = esp_random();
    return epoch == 0U ? 0x59585231U : epoch;
}

static bool timezone_offset_valid(int16_t offset_min)
{
    return offset_min >= GW_SCHEDULER_MIN_TIMEZONE_OFFSET_MIN &&
           offset_min <= GW_SCHEDULER_MAX_TIMEZONE_OFFSET_MIN;
}

static uint64_t duration_unit_ms(gw_scheduler_duration_unit_t unit)
{
    switch (unit) {
    case GW_SCHEDULER_DURATION_MINUTE:
        return GW_SCHEDULER_MS_PER_MINUTE;
    case GW_SCHEDULER_DURATION_HOUR:
        return GW_SCHEDULER_MS_PER_HOUR;
    case GW_SCHEDULER_DURATION_DAY:
        return GW_SCHEDULER_MS_PER_DAY;
    case GW_SCHEDULER_DURATION_WEEK:
        return GW_SCHEDULER_MS_PER_WEEK;
    default:
        return 0;
    }
}

/** @brief 判断任务是否使用“间隔值 + 单调 uptime 截止点”的调度模型。 */
static bool task_uses_elapsed_interval(gw_scheduler_task_type_t type)
{
    return type == GW_SCHEDULER_TASK_DURATION ||
           type == GW_SCHEDULER_TASK_INTERVAL;
}

static esp_err_t validate_task(const gw_scheduler_task_t *task)
{
    if (task == NULL ||
        memchr(task->name, '\0', sizeof(task->name)) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (task->type == GW_SCHEDULER_TASK_DURATION) {
        uint64_t unit_ms = duration_unit_ms(task->trigger.duration.unit);
        if (task->trigger.duration.value == 0U || unit_ms == 0U ||
            (uint64_t)task->trigger.duration.value > UINT64_MAX / unit_ms ||
            (uint64_t)task->trigger.duration.value * unit_ms >
                GW_SCHEDULER_MAX_DURATION_MS) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (task->type == GW_SCHEDULER_TASK_INTERVAL) {
        uint64_t unit_ms = duration_unit_ms(task->trigger.duration.unit);
        if (task->trigger.duration.value == 0U ||
            (task->trigger.duration.unit != GW_SCHEDULER_DURATION_MINUTE &&
             task->trigger.duration.unit != GW_SCHEDULER_DURATION_HOUR) ||
            unit_ms == 0U ||
            (uint64_t)task->trigger.duration.value > UINT64_MAX / unit_ms ||
            (uint64_t)task->trigger.duration.value * unit_ms >
                GW_SCHEDULER_MAX_DURATION_MS) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (task->type == GW_SCHEDULER_TASK_DAILY) {
        if (task->trigger.daily.hour > 23U || task->trigger.daily.minute > 59U) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (task->type == GW_SCHEDULER_TASK_CRON) {
        gw_cron_schedule_t parsed;
        if (memchr(task->trigger.cron.expression,
                   '\0',
                   sizeof(task->trigger.cron.expression)) == NULL ||
            gw_cron_parse(task->trigger.cron.expression, &parsed) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (task->action_type == GW_SCHEDULER_ACTION_NOTIFY_ONLY ||
        task->action_type == GW_SCHEDULER_ACTION_BOARD_LED_ON ||
        task->action_type == GW_SCHEDULER_ACTION_BOARD_LED_OFF) {
        if (task->action_len != 0U) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (task->action_type == GW_SCHEDULER_ACTION_DONGLE_RAW) {
        if (gw_ymzl_frame_validate(task->action_data, task->action_len) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (task->action_type == GW_SCHEDULER_ACTION_LIGHT_ON ||
               task->action_type == GW_SCHEDULER_ACTION_LIGHT_OFF ||
               task->action_type == GW_SCHEDULER_ACTION_SCENE_SWITCH) {
        gw_gs200_action_t action;
        if (gw_gs200_parse_action_frame(task->action_data,
                                        task->action_len,
                                        &action) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }

        bool kind_matches =
            (task->action_type == GW_SCHEDULER_ACTION_LIGHT_ON &&
             action.kind == GW_GS200_ACTION_LIGHT_ON) ||
            (task->action_type == GW_SCHEDULER_ACTION_LIGHT_OFF &&
             action.kind == GW_GS200_ACTION_LIGHT_OFF) ||
            (task->action_type == GW_SCHEDULER_ACTION_SCENE_SWITCH &&
             action.kind == GW_GS200_ACTION_SCENE_SWITCH);
        if (!kind_matches) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (task->action_type == GW_SCHEDULER_ACTION_COMMAND) {
        if (memchr(task->command_name, '\0', sizeof(task->command_name)) == NULL ||
            task->command_name[0] == '\0' || task->command_version == 0U ||
            task->command_args_len == 0U ||
            task->command_args_len > GW_SCHEDULER_COMMAND_ARGS_MAX_BYTES ||
            task->command_args_json[task->command_args_len] != '\0' ||
            gw_command_validate(task->command_name,
                                task->command_version,
                                task->command_args_json,
                                task->command_args_len) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        if (task->action_len != 0U) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/** @brief 清除输入结构未使用的字节，避免把调用方栈残留写入 NVS。 */
static void canonicalize_task(const gw_scheduler_task_t *source, gw_scheduler_task_t *target)
{
    memset(target, 0, sizeof(*target));
    target->id = source->id;
    target->revision = source->revision;
    target->enabled = source->enabled;
    target->type = source->type;
    if (task_uses_elapsed_interval(source->type)) {
        target->trigger.duration = source->trigger.duration;
    } else if (source->type == GW_SCHEDULER_TASK_DAILY) {
        target->trigger.daily = source->trigger.daily;
    } else {
        strlcpy(target->trigger.cron.expression,
                source->trigger.cron.expression,
                sizeof(target->trigger.cron.expression));
    }
    target->action_type = source->action_type;
    target->action_len = source->action_len;
    memcpy(target->action_data, source->action_data, source->action_len);
    if (source->action_type == GW_SCHEDULER_ACTION_COMMAND) {
        strlcpy(target->command_name,
                source->command_name,
                sizeof(target->command_name));
        target->command_version = source->command_version;
        target->command_args_len = source->command_args_len;
        memcpy(target->command_args_json,
               source->command_args_json,
               source->command_args_len);
        target->command_args_json[source->command_args_len] = '\0';
    }

    size_t name_len = strnlen(source->name, sizeof(source->name));
    memcpy(target->name, source->name, name_len);
    target->name[name_len] = '\0';
}

static int find_task_index_locked(uint32_t id)
{
    for (size_t i = 0; i < s_task_count; ++i) {
        if (s_tasks[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t allocate_task_id_locked(void)
{
    uint32_t candidate = s_next_task_id == 0U ? 1U : s_next_task_id;

    /* 最多只有 16 个 ID 被占用，连续检查 17 次必能找到空闲值。 */
    for (size_t attempt = 0; attempt <= GW_SCHEDULER_MAX_TASKS; ++attempt) {
        if (find_task_index_locked(candidate) < 0) {
            s_next_task_id = increment_u32_nonzero(candidate);
            return candidate;
        }
        candidate = increment_u32_nonzero(candidate);
    }
    return 0U;
}

static esp_err_t compute_duration_deadlines(const gw_scheduler_task_t *task,
                                            const gw_time_point_t *now,
                                            uint64_t *out_utc_ms,
                                            uint64_t *out_uptime_ms)
{
    if (task == NULL || now == NULL || out_utc_ms == NULL || out_uptime_ms == NULL ||
        !now->utc_trusted) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t unit_ms = duration_unit_ms(task->trigger.duration.unit);
    if (unit_ms == 0U || task->trigger.duration.value == 0U ||
        (uint64_t)task->trigger.duration.value > UINT64_MAX / unit_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t interval_ms = (uint64_t)task->trigger.duration.value * unit_ms;
    if (interval_ms > GW_SCHEDULER_MAX_DURATION_MS ||
        now->utc_ms > UINT64_MAX - interval_ms ||
        now->uptime_ms > UINT64_MAX - interval_ms) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_utc_ms = now->utc_ms + interval_ms;
    *out_uptime_ms = now->uptime_ms + interval_ms;
    return ESP_OK;
}

/**
 * @brief 用重启后恢复出的 UTC 截止点建立本次启动的单调截止点。
 *
 * 一旦建立成功，后续校时不会再次计算，从而避免系统时间跳变缩短或延长倒计时。
 */
static void rebuild_duration_runtime_locked(size_t index, const gw_time_point_t *now)
{
    if (index >= s_task_count || now == NULL || !now->utc_trusted ||
        !s_tasks[index].enabled || !task_uses_elapsed_interval(s_tasks[index].type) ||
        s_runtime[index].duration_uptime_valid ||
        s_runtime[index].duration_deadline_utc_ms == 0U ||
        s_runtime[index].duration_deadline_utc_ms <= now->utc_ms) {
        return;
    }

    uint64_t delta_ms = s_runtime[index].duration_deadline_utc_ms - now->utc_ms;

    if (now->uptime_ms > UINT64_MAX - delta_ms) {
        s_runtime[index].duration_due_uptime_ms = UINT64_MAX;
    } else {
        s_runtime[index].duration_due_uptime_ms = now->uptime_ms + delta_ms;
    }
    s_runtime[index].duration_uptime_valid = true;
}

/**
 * @brief 循环任务触发后推进到严格晚于当前 uptime 的下一次，跳过积压次数。
 *
 * scheduled UTC 与 uptime 使用同一个步数推进；这样连续开机时不受墙钟微调影响，
 * 同时把下一次理论 UTC 落盘用于重启恢复。
 */
static esp_err_t advance_interval_runtime_locked(size_t index,
                                                 const gw_time_point_t *now)
{
    if (index >= s_task_count || now == NULL ||
        s_tasks[index].type != GW_SCHEDULER_TASK_INTERVAL ||
        !s_runtime[index].duration_uptime_valid) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t unit_ms = duration_unit_ms(s_tasks[index].trigger.duration.unit);
    uint32_t value = s_tasks[index].trigger.duration.value;
    if (unit_ms == 0U || value == 0U ||
        (uint64_t)value > UINT64_MAX / unit_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t interval_ms = (uint64_t)value * unit_ms;

    uint64_t steps = 1U;
    if (now->uptime_ms >= s_runtime[index].duration_due_uptime_ms) {
        steps = ((now->uptime_ms - s_runtime[index].duration_due_uptime_ms) /
                 interval_ms) + 1U;
    }
    if (steps > UINT64_MAX / interval_ms) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t advance_ms = steps * interval_ms;
    if (s_runtime[index].duration_due_uptime_ms > UINT64_MAX - advance_ms ||
        s_runtime[index].duration_deadline_utc_ms > UINT64_MAX - advance_ms) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_runtime[index].duration_due_uptime_ms += advance_ms;
    s_runtime[index].duration_deadline_utc_ms += advance_ms;
    return ESP_OK;
}

static void make_persist_entry(const gw_scheduler_task_t *task,
                               const gw_scheduler_runtime_t *runtime,
                               gw_scheduler_persist_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->id = task->id;
    entry->revision = task->revision;
    entry->enabled = task->enabled ? 1U : 0U;
    entry->type = (uint8_t)task->type;
    if (task_uses_elapsed_interval(task->type)) {
        entry->duration_unit = (uint8_t)task->trigger.duration.unit;
        entry->duration_value = task->trigger.duration.value;
        entry->duration_deadline_utc_ms = runtime->duration_deadline_utc_ms;
    } else if (task->type == GW_SCHEDULER_TASK_DAILY) {
        entry->daily_hour = task->trigger.daily.hour;
        entry->daily_minute = task->trigger.daily.minute;
    } else {
        memcpy(entry->cron_expression,
               task->trigger.cron.expression,
               sizeof(entry->cron_expression));
    }
    entry->last_fired_local_day = runtime->last_fired_local_day;
    entry->last_fired_local_minute = runtime->last_fired_local_minute;
    entry->action_type = (uint8_t)task->action_type;
    entry->action_len = task->action_len;
    memcpy(entry->name, task->name, sizeof(entry->name));
    memcpy(entry->action_data, task->action_data, task->action_len);
    if (task->action_type == GW_SCHEDULER_ACTION_COMMAND) {
        memcpy(entry->command_name, task->command_name, sizeof(entry->command_name));
        entry->command_version = task->command_version;
        entry->command_args_len = task->command_args_len;
        memcpy(entry->command_args_json,
               task->command_args_json,
               task->command_args_len + 1U);
    }
}

static esp_err_t parse_persist_entry(const gw_scheduler_persist_entry_t *entry,
                                     gw_scheduler_task_t *task,
                                     gw_scheduler_runtime_t *runtime)
{
    if (entry == NULL || task == NULL || runtime == NULL ||
        entry->enabled > 1U || entry->id == 0U || entry->revision == 0U ||
        memchr(entry->name, '\0', sizeof(entry->name)) == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(task, 0, sizeof(*task));
    memset(runtime, 0, sizeof(*runtime));
    task->id = entry->id;
    task->revision = entry->revision;
    task->enabled = entry->enabled != 0U;
    task->type = (gw_scheduler_task_type_t)entry->type;
    memcpy(task->name, entry->name, sizeof(task->name));
    if (task_uses_elapsed_interval(task->type)) {
        task->trigger.duration.value = entry->duration_value;
        task->trigger.duration.unit = (gw_scheduler_duration_unit_t)entry->duration_unit;
        runtime->duration_deadline_utc_ms = entry->duration_deadline_utc_ms;
        if ((task->enabled && runtime->duration_deadline_utc_ms == 0U) ||
            (!task->enabled && runtime->duration_deadline_utc_ms != 0U)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    } else if (task->type == GW_SCHEDULER_TASK_DAILY) {
        task->trigger.daily.hour = entry->daily_hour;
        task->trigger.daily.minute = entry->daily_minute;
        if (entry->duration_deadline_utc_ms != 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    } else if (task->type == GW_SCHEDULER_TASK_CRON) {
        memcpy(task->trigger.cron.expression,
               entry->cron_expression,
               sizeof(task->trigger.cron.expression));
        if (entry->duration_deadline_utc_ms != 0U ||
            gw_cron_parse(task->trigger.cron.expression,
                          &runtime->cron_schedule) != ESP_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        runtime->cron_schedule_valid = true;
        runtime->cron_needs_reconcile = task->enabled;
    }
    task->action_type = (gw_scheduler_action_type_t)entry->action_type;
    task->action_len = entry->action_len;
    if (task->action_len <= sizeof(task->action_data)) {
        memcpy(task->action_data, entry->action_data, task->action_len);
    }
    runtime->last_fired_local_day = entry->last_fired_local_day;
    runtime->last_fired_local_minute = entry->last_fired_local_minute;
    if (task->action_type == GW_SCHEDULER_ACTION_COMMAND) {
        if (entry->command_args_len > GW_SCHEDULER_COMMAND_ARGS_MAX_BYTES ||
            entry->command_name[GW_SCHEDULER_COMMAND_NAME_MAX_BYTES] != '\0' ||
            entry->command_args_json[entry->command_args_len] != '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        memcpy(task->command_name, entry->command_name, sizeof(task->command_name));
        task->command_version = entry->command_version;
        task->command_args_len = entry->command_args_len;
        memcpy(task->command_args_json,
               entry->command_args_json,
               task->command_args_len + 1U);
    }

    return validate_task(task) == ESP_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static uint32_t persist_table_crc(const gw_scheduler_persist_table_t *table)
{
    return esp_crc32_le(0,
                        (const uint8_t *)table,
                        (uint32_t)offsetof(gw_scheduler_persist_table_t, crc32));
}

static uint32_t persist_table_v1_crc(const gw_scheduler_persist_table_v1_t *table)
{
    return esp_crc32_le(0,
                        (const uint8_t *)table,
                        (uint32_t)offsetof(gw_scheduler_persist_table_v1_t, crc32));
}

/** @brief 为任务执行游标生成带主备后缀余量的动态 NVS key。 */
static esp_err_t make_runtime_record_key(uint32_t task_id,
                                         char out_key[GW_NVS_KEY_MAX_BYTES])
{
    if (task_id == 0U || out_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out_key,
                           GW_NVS_KEY_MAX_BYTES,
                           "r%08lX",
                           (unsigned long)task_id);
    return written == 9 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/** @brief 持久化已经领取的单次 occurrence 进度；调用方必须持有调度锁。 */
static esp_err_t save_runtime_record_locked(size_t index, uint64_t occurrence_id)
{
    if (index >= s_task_count || occurrence_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[GW_NVS_KEY_MAX_BYTES] = {0};
    esp_err_t err = make_runtime_record_key(s_tasks[index].id, key);
    if (err != ESP_OK) {
        s_last_error = err;
        return err;
    }

    gw_scheduler_runtime_record_t record = {
        .magic = {'Y', 'X', 'R', '1'},
        .version = GW_SCHEDULER_RUNTIME_RECORD_VERSION,
        .type = (uint8_t)s_tasks[index].type,
        .enabled = s_tasks[index].enabled ? 1U : 0U,
        .reserved = 0U,
        .task_id = s_tasks[index].id,
        .task_revision = s_tasks[index].revision,
        .table_revision = s_table_revision,
        .runtime_epoch = s_runtime_epoch,
        .occurrence_id = occurrence_id,
        .duration_deadline_utc_ms =
            task_uses_elapsed_interval(s_tasks[index].type)
                ? s_runtime[index].duration_deadline_utc_ms
                : 0U,
        .last_fired_local_day = s_runtime[index].last_fired_local_day,
        .last_fired_local_minute = s_runtime[index].last_fired_local_minute,
    };
    err = gw_storage_secure_set_blob(GW_NVS_NAMESPACE_SCHEDULE,
                                     key,
                                     GW_STORAGE_VALUE_BLOB,
                                     &record,
                                     sizeof(record));
    s_last_error = err;
    return err;
}

/**
 * @brief 运行游标存在但不可验证时关闭单个任务，防止回退到旧整表后重复触发。
 *
 * 任务配置仍保留给 APP 查看和重新启用；下一次完整表保存会提升 table_revision，
 * 使损坏或旧版本游标永久失配。调用方必须持有调度锁。
 */
static void fail_close_runtime_record_locked(size_t index, esp_err_t reason)
{
    if (index >= s_task_count) {
        return;
    }

    s_tasks[index].enabled = false;
    s_runtime[index].duration_deadline_utc_ms = 0U;
    s_runtime[index].duration_due_uptime_ms = 0U;
    s_runtime[index].reserved_occurrence_id = 0U;
    s_runtime[index].pending_scheduled_utc_ms = 0U;
    s_runtime[index].last_fired_local_day = GW_SCHEDULER_LOCAL_DAY_NEVER;
    s_runtime[index].last_fired_local_minute = GW_SCHEDULER_LOCAL_MINUTE_NEVER;
    s_runtime[index].duration_uptime_valid = false;
    s_runtime[index].daily_needs_reconcile = false;
    s_runtime[index].dispatch_pending = false;
    s_runtime[index].dispatch_progress_persisted = false;
    s_persistence_dirty = true;
    s_last_error = reason;

    ESP_LOGE(TAG,
             "runtime record fail-closed: task=%lu err=%s",
             (unsigned long)s_tasks[index].id,
             esp_err_to_name(reason));
}

/** @brief 校验并恢复与当前配置版本匹配的单任务执行游标。 */
static esp_err_t load_runtime_record_locked(size_t index, bool *out_applied)
{
    if (index >= s_task_count || out_applied == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_applied = false;

    char key[GW_NVS_KEY_MAX_BYTES] = {0};
    esp_err_t err = make_runtime_record_key(s_tasks[index].id, key);
    if (err != ESP_OK) {
        return err;
    }

    gw_scheduler_runtime_record_t record = {0};
    size_t out_len = 0U;
    err = gw_storage_secure_get_blob(GW_NVS_NAMESPACE_SCHEDULE,
                                     key,
                                     GW_STORAGE_VALUE_BLOB,
                                     &record,
                                     sizeof(record),
                                     &out_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        /*
         * 不能回退到可能早于“已领取”状态的完整表，否则 RTC 小幅回拨时可能重放
         * 同一个 occurrence。只关闭受影响任务，其余任务和网关业务继续启动。
         */
        fail_close_runtime_record_locked(index, err);
        return ESP_OK;
    }

    bool is_v1 = out_len == sizeof(gw_scheduler_runtime_record_v1_t) &&
                 record.version == 1U;
    bool envelope_valid = (out_len == sizeof(record) || is_v1) &&
                          record.magic[0] == 'Y' && record.magic[1] == 'X' &&
                          record.magic[2] == 'R' && record.magic[3] == '1' &&
                          (record.version == GW_SCHEDULER_RUNTIME_RECORD_VERSION || is_v1) &&
                          record.reserved == 0U &&
                          record.occurrence_id != 0U;
    if (!envelope_valid) {
        fail_close_runtime_record_locked(index, ESP_ERR_INVALID_RESPONSE);
        return ESP_OK;
    }
    if (is_v1) {
        record.last_fired_local_minute = GW_SCHEDULER_LOCAL_MINUTE_NEVER;
    }

    if (record.task_revision != s_tasks[index].revision ||
        record.table_revision != s_table_revision ||
        record.runtime_epoch != s_runtime_epoch) {
        /* CRUD/时区/时间重基准已生成更新的完整表，旧版本游标自然失效。 */
        return ESP_OK;
    }
    if (record.task_id != s_tasks[index].id ||
        record.type != (uint8_t)s_tasks[index].type) {
        fail_close_runtime_record_locked(index, ESP_ERR_INVALID_RESPONSE);
        return ESP_OK;
    }

    bool progress_valid = false;
    if (s_tasks[index].type == GW_SCHEDULER_TASK_DURATION) {
        progress_valid = record.enabled == 0U &&
                         record.duration_deadline_utc_ms == 0U;
    } else if (s_tasks[index].type == GW_SCHEDULER_TASK_INTERVAL) {
        progress_valid = record.enabled == 1U &&
                         record.duration_deadline_utc_ms != 0U;
    } else if (s_tasks[index].type == GW_SCHEDULER_TASK_DAILY) {
        progress_valid = record.enabled == 1U &&
                         record.duration_deadline_utc_ms == 0U &&
                         record.last_fired_local_day !=
                             GW_SCHEDULER_LOCAL_DAY_NEVER;
    } else if (s_tasks[index].type == GW_SCHEDULER_TASK_CRON) {
        progress_valid = record.enabled == 1U &&
                         record.duration_deadline_utc_ms == 0U &&
                         record.last_fired_local_minute !=
                             GW_SCHEDULER_LOCAL_MINUTE_NEVER;
    }
    if (!progress_valid || !s_tasks[index].enabled) {
        fail_close_runtime_record_locked(index, ESP_ERR_INVALID_RESPONSE);
        return ESP_OK;
    }

    if (s_tasks[index].type == GW_SCHEDULER_TASK_DURATION) {
        s_tasks[index].enabled = false;
        s_runtime[index].duration_deadline_utc_ms = 0U;
    } else if (s_tasks[index].type == GW_SCHEDULER_TASK_INTERVAL) {
        s_runtime[index].duration_deadline_utc_ms =
            record.duration_deadline_utc_ms;
    } else if (s_tasks[index].type == GW_SCHEDULER_TASK_DAILY) {
        s_runtime[index].last_fired_local_day =
            record.last_fired_local_day;
    } else {
        s_runtime[index].last_fired_local_minute =
            record.last_fired_local_minute;
    }
    s_runtime[index].duration_due_uptime_ms = 0U;
    s_runtime[index].duration_uptime_valid = false;
    s_runtime[index].reserved_occurrence_id = 0U;
    s_runtime[index].pending_scheduled_utc_ms = 0U;
    s_runtime[index].dispatch_pending = false;
    s_runtime[index].dispatch_progress_persisted = false;

    if (record.occurrence_id >= s_next_occurrence_id) {
        s_next_occurrence_id = increment_u64_nonzero(record.occurrence_id);
    }
    s_persistence_dirty = true;
    *out_applied = true;
    return ESP_OK;
}

/** @brief 删除任务后尽力清理动态执行游标；旧游标即使残留也会被版本校验隔离。 */
static void erase_runtime_record_best_effort(uint32_t task_id)
{
    char key[GW_NVS_KEY_MAX_BYTES] = {0};
    esp_err_t err = make_runtime_record_key(task_id, key);
    if (err == ESP_OK) {
        err = gw_storage_secure_erase_key(GW_NVS_NAMESPACE_SCHEDULE, key);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "erase runtime record failed: task=%lu err=%s",
                 (unsigned long)task_id,
                 esp_err_to_name(err));
    }
}

static esp_err_t save_table_locked(void)
{
    gw_scheduler_persist_table_t *table = calloc(1, sizeof(*table));
    if (table == NULL) {
        s_last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }

    table->magic[0] = 'Y';
    table->magic[1] = 'X';
    table->magic[2] = 'T';
    table->magic[3] = '1';
    table->version = GW_SCHEDULER_TABLE_VERSION;
    table->table_size = (uint16_t)sizeof(*table);
    table->table_revision = increment_u32_nonzero(s_table_revision);
    table->timezone_offset_min = s_timezone_offset_min;
    table->task_count = (uint16_t)s_task_count;
    table->next_task_id = s_next_task_id == 0U ? 1U : s_next_task_id;
    table->next_occurrence_id = s_next_occurrence_id == 0U ? 1ULL : s_next_occurrence_id;
    memcpy(table->reserved, &s_runtime_epoch, sizeof(s_runtime_epoch));
    for (size_t i = 0; i < s_task_count; ++i) {
        make_persist_entry(&s_tasks[i], &s_runtime[i], &table->entries[i]);
    }
    table->crc32 = persist_table_crc(table);

    esp_err_t err = gw_storage_secure_set_blob(GW_NVS_NAMESPACE_SCHEDULE,
                                               GW_PARAM_KEY_SCHEDULE_TABLE,
                                               GW_STORAGE_VALUE_BLOB,
                                               table,
                                               sizeof(*table));
    if (err == ESP_OK) {
        s_table_revision = table->table_revision;
        s_persistence_dirty = false;
        s_last_error = ESP_OK;
    } else {
        s_last_error = err;
    }
    free(table);
    return err;
}

static esp_err_t load_table_locked(void)
{
    gw_scheduler_persist_table_t *table = calloc(1, sizeof(*table));
    if (table == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t out_len = 0;
    esp_err_t err = gw_storage_secure_get_blob(GW_NVS_NAMESPACE_SCHEDULE,
                                               GW_PARAM_KEY_SCHEDULE_TABLE,
                                               GW_STORAGE_VALUE_BLOB,
                                               table,
                                               sizeof(*table),
                                               &out_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        free(table);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        free(table);
        return err;
    }

    bool migrated_v1 = false;
    if (out_len == sizeof(gw_scheduler_persist_table_v1_t)) {
        gw_scheduler_persist_table_v1_t *v1 = (gw_scheduler_persist_table_v1_t *)table;
        bool v1_valid = v1->magic[0] == 'Y' && v1->magic[1] == 'X' &&
                        v1->magic[2] == 'T' && v1->magic[3] == '1' &&
                        v1->version == 1U &&
                        v1->table_size == sizeof(*v1) &&
                        v1->task_count <= GW_SCHEDULER_MAX_TASKS &&
                        timezone_offset_valid(v1->timezone_offset_min) &&
                        v1->crc32 == persist_table_v1_crc(v1);
        if (!v1_valid) {
            free(table);
            return ESP_ERR_INVALID_RESPONSE;
        }

        gw_scheduler_persist_table_v1_t *copy = malloc(sizeof(*copy));
        if (copy == NULL) {
            free(table);
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, v1, sizeof(*copy));
        memset(table, 0, sizeof(*table));
        memcpy(table->magic, copy->magic, sizeof(table->magic));
        table->version = GW_SCHEDULER_TABLE_VERSION;
        table->table_size = (uint16_t)sizeof(*table);
        table->table_revision = copy->table_revision;
        table->timezone_offset_min = copy->timezone_offset_min;
        table->task_count = copy->task_count;
        table->next_task_id = copy->next_task_id;
        table->next_occurrence_id = copy->next_occurrence_id;
        memcpy(table->reserved, copy->reserved, sizeof(table->reserved));
        for (size_t i = 0; i < copy->task_count; ++i) {
            const gw_scheduler_persist_entry_v1_t *source = &copy->entries[i];
            gw_scheduler_persist_entry_t *target = &table->entries[i];
            target->id = source->id;
            target->revision = source->revision;
            target->enabled = source->enabled;
            target->type = source->type;
            target->duration_unit = source->duration_unit;
            target->daily_hour = source->daily_hour;
            target->daily_minute = source->daily_minute;
            target->action_type = source->action_type;
            target->action_len = source->action_len;
            target->reserved0 = source->reserved0;
            target->duration_value = source->duration_value;
            target->duration_deadline_utc_ms = source->duration_deadline_utc_ms;
            target->last_fired_local_day = source->last_fired_local_day;
            target->last_fired_local_minute = GW_SCHEDULER_LOCAL_MINUTE_NEVER;
            memcpy(target->name, source->name, sizeof(target->name));
            memcpy(target->action_data,
                   source->action_data,
                   sizeof(target->action_data));
        }
        free(copy);
        table->crc32 = persist_table_crc(table);
        out_len = sizeof(*table);
        migrated_v1 = true;
    }

    bool header_valid = out_len == sizeof(*table) &&
                        table->magic[0] == 'Y' && table->magic[1] == 'X' &&
                        table->magic[2] == 'T' && table->magic[3] == '1' &&
                        table->version == GW_SCHEDULER_TABLE_VERSION &&
                        table->table_size == sizeof(*table) &&
                        table->task_count <= GW_SCHEDULER_MAX_TASKS &&
                        timezone_offset_valid(table->timezone_offset_min) &&
                        table->crc32 == persist_table_crc(table);
    if (!header_valid) {
        free(table);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(s_tasks, 0, sizeof(s_tasks));
    memset(s_runtime, 0, sizeof(s_runtime));
    s_task_count = table->task_count;
    s_table_revision = table->table_revision;
    s_timezone_offset_min = table->timezone_offset_min;
    s_next_task_id = table->next_task_id == 0U ? 1U : table->next_task_id;
    s_next_occurrence_id = table->next_occurrence_id == 0U ? 1ULL :
                           table->next_occurrence_id;
    if (migrated_v1) {
        s_persistence_dirty = true;
        ESP_LOGI(TAG, "scheduler table migrated from V1");
    }
    uint32_t stored_runtime_epoch = 0U;
    memcpy(&stored_runtime_epoch,
           table->reserved,
           sizeof(stored_runtime_epoch));
    if (stored_runtime_epoch == 0U) {
        /* 兼容尚未写入 epoch 的旧版 V1 表；首次轮询会折叠为新格式。 */
        s_persistence_dirty = true;
    } else {
        s_runtime_epoch = stored_runtime_epoch;
    }
    for (size_t i = 0; i < s_task_count; ++i) {
        err = parse_persist_entry(&table->entries[i], &s_tasks[i], &s_runtime[i]);
        if (err != ESP_OK) {
            free(table);
            return err;
        }
        for (size_t previous = 0; previous < i; ++previous) {
            if (s_tasks[previous].id == s_tasks[i].id) {
                free(table);
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        if (s_tasks[i].enabled && s_tasks[i].type == GW_SCHEDULER_TASK_DAILY) {
            /*
             * daily 必须等本次启动第一次获得可信 UTC 后再决定是否跳过当天旧时刻。
             * 该标记故意不持久化，每次重启都重新执行一次断电边界判定。
             */
            s_runtime[i].daily_needs_reconcile = true;
        }

        bool runtime_applied = false;
        err = load_runtime_record_locked(i, &runtime_applied);
        if (err != ESP_OK) {
            free(table);
            return err;
        }
        if (runtime_applied) {
            ESP_LOGI(TAG,
                     "runtime progress restored: task=%lu type=%u",
                     (unsigned long)s_tasks[i].id,
                     (unsigned int)s_tasks[i].type);
        }
    }

    free(table);

    return ESP_OK;
}

static gw_scheduler_state_backup_t *make_state_backup_locked(void)
{
    gw_scheduler_state_backup_t *backup = malloc(sizeof(*backup));
    if (backup == NULL) {
        return NULL;
    }

    memcpy(backup->tasks, s_tasks, sizeof(s_tasks));
    memcpy(backup->runtime, s_runtime, sizeof(s_runtime));
    backup->task_count = s_task_count;
    backup->table_revision = s_table_revision;
    backup->next_task_id = s_next_task_id;
    backup->next_occurrence_id = s_next_occurrence_id;
    backup->runtime_epoch = s_runtime_epoch;
    backup->timezone_offset_min = s_timezone_offset_min;
    backup->persistence_dirty = s_persistence_dirty;
    return backup;
}

static void restore_state_locked(const gw_scheduler_state_backup_t *backup)
{
    memcpy(s_tasks, backup->tasks, sizeof(s_tasks));
    memcpy(s_runtime, backup->runtime, sizeof(s_runtime));
    s_task_count = backup->task_count;
    s_table_revision = backup->table_revision;
    s_next_task_id = backup->next_task_id;
    s_next_occurrence_id = backup->next_occurrence_id;
    s_runtime_epoch = backup->runtime_epoch;
    s_timezone_offset_min = backup->timezone_offset_min;
    s_persistence_dirty = backup->persistence_dirty;
}

static esp_err_t flush_dirty_locked(void)
{
    return s_persistence_dirty ? save_table_locked() : ESP_OK;
}

/** @brief 判断错误是否表示“定时表自身损坏”，此类错误可隔离为空表继续启动。 */
static bool schedule_table_corrupted(esp_err_t err)
{
    return err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC ||
           err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NVS_TYPE_MISMATCH;
}

/** @brief 丢弃加载过程中可能写入的半成品状态，恢复一个干净空表。 */
static void reset_empty_table_locked(void)
{
    memset(s_tasks, 0, sizeof(s_tasks));
    memset(s_runtime, 0, sizeof(s_runtime));
    s_task_count = 0U;
    s_table_revision = 0U;
    s_next_task_id = 1U;
    s_next_occurrence_id = 1ULL;
    s_runtime_epoch = make_runtime_epoch();
    s_timezone_offset_min = 0;
    s_persistence_dirty = false;
}

static void release_due_payload(void *payload)
{
    free(payload);
}

static esp_err_t post_due_event_locked(size_t index,
                                       uint64_t occurrence_id,
                                       uint64_t scheduled_utc_ms,
                                       const gw_time_point_t *now)
{
    gw_scheduler_due_payload_t *payload = calloc(1, sizeof(*payload));
    if (payload == NULL) {
        s_last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }

    payload->task_id = s_tasks[index].id;
    payload->revision = s_tasks[index].revision;
    payload->occurrence_id = occurrence_id;
    payload->scheduled_utc_ms = scheduled_utc_ms;
    payload->action_type = s_tasks[index].action_type;
    payload->action_len = s_tasks[index].action_len;
    memcpy(payload->action_data, s_tasks[index].action_data, payload->action_len);
    if (s_tasks[index].action_type == GW_SCHEDULER_ACTION_COMMAND) {
        memcpy(payload->command_name,
               s_tasks[index].command_name,
               sizeof(payload->command_name));
        payload->command_version = s_tasks[index].command_version;
        payload->command_args_len = s_tasks[index].command_args_len;
        memcpy(payload->command_args_json,
               s_tasks[index].command_args_json,
               s_tasks[index].command_args_len + 1U);
    }

    gw_event_t event = {
        .event_id = GW_EVT_SCHEDULE_DUE,
        .priority = GW_EVENT_PRIORITY_HIGH,
        .source = "scheduler",
        .target = NULL,
        .request_id = NULL,
        .method = "schedule.due",
        .uptime_ms = now->uptime_ms,
        .timestamp = now->utc_trusted ? now->utc_ms : 0U,
        .result = gw_result_ok(),
        .payload = payload,
        .payload_len = sizeof(*payload),
        .release_cb = release_due_payload,
    };

    esp_err_t err = gw_event_post(&event);
    if (err != ESP_OK) {
        free(payload);
        s_last_error = err;
        return err;
    }

    return ESP_OK;
}

/**
 * @brief 计算 daily 在“当前本地自然日”的理论执行时刻。
 *
 * 只接受可信 UTC，并显式检查有符号运算边界，避免异常时间值绕回后误触发。
 */
static bool daily_current_occurrence_locked(size_t index,
                                            const gw_time_point_t *now,
                                            int32_t *out_local_day,
                                            uint64_t *out_scheduled_utc_ms)
{
    if (!now->utc_trusted || now->utc_ms > INT64_MAX ||
        index >= s_task_count || !s_tasks[index].enabled ||
        s_tasks[index].type != GW_SCHEDULER_TASK_DAILY ||
        out_local_day == NULL || out_scheduled_utc_ms == NULL) {
        return false;
    }

    int64_t offset_ms = (int64_t)s_timezone_offset_min *
                        (int64_t)GW_SCHEDULER_MS_PER_MINUTE;
    int64_t utc_ms = (int64_t)now->utc_ms;
    if ((offset_ms > 0 && utc_ms > INT64_MAX - offset_ms) ||
        (offset_ms < 0 && utc_ms < INT64_MIN - offset_ms)) {
        return false;
    }
    int64_t local_ms = utc_ms + offset_ms;
    if (local_ms < 0) {
        return false;
    }

    int64_t local_day = local_ms / (int64_t)GW_SCHEDULER_MS_PER_DAY;
    if (local_day > INT32_MAX) {
        return false;
    }

    int64_t scheduled_local_ms = local_day * (int64_t)GW_SCHEDULER_MS_PER_DAY +
                                 (int64_t)s_tasks[index].trigger.daily.hour *
                                     (int64_t)GW_SCHEDULER_MS_PER_HOUR +
                                 (int64_t)s_tasks[index].trigger.daily.minute *
                                     (int64_t)GW_SCHEDULER_MS_PER_MINUTE;
    int64_t scheduled_utc_ms = scheduled_local_ms - offset_ms;
    if (scheduled_utc_ms < 0) {
        return false;
    }
    *out_local_day = (int32_t)local_day;
    *out_scheduled_utc_ms = (uint64_t)scheduled_utc_ms;
    return true;
}

/** @brief 判断当前 daily 时刻是否已到；同一本地日以及回拨到旧日期时均不会重复。 */
static bool daily_due_locked(size_t index,
                             const gw_time_point_t *now,
                             int32_t *out_local_day,
                             uint64_t *out_scheduled_utc_ms)
{
    if (index >= s_task_count || s_runtime[index].daily_needs_reconcile ||
        !daily_current_occurrence_locked(index,
                                         now,
                                         out_local_day,
                                         out_scheduled_utc_ms)) {
        return false;
    }

    if (s_runtime[index].last_fired_local_day >= *out_local_day) {
        return false;
    }
    return now->utc_ms >= *out_scheduled_utc_ms;
}

static bool cron_due_locked(size_t index,
                            const gw_time_point_t *now,
                            int64_t *out_local_minute,
                            uint64_t *out_scheduled_utc_ms)
{
    if (index >= s_task_count || now == NULL || !now->utc_trusted ||
        out_local_minute == NULL || out_scheduled_utc_ms == NULL ||
        !s_tasks[index].enabled ||
        s_tasks[index].type != GW_SCHEDULER_TASK_CRON ||
        !s_runtime[index].cron_schedule_valid ||
        s_runtime[index].cron_needs_reconcile) {
        return false;
    }

    int64_t local_minute = 0;
    bool matches = gw_cron_matches(&s_runtime[index].cron_schedule,
                                   now->utc_ms,
                                   s_timezone_offset_min,
                                   &local_minute);
    *out_local_minute = local_minute;
    *out_scheduled_utc_ms = (now->utc_ms / GW_SCHEDULER_MS_PER_MINUTE) *
                            GW_SCHEDULER_MS_PER_MINUTE;
    return matches && s_runtime[index].last_fired_local_minute < local_minute;
}

/**
 * @brief 在 RAM 中为到期事件预留唯一编号。
 *
 * 编号与领取后的执行进度随后由紧凑运行记录一次性落盘，避免“先保存编号、再保存
 * 进度”造成双倍写入。队列暂满时继续沿用同一编号，不会重复领取 occurrence。
 */
static esp_err_t reserve_occurrence_locked(size_t index, uint64_t *out_occurrence_id)
{
    if (index >= s_task_count || out_occurrence_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime[index].reserved_occurrence_id == 0U) {
        uint64_t occurrence_id = s_next_occurrence_id == 0U ? 1ULL :
                                 s_next_occurrence_id;
        s_runtime[index].reserved_occurrence_id = occurrence_id;
        s_next_occurrence_id = increment_u64_nonzero(occurrence_id);
    }

    *out_occurrence_id = s_runtime[index].reserved_occurrence_id;
    return ESP_OK;
}

/**
 * @brief 首次获得可信 UTC 时处理从 NVS 恢复的任务。
 *
 * duration 若已过截止点则直接停用；daily 若当天计划时刻已经过去则把当天标成
 * 已处理。两类情况都不会补投事件。未到期 duration 才会在本次启动建立 uptime
 * 截止点，从而把“断电期间越期”和“持续开机后正常到期”明确区分开。
 */
static esp_err_t reconcile_pending_tasks_locked(const gw_time_point_t *now)
{
    if (now == NULL || !now->utc_trusted) {
        return ESP_OK;
    }

    bool persist_changed = false;
    for (size_t i = 0; i < s_task_count; ++i) {
        if (!s_tasks[i].enabled) {
            s_runtime[i].daily_needs_reconcile = false;
            s_runtime[i].cron_needs_reconcile = false;
            continue;
        }

        if (task_uses_elapsed_interval(s_tasks[i].type) &&
            !s_runtime[i].duration_uptime_valid) {
            if (s_runtime[i].duration_deadline_utc_ms <= now->utc_ms) {
                if (s_tasks[i].type == GW_SCHEDULER_TASK_DURATION) {
                    ESP_LOGI(TAG,
                             "skip expired duration after restart: task=%lu deadline=%llu now=%llu",
                             (unsigned long)s_tasks[i].id,
                             (unsigned long long)s_runtime[i].duration_deadline_utc_ms,
                             (unsigned long long)now->utc_ms);
                    s_tasks[i].enabled = false;
                    s_runtime[i].duration_deadline_utc_ms = 0U;
                    s_runtime[i].duration_due_uptime_ms = 0U;
                    s_runtime[i].reserved_occurrence_id = 0U;
                } else {
                    /* 循环任务断电期间的次数只跳过，从本次可信启动重新计一周期。 */
                    ESP_LOGI(TAG,
                             "skip expired interval after restart: task=%lu deadline=%llu now=%llu",
                             (unsigned long)s_tasks[i].id,
                             (unsigned long long)s_runtime[i].duration_deadline_utc_ms,
                             (unsigned long long)now->utc_ms);
                    esp_err_t deadline_err = compute_duration_deadlines(
                        &s_tasks[i],
                        now,
                        &s_runtime[i].duration_deadline_utc_ms,
                        &s_runtime[i].duration_due_uptime_ms);
                    if (deadline_err != ESP_OK) {
                        s_last_error = deadline_err;
                        return deadline_err;
                    }
                    s_runtime[i].duration_uptime_valid = true;
                }
                persist_changed = true;
            } else {
                rebuild_duration_runtime_locked(i, now);
            }
            continue;
        }

        if (s_tasks[i].type == GW_SCHEDULER_TASK_DAILY &&
            s_runtime[i].daily_needs_reconcile) {
            int32_t local_day = GW_SCHEDULER_LOCAL_DAY_NEVER;
            uint64_t scheduled_utc_ms = 0U;
            if (!daily_current_occurrence_locked(i,
                                                  now,
                                                  &local_day,
                                                  &scheduled_utc_ms)) {
                s_last_error = ESP_ERR_INVALID_STATE;
                return ESP_ERR_INVALID_STATE;
            }

            if (now->utc_ms >= scheduled_utc_ms &&
                s_runtime[i].last_fired_local_day < local_day) {
                ESP_LOGI(TAG,
                         "skip elapsed daily occurrence after restart: task=%lu day=%ld",
                         (unsigned long)s_tasks[i].id,
                         (long)local_day);
                s_runtime[i].last_fired_local_day = local_day;
                persist_changed = true;
            }
            s_runtime[i].daily_needs_reconcile = false;
            continue;
        }

        if (s_tasks[i].type == GW_SCHEDULER_TASK_CRON &&
            s_runtime[i].cron_needs_reconcile) {
            int64_t local_minute = 0;
            (void)gw_cron_matches(&s_runtime[i].cron_schedule,
                                  now->utc_ms,
                                  s_timezone_offset_min,
                                  &local_minute);
            s_runtime[i].last_fired_local_minute = local_minute;
            s_runtime[i].cron_needs_reconcile = false;
            persist_changed = true;
        }
    }

    if (!persist_changed) {
        return ESP_OK;
    }

    s_persistence_dirty = true;
    return flush_dirty_locked();
}

esp_err_t gw_scheduler_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            s_last_error = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_time_change_lock == NULL) {
        s_time_change_lock = xSemaphoreCreateMutex();
        if (s_time_change_lock == NULL) {
            s_last_error = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (s_initialized) {
        scheduler_unlock();
        return ESP_OK;
    }

    memset(s_tasks, 0, sizeof(s_tasks));
    memset(s_runtime, 0, sizeof(s_runtime));
    s_task_count = 0;
    s_table_revision = 0;
    s_next_task_id = 1U;
    s_next_occurrence_id = 1ULL;
    s_runtime_epoch = make_runtime_epoch();
    s_timezone_offset_min = 0;
    s_persistence_dirty = false;
    s_time_change_paused = false;
    s_time_change_owner = NULL;

    gw_time_point_t now = gw_time_now();
    err = load_table_locked();
    if (schedule_table_corrupted(err)) {
        /*
         * 定时配置是可选业务数据，损坏时不能阻止网络、MQTT 和 BLE 启动。主备
         * 加密值均无法被现有读取路径恢复后，将其隔离并以空表继续运行。
         */
        esp_err_t corrupt_err = err;
        ESP_LOGE(TAG,
                 "schedule table corrupted; isolate and continue empty: %s",
                 esp_err_to_name(corrupt_err));
        esp_err_t erase_err = gw_storage_secure_erase_key(GW_NVS_NAMESPACE_SCHEDULE,
                                                           GW_PARAM_KEY_SCHEDULE_TABLE);
        reset_empty_table_locked();
        s_initialized = true;
        if (erase_err != ESP_OK) {
            /* poll 会重试把空表写回，NVS 短暂故障不能阻断其余业务模块启动。 */
            s_persistence_dirty = true;
            s_last_error = erase_err;
            ESP_LOGE(TAG,
                     "erase corrupted schedule table failed; retry empty table later: %s",
                     esp_err_to_name(erase_err));
        } else {
            s_last_error = corrupt_err;
        }
        scheduler_unlock();
        return ESP_OK;
    }
    if (err == ESP_OK) {
        s_initialized = true;
        esp_err_t reconcile_err = reconcile_pending_tasks_locked(&now);
        if (reconcile_err != ESP_OK) {
            /*
             * 内存中的“已越期跳过”状态已经生效，先保证不会补触发；NVS 临时失败
             * 由 poll 重试，不能因此让整个网关在 ESP_ERROR_CHECK 中反复重启。
             */
            s_last_error = reconcile_err;
            ESP_LOGE(TAG,
                     "reconcile restored schedules failed; retry later: %s",
                     esp_err_to_name(reconcile_err));
        } else {
            s_last_error = ESP_OK;
        }
        ESP_LOGI(TAG,
                 "scheduler ready: tasks=%u table_revision=%lu timezone_offset_min=%d",
                 (unsigned int)s_task_count,
                 (unsigned long)s_table_revision,
                 (int)s_timezone_offset_min);
    } else {
        s_last_error = err;
        ESP_LOGE(TAG, "load schedule table failed: %s", esp_err_to_name(err));
    }
    scheduler_unlock();
    return err;
}

esp_err_t gw_scheduler_poll(void)
{
    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_time_change_paused) {
        /* 授时事务尚未完成边界对账时，禁止观察到半更新的时间轴。 */
        scheduler_unlock();
        return ESP_OK;
    }

    err = flush_dirty_locked();
    if (err != ESP_OK) {
        scheduler_unlock();
        return err;
    }

    gw_time_point_t now = gw_time_now();
    if (!now.utc_trusted) {
        /* duration 也必须经过同一可信时间闸门，不能只依赖此前建立的 uptime。 */
        scheduler_unlock();
        return ESP_OK;
    }

    err = reconcile_pending_tasks_locked(&now);
    if (err != ESP_OK) {
        scheduler_unlock();
        return err;
    }

    for (size_t i = 0; i < s_task_count; ++i) {
        if (s_runtime[i].dispatch_pending) {
            /*
             * 进度写失败和队列暂满共用同一 RAM pending：前者先重试紧凑记录，
             * 后者只重试入队。两种情况都沿用原 occurrence_id，绝不重复领取。
             */
            if (!s_runtime[i].dispatch_progress_persisted) {
                err = save_runtime_record_locked(
                    i,
                    s_runtime[i].reserved_occurrence_id);
                if (err != ESP_OK) {
                    scheduler_unlock();
                    return err;
                }
                s_runtime[i].dispatch_progress_persisted = true;
            }
            err = post_due_event_locked(i,
                                        s_runtime[i].reserved_occurrence_id,
                                        s_runtime[i].pending_scheduled_utc_ms,
                                        &now);
            if (err != ESP_OK) {
                scheduler_unlock();
                return err;
            }
            s_runtime[i].dispatch_pending = false;
            s_runtime[i].dispatch_progress_persisted = false;
            s_runtime[i].reserved_occurrence_id = 0U;
            s_runtime[i].pending_scheduled_utc_ms = 0U;
            s_last_error = ESP_OK;
            continue;
        }
        if (!s_tasks[i].enabled) {
            continue;
        }

        bool due = false;
        int32_t local_day = GW_SCHEDULER_LOCAL_DAY_NEVER;
        int64_t local_minute = GW_SCHEDULER_LOCAL_MINUTE_NEVER;
        uint64_t scheduled_utc_ms = 0;
        if (task_uses_elapsed_interval(s_tasks[i].type)) {
            due = s_runtime[i].duration_uptime_valid &&
                  now.uptime_ms >= s_runtime[i].duration_due_uptime_ms;
            scheduled_utc_ms = s_runtime[i].duration_deadline_utc_ms;
        } else if (s_tasks[i].type == GW_SCHEDULER_TASK_DAILY) {
            due = daily_due_locked(i, &now, &local_day, &scheduled_utc_ms);
        } else if (s_tasks[i].type == GW_SCHEDULER_TASK_CRON) {
            due = cron_due_locked(i,
                                  &now,
                                  &local_minute,
                                  &scheduled_utc_ms);
        }
        if (!due) {
            continue;
        }

        uint64_t occurrence_id = 0U;
        err = reserve_occurrence_locked(i, &occurrence_id);
        if (err != ESP_OK) {
            scheduler_unlock();
            return err;
        }

        /* 先推进领取进度，再用单任务紧凑记录一次性保存编号和进度。 */
        s_runtime[i].dispatch_pending = true;
        s_runtime[i].dispatch_progress_persisted = false;
        s_runtime[i].pending_scheduled_utc_ms = scheduled_utc_ms;
        if (s_tasks[i].type == GW_SCHEDULER_TASK_DURATION) {
            /* 一次性任务领取后自动停用；配置 revision 保持不变。 */
            s_tasks[i].enabled = false;
            s_runtime[i].duration_deadline_utc_ms = 0;
            s_runtime[i].duration_due_uptime_ms = 0;
            s_runtime[i].duration_uptime_valid = false;
        } else if (s_tasks[i].type == GW_SCHEDULER_TASK_INTERVAL) {
            /* 循环任务先持久化下一周期，再投递本次动作，重启不会重复本次到期点。 */
            err = advance_interval_runtime_locked(i, &now);
            if (err != ESP_OK) {
                s_runtime[i].dispatch_pending = false;
                s_runtime[i].reserved_occurrence_id = 0U;
                s_runtime[i].pending_scheduled_utc_ms = 0U;
                scheduler_unlock();
                return err;
            }
        } else if (s_tasks[i].type == GW_SCHEDULER_TASK_DAILY) {
            s_runtime[i].last_fired_local_day = local_day;
        } else {
            s_runtime[i].last_fired_local_minute = local_minute;
        }

        err = save_runtime_record_locked(i, occurrence_id);
        if (err != ESP_OK) {
            /* 尚未投递动作；下次 poll 继续保存同一编号和领取进度。 */
            scheduler_unlock();
            return err;
        }
        s_runtime[i].dispatch_progress_persisted = true;

        err = post_due_event_locked(i, occurrence_id, scheduled_utc_ms, &now);
        if (err != ESP_OK) {
            /* 已落盘但队列暂满：保留 RAM 待投递状态，仅在本次开机继续重试。 */
            scheduler_unlock();
            return err;
        }
        s_runtime[i].dispatch_pending = false;
        s_runtime[i].dispatch_progress_persisted = false;
        s_runtime[i].reserved_occurrence_id = 0U;
        s_runtime[i].pending_scheduled_utc_ms = 0U;
        s_last_error = ESP_OK;
    }

    scheduler_unlock();
    return ESP_OK;
}

/** @brief 已持有调度锁时完成可信时间边界对账。 */
static esp_err_t reconcile_time_change_locked(const gw_time_point_t *now,
                                              bool reset_daily_boundary)
{
    if (now == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 权威校时可能把墙钟直接推过 daily 计划点。把它视为新的时间边界重新对账，
     * 已经建立的 duration 仍使用 uptime，避免校时跳变改变“经过了多久”的语义。
     */
    if (reset_daily_boundary) {
        for (size_t i = 0; i < s_task_count; ++i) {
            if (s_tasks[i].enabled && s_tasks[i].type == GW_SCHEDULER_TASK_DAILY) {
                s_runtime[i].daily_needs_reconcile = true;
            } else if (s_tasks[i].enabled &&
                       s_tasks[i].type == GW_SCHEDULER_TASK_CRON) {
                s_runtime[i].cron_needs_reconcile = true;
            }
        }
    }

    if (now->utc_trusted) {
        /*
         * RAM 的单调截止点绝不随 UTC 校时变化；这里只把“尚余多少单调时间”
         * 投影到新的 UTC 时间轴并保存。这样连续运行语义不变，而校时后若重启，
         * 也会从正确的持久截止点继续，不会提前、延后或误判为已过期。
         */
        for (size_t i = 0; i < s_task_count; ++i) {
            if (!s_tasks[i].enabled ||
                !task_uses_elapsed_interval(s_tasks[i].type) ||
                !s_runtime[i].duration_uptime_valid) {
                continue;
            }

            uint64_t rebased_deadline_utc_ms = 0U;
            if (s_runtime[i].duration_due_uptime_ms >= now->uptime_ms) {
                uint64_t remaining_ms =
                    s_runtime[i].duration_due_uptime_ms - now->uptime_ms;
                if (now->utc_ms > UINT64_MAX - remaining_ms) {
                    s_last_error = ESP_ERR_INVALID_SIZE;
                    return ESP_ERR_INVALID_SIZE;
                }
                rebased_deadline_utc_ms = now->utc_ms + remaining_ms;
            } else {
                /*
                 * interval 可能因 worker 短时阻塞而已经到期但尚未领取。保留其
                 * “逾期了多久”的相位，后续跳过积压周期时才能得到正确下一截止点。
                 */
                uint64_t overdue_ms =
                    now->uptime_ms - s_runtime[i].duration_due_uptime_ms;
                if (now->utc_ms < overdue_ms) {
                    s_last_error = ESP_ERR_INVALID_SIZE;
                    return ESP_ERR_INVALID_SIZE;
                }
                rebased_deadline_utc_ms = now->utc_ms - overdue_ms;
            }
            if (s_runtime[i].duration_deadline_utc_ms !=
                rebased_deadline_utc_ms) {
                s_runtime[i].duration_deadline_utc_ms =
                    rebased_deadline_utc_ms;
                s_persistence_dirty = true;
            }
        }
    }

    esp_err_t err = flush_dirty_locked();
    if (err == ESP_OK) {
        err = reconcile_pending_tasks_locked(now);
    }
    return err;
}

esp_err_t gw_scheduler_time_change_begin(void)
{
    if (s_time_change_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_time_change_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        (void)xSemaphoreGive(s_time_change_lock);
        return err;
    }
    if (!s_initialized || s_time_change_paused) {
        scheduler_unlock();
        (void)xSemaphoreGive(s_time_change_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 先在调度锁内关闭轮询，再释放调度锁执行 RTC/NVS 长操作。这样既没有
     * 半更新时间窗口，也不会长时间阻塞只读状态查询。
     */
    s_time_change_paused = true;
    s_time_change_owner = xTaskGetCurrentTaskHandle();
    scheduler_unlock();
    return ESP_OK;
}

esp_err_t gw_scheduler_time_change_finish(bool reset_daily_boundary)
{
    if (s_time_change_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized || !s_time_change_paused ||
        s_time_change_owner != xTaskGetCurrentTaskHandle()) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    gw_time_point_t now = gw_time_now();
    err = reconcile_time_change_locked(&now, reset_daily_boundary);

    /*
     * 即使 NVS 暂时失败也必须解除暂停。待对账标记仍保留在 RAM，下一次 poll
     * 会先补持久化/对账；在此之前 daily_due 会被标记拦住，不会误补触发。
     */
    s_time_change_paused = false;
    s_time_change_owner = NULL;
    scheduler_unlock();
    if (xSemaphoreGive(s_time_change_lock) != pdTRUE && err == ESP_OK) {
        err = ESP_FAIL;
    }
    return err;
}

bool gw_scheduler_time_change_owned_by_current_task(void)
{
    if (s_lock == NULL) {
        return false;
    }
    if (scheduler_lock() != ESP_OK) {
        return false;
    }
    bool owned = s_initialized && s_time_change_paused &&
                 s_time_change_owner == xTaskGetCurrentTaskHandle();
    scheduler_unlock();
    return owned;
}

esp_err_t gw_scheduler_notify_time_changed(bool reset_daily_boundary)
{
    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized || !scheduler_mutation_allowed_locked()) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    gw_time_point_t now = gw_time_now();
    err = reconcile_time_change_locked(&now, reset_daily_boundary);
    scheduler_unlock();
    return err;
}

esp_err_t gw_scheduler_get_status(gw_scheduler_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    gw_time_point_t now = gw_time_now();
    if (s_lock == NULL) {
        out->last_error = s_last_error;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    out->initialized = s_initialized;
    out->persistence_dirty = s_persistence_dirty;
    out->utc_trusted = now.utc_trusted;
    out->task_count = (uint16_t)s_task_count;
    out->timezone_offset_min = s_timezone_offset_min;
    out->table_revision = s_table_revision;
    out->next_occurrence_id = s_next_occurrence_id;
    out->last_error = s_last_error;
    for (size_t i = 0; i < s_task_count; ++i) {
        if (s_runtime[i].dispatch_pending &&
            !s_runtime[i].dispatch_progress_persisted) {
            out->persistence_dirty = true;
        }
        if (s_tasks[i].enabled) {
            ++out->enabled_count;
        }
        if (s_tasks[i].enabled && task_uses_elapsed_interval(s_tasks[i].type) &&
            !s_runtime[i].duration_uptime_valid) {
            ++out->duration_waiting_for_time;
        }
        if (s_tasks[i].enabled && s_tasks[i].type == GW_SCHEDULER_TASK_DAILY &&
            s_runtime[i].daily_needs_reconcile) {
            ++out->daily_waiting_for_time;
        }
        if (s_tasks[i].enabled && s_tasks[i].type == GW_SCHEDULER_TASK_CRON &&
            s_runtime[i].cron_needs_reconcile) {
            ++out->cron_waiting_for_time;
        }
    }
    bool initialized = s_initialized;
    scheduler_unlock();
    return initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t gw_scheduler_get_snapshot(gw_scheduler_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    out->table_revision = s_table_revision;
    out->timezone_offset_min = s_timezone_offset_min;
    out->task_count = (uint16_t)s_task_count;
    memcpy(out->tasks, s_tasks, s_task_count * sizeof(s_tasks[0]));
    scheduler_unlock();
    return ESP_OK;
}

esp_err_t gw_scheduler_get(uint32_t id, gw_scheduler_task_t *out)
{
    if (id == 0U || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    int index = find_task_index_locked(id);
    if (index < 0) {
        scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_tasks[index];
    scheduler_unlock();
    return ESP_OK;
}

esp_err_t gw_scheduler_upsert(const gw_scheduler_task_t *task, gw_scheduler_task_t *out_task)
{
    esp_err_t err = validate_task(task);
    if (err != ESP_OK) {
        return err;
    }

    err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!scheduler_mutation_allowed_locked()) {
        scheduler_unlock();
        return ESP_ERR_NOT_FINISHED;
    }
    err = flush_dirty_locked();
    if (err != ESP_OK) {
        scheduler_unlock();
        return err;
    }

    /* 倒计时和循环间隔的起点取实际持久化前的当前时刻。 */
    gw_time_point_t now = gw_time_now();
    if (task_uses_elapsed_interval(task->type) && task->enabled && !now.utc_trusted) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    bool creating = task->id == 0U;
    int index = creating ? (int)s_task_count : find_task_index_locked(task->id);
    if (creating && s_task_count >= GW_SCHEDULER_MAX_TASKS) {
        scheduler_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (!creating && index < 0) {
        scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (!creating && task->revision != 0U && task->revision != s_tasks[index].revision) {
        scheduler_unlock();
        return ESP_ERR_INVALID_VERSION;
    }
    if (!creating && s_runtime[index].dispatch_pending) {
        /* 已领取 occurrence 必须使用领取时的原任务内容，禁止被并发覆盖。 */
        scheduler_unlock();
        return ESP_ERR_NOT_FINISHED;
    }

    gw_scheduler_state_backup_t *backup = make_state_backup_locked();
    if (backup == NULL) {
        scheduler_unlock();
        return ESP_ERR_NO_MEM;
    }

    gw_scheduler_task_t updated;
    canonicalize_task(task, &updated);
    if (creating) {
        updated.id = allocate_task_id_locked();
        updated.revision = 1U;
        if (updated.id == 0U) {
            restore_state_locked(backup);
            free(backup);
            scheduler_unlock();
            return ESP_ERR_NO_MEM;
        }
        ++s_task_count;
    } else {
        updated.id = s_tasks[index].id;
        updated.revision = increment_u32_nonzero(s_tasks[index].revision);
    }

    gw_scheduler_runtime_t new_runtime = {
        .last_fired_local_day = GW_SCHEDULER_LOCAL_DAY_NEVER,
        .last_fired_local_minute = GW_SCHEDULER_LOCAL_MINUTE_NEVER,
    };
    if (task_uses_elapsed_interval(updated.type) && updated.enabled) {
        err = compute_duration_deadlines(&updated,
                                         &now,
                                         &new_runtime.duration_deadline_utc_ms,
                                         &new_runtime.duration_due_uptime_ms);
        if (err != ESP_OK) {
            restore_state_locked(backup);
            free(backup);
            scheduler_unlock();
            return err;
        }
        new_runtime.duration_uptime_valid = true;
    } else if (updated.type == GW_SCHEDULER_TASK_DAILY && !creating &&
               s_tasks[index].type == GW_SCHEDULER_TASK_DAILY &&
               s_tasks[index].trigger.daily.hour == updated.trigger.daily.hour &&
               s_tasks[index].trigger.daily.minute == updated.trigger.daily.minute) {
        /* 只改名称或动作时保留当日去重记录，避免保存后立即重复执行。 */
        new_runtime.last_fired_local_day = s_runtime[index].last_fired_local_day;
    }
    if (updated.type == GW_SCHEDULER_TASK_DAILY && updated.enabled) {
        /*
         * 新建、重新启用或修改 daily 后先经过一次当前时刻对账。若今天的计划
         * 时刻已经过去，本次只登记为跳过，不能把配置操作误当成到期沿。
         */
        new_runtime.daily_needs_reconcile = true;
    } else if (updated.type == GW_SCHEDULER_TASK_CRON) {
        err = gw_cron_parse(updated.trigger.cron.expression,
                            &new_runtime.cron_schedule);
        if (err != ESP_OK) {
            restore_state_locked(backup);
            free(backup);
            scheduler_unlock();
            return err;
        }
        new_runtime.cron_schedule_valid = true;
        new_runtime.cron_needs_reconcile = updated.enabled;
    }

    s_tasks[index] = updated;
    s_runtime[index] = new_runtime;
    err = save_table_locked();
    if (err != ESP_OK) {
        restore_state_locked(backup);
        s_last_error = err;
        free(backup);
        scheduler_unlock();
        return err;
    }

    if (out_task != NULL) {
        *out_task = s_tasks[index];
    }
    free(backup);
    scheduler_unlock();
    return ESP_OK;
}

esp_err_t gw_scheduler_delete_if_revision(uint32_t id, uint32_t expected_revision)
{
    if (id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!scheduler_mutation_allowed_locked()) {
        scheduler_unlock();
        return ESP_ERR_NOT_FINISHED;
    }
    err = flush_dirty_locked();
    if (err != ESP_OK) {
        scheduler_unlock();
        return err;
    }

    int index = find_task_index_locked(id);
    if (index < 0) {
        scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_revision != 0U &&
        expected_revision != s_tasks[index].revision) {
        scheduler_unlock();
        return ESP_ERR_INVALID_VERSION;
    }
    if (s_runtime[index].dispatch_pending) {
        scheduler_unlock();
        return ESP_ERR_NOT_FINISHED;
    }

    gw_scheduler_state_backup_t *backup = make_state_backup_locked();
    if (backup == NULL) {
        scheduler_unlock();
        return ESP_ERR_NO_MEM;
    }
    size_t trailing = s_task_count - (size_t)index - 1U;
    if (trailing > 0U) {
        memmove(&s_tasks[index], &s_tasks[index + 1], trailing * sizeof(s_tasks[0]));
        memmove(&s_runtime[index], &s_runtime[index + 1], trailing * sizeof(s_runtime[0]));
    }
    --s_task_count;
    memset(&s_tasks[s_task_count], 0, sizeof(s_tasks[0]));
    memset(&s_runtime[s_task_count], 0, sizeof(s_runtime[0]));

    err = save_table_locked();
    if (err != ESP_OK) {
        restore_state_locked(backup);
        s_last_error = err;
    } else {
        erase_runtime_record_best_effort(id);
    }
    free(backup);
    scheduler_unlock();
    return err;
}

esp_err_t gw_scheduler_delete(uint32_t id)
{
    return gw_scheduler_delete_if_revision(id, 0U);
}

esp_err_t gw_scheduler_set_enabled(uint32_t id,
                                   bool enabled,
                                   uint32_t expected_revision,
                                   gw_scheduler_task_t *out_task)
{
    if (id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!scheduler_mutation_allowed_locked()) {
        scheduler_unlock();
        return ESP_ERR_NOT_FINISHED;
    }
    err = flush_dirty_locked();
    if (err != ESP_OK) {
        scheduler_unlock();
        return err;
    }

    gw_time_point_t now = gw_time_now();

    int index = find_task_index_locked(id);
    if (index < 0) {
        scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_revision != 0U && expected_revision != s_tasks[index].revision) {
        scheduler_unlock();
        return ESP_ERR_INVALID_VERSION;
    }
    if (s_runtime[index].dispatch_pending) {
        scheduler_unlock();
        return ESP_ERR_NOT_FINISHED;
    }
    if (s_tasks[index].enabled == enabled) {
        if (out_task != NULL) {
            *out_task = s_tasks[index];
        }
        scheduler_unlock();
        return ESP_OK;
    }
    if (enabled && task_uses_elapsed_interval(s_tasks[index].type) && !now.utc_trusted) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    gw_scheduler_state_backup_t *backup = make_state_backup_locked();
    if (backup == NULL) {
        scheduler_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_tasks[index].enabled = enabled;
    s_tasks[index].revision = increment_u32_nonzero(s_tasks[index].revision);
    s_runtime[index].reserved_occurrence_id = 0U;
    if (task_uses_elapsed_interval(s_tasks[index].type)) {
        s_runtime[index].duration_deadline_utc_ms = 0;
        s_runtime[index].duration_due_uptime_ms = 0;
        s_runtime[index].duration_uptime_valid = false;
        if (enabled) {
            err = compute_duration_deadlines(&s_tasks[index],
                                             &now,
                                             &s_runtime[index].duration_deadline_utc_ms,
                                             &s_runtime[index].duration_due_uptime_ms);
            if (err != ESP_OK) {
                restore_state_locked(backup);
                free(backup);
                scheduler_unlock();
                return err;
            }
            s_runtime[index].duration_uptime_valid = true;
        }
    } else if (s_tasks[index].type == GW_SCHEDULER_TASK_DAILY) {
        s_runtime[index].daily_needs_reconcile = enabled;
    } else if (s_tasks[index].type == GW_SCHEDULER_TASK_CRON) {
        s_runtime[index].last_fired_local_minute = GW_SCHEDULER_LOCAL_MINUTE_NEVER;
        s_runtime[index].cron_needs_reconcile = enabled;
    }

    err = save_table_locked();
    if (err != ESP_OK) {
        restore_state_locked(backup);
        s_last_error = err;
    } else if (out_task != NULL) {
        *out_task = s_tasks[index];
    }
    free(backup);
    scheduler_unlock();
    return err;
}

esp_err_t gw_scheduler_set_timezone_offset(int16_t offset_min)
{
    if (!timezone_offset_valid(offset_min)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!scheduler_mutation_allowed_locked()) {
        scheduler_unlock();
        return ESP_ERR_NOT_FINISHED;
    }
    err = flush_dirty_locked();
    if (err != ESP_OK) {
        scheduler_unlock();
        return err;
    }
    if (s_timezone_offset_min == offset_min) {
        scheduler_unlock();
        return ESP_OK;
    }
    for (size_t i = 0; i < s_task_count; ++i) {
        if (s_runtime[i].dispatch_pending) {
            /* 避免已领取 daily 在变更时区时丢失 occurrence_id 或替换动作。 */
            scheduler_unlock();
            return ESP_ERR_NOT_FINISHED;
        }
    }

    gw_scheduler_state_backup_t *backup = make_state_backup_locked();
    if (backup == NULL) {
        scheduler_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_timezone_offset_min = offset_min;
    for (size_t i = 0; i < s_task_count; ++i) {
        if (s_tasks[i].type == GW_SCHEDULER_TASK_DAILY) {
            s_runtime[i].last_fired_local_day = GW_SCHEDULER_LOCAL_DAY_NEVER;
            s_runtime[i].reserved_occurrence_id = 0U;
            s_runtime[i].daily_needs_reconcile = s_tasks[i].enabled;
        } else if (s_tasks[i].type == GW_SCHEDULER_TASK_CRON) {
            s_runtime[i].last_fired_local_minute = GW_SCHEDULER_LOCAL_MINUTE_NEVER;
            s_runtime[i].reserved_occurrence_id = 0U;
            s_runtime[i].cron_needs_reconcile = s_tasks[i].enabled;
        }
    }

    err = save_table_locked();
    if (err != ESP_OK) {
        restore_state_locked(backup);
        s_last_error = err;
    }
    free(backup);
    scheduler_unlock();
    return err;
}

#else /* GW_SCHEDULER_SERVICE_ENABLED == 0 */

/*
 * 暂停构建只保留极小的协调状态：NVS 中的任务表不读取、不擦除，运行期不分配
 * s_tasks/s_runtime，也不提供定时任务 CRUD；这样暂停服务不会为未使用的业务表
 * 常驻片内 RAM。时间同步仍需要 begin/finish 的轻量事务，因此这些接口保留为
 * 无调度数据的空操作。
 */
esp_err_t gw_scheduler_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            s_last_error = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_time_change_lock == NULL) {
        s_time_change_lock = xSemaphoreCreateMutex();
        if (s_time_change_lock == NULL) {
            s_last_error = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        s_initialized = true;
        s_persistence_dirty = false;
        s_time_change_paused = false;
        s_time_change_owner = NULL;
        s_task_count = 0U;
        s_table_revision = 0U;
        s_next_task_id = 1U;
        s_next_occurrence_id = 1ULL;
        s_runtime_epoch = 0U;
        s_timezone_offset_min = 0;
        s_last_error = ESP_OK;
        ESP_LOGW(TAG,
                 "scheduler execution disabled; NVS task table retained and not loaded");
    }
    scheduler_unlock();
    return ESP_OK;
}

esp_err_t gw_scheduler_poll(void)
{
    return ESP_OK;
}

esp_err_t gw_scheduler_time_change_begin(void)
{
    if (s_time_change_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_time_change_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        (void)xSemaphoreGive(s_time_change_lock);
        return err;
    }
    if (!s_initialized || s_time_change_paused) {
        scheduler_unlock();
        (void)xSemaphoreGive(s_time_change_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_time_change_paused = true;
    s_time_change_owner = xTaskGetCurrentTaskHandle();
    scheduler_unlock();
    return ESP_OK;
}

esp_err_t gw_scheduler_time_change_finish(bool reset_daily_boundary)
{
    (void)reset_daily_boundary;
    if (s_time_change_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized || !s_time_change_paused ||
        s_time_change_owner != xTaskGetCurrentTaskHandle()) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_time_change_paused = false;
    s_time_change_owner = NULL;
    scheduler_unlock();
    if (xSemaphoreGive(s_time_change_lock) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool gw_scheduler_time_change_owned_by_current_task(void)
{
    if (s_lock == NULL || scheduler_lock() != ESP_OK) {
        return false;
    }
    bool owned = s_initialized && s_time_change_paused &&
                 s_time_change_owner == xTaskGetCurrentTaskHandle();
    scheduler_unlock();
    return owned;
}

esp_err_t gw_scheduler_notify_time_changed(bool reset_daily_boundary)
{
    (void)reset_daily_boundary;
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t gw_scheduler_get_status(gw_scheduler_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->last_error = s_last_error;
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    out->initialized = s_initialized;
    out->persistence_dirty = false;
    out->utc_trusted = gw_time_now().utc_trusted;
    out->task_count = 0U;
    out->enabled_count = 0U;
    out->timezone_offset_min = s_timezone_offset_min;
    out->table_revision = s_table_revision;
    out->next_occurrence_id = s_next_occurrence_id;
    out->last_error = s_last_error;
    scheduler_unlock();
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t gw_scheduler_get_snapshot(gw_scheduler_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    out->timezone_offset_min = s_timezone_offset_min;
    out->table_revision = s_table_revision;
    scheduler_unlock();
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t gw_scheduler_get(uint32_t id, gw_scheduler_task_t *out)
{
    (void)id;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_scheduler_upsert(const gw_scheduler_task_t *task,
                              gw_scheduler_task_t *out_task)
{
    (void)task;
    (void)out_task;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_scheduler_delete_if_revision(uint32_t id,
                                          uint32_t expected_revision)
{
    (void)id;
    (void)expected_revision;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_scheduler_delete(uint32_t id)
{
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_scheduler_set_enabled(uint32_t id,
                                   bool enabled,
                                   uint32_t expected_revision,
                                   gw_scheduler_task_t *out_task)
{
    (void)id;
    (void)enabled;
    (void)expected_revision;
    (void)out_task;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_scheduler_set_timezone_offset(int16_t offset_min)
{
    if (offset_min < GW_SCHEDULER_MIN_TIMEZONE_OFFSET_MIN ||
        offset_min > GW_SCHEDULER_MAX_TIMEZONE_OFFSET_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = scheduler_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* 暂停期间仅保留时间同步接口所需的瞬态时区值，不改写任务 NVS。 */
    s_timezone_offset_min = offset_min;
    scheduler_unlock();
    return ESP_OK;
}

#endif /* GW_SCHEDULER_SERVICE_ENABLED */
