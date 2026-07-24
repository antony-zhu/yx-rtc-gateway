#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 固件最多保存的定时任务数。 */
#define GW_SCHEDULER_MAX_TASKS               16U

/** @brief 任务名称允许的最大 UTF-8 字节数，不含结尾的 `\0`。 */
#define GW_SCHEDULER_NAME_MAX_BYTES          32U

/** @brief 定时动作内部二进制数据允许携带的最大字节数。 */
#define GW_SCHEDULER_ACTION_DATA_MAX_BYTES   64U

/** @brief 支持的定时任务类型。 */
typedef enum {
    GW_SCHEDULER_TASK_DURATION = 1, /*!< 从保存或重新启用时开始的一次性倒计时 */
    GW_SCHEDULER_TASK_DAILY = 2,    /*!< 按设备本地时间每天执行一次，重启不补旧时刻 */
    GW_SCHEDULER_TASK_INTERVAL = 3, /*!< 按若干分钟/小时循环，重启不补断电期间次数 */
} gw_scheduler_task_type_t;

/** @brief 时长定时的输入单位。 */
typedef enum {
    GW_SCHEDULER_DURATION_MINUTE = 1, /*!< 分钟 */
    GW_SCHEDULER_DURATION_HOUR = 2,   /*!< 小时 */
    GW_SCHEDULER_DURATION_DAY = 3,    /*!< 天 */
    GW_SCHEDULER_DURATION_WEEK = 4,   /*!< 周 */
} gw_scheduler_duration_unit_t;

/** @brief 定时到期后要执行的动作类型。 */
typedef enum {
    GW_SCHEDULER_ACTION_NOTIFY_ONLY = 1, /*!< 只产生到期事件，不携带 dongle 命令 */
    GW_SCHEDULER_ACTION_DONGLE_RAW = 2,  /*!< 向 dongle 写入原始二进制命令 */
    GW_SCHEDULER_ACTION_LIGHT_ON = 3,    /*!< 通过 GS200 将目标灯具设为常开 */
    GW_SCHEDULER_ACTION_LIGHT_OFF = 4,   /*!< 通过 GS200 将目标灯具设为常关 */
    GW_SCHEDULER_ACTION_SCENE_SWITCH = 5, /*!< 通过 GS200 载入目标灯具的指定情景 */
    GW_SCHEDULER_ACTION_BOARD_LED_ON = 6, /*!< 点亮板载定时任务灯 GPIO41 */
    GW_SCHEDULER_ACTION_BOARD_LED_OFF = 7, /*!< 熄灭板载定时任务灯 GPIO41 */
} gw_scheduler_action_type_t;

/** @brief 一次性时长定时参数。 */
typedef struct {
    uint32_t value;                     /*!< 数值，必须大于 0 */
    gw_scheduler_duration_unit_t unit;  /*!< 分、时、天或周 */
} gw_scheduler_duration_t;

/** @brief 每日定时参数，使用设备当前时区对应的本地时间。 */
typedef struct {
    uint8_t hour;   /*!< 小时，范围 0-23 */
    uint8_t minute; /*!< 分钟，范围 0-59 */
} gw_scheduler_daily_t;

/**
 * @brief 可由蓝牙等配置入口读写的定时任务。
 *
 * 创建任务时 id 和 revision 均填写 0；更新任务时填写现有 id，revision
 * 可填写读取到的版本以进行乐观并发校验，也可填写 0 表示不校验。任务名称
 * 按 UTF-8 字节保存，必须以 `\0` 结束。BLE APP 只允许创建 light_on、
 * light_off 和 scene_switch；地址、网络号、控制字、CRC 和完整协议帧均由固件
 * 组装。调度器内部仍保留旧动作枚举，以兼容既有 NVS 数据和嵌入式自测，
 * 这些旧动作不再作为 APP 配置接口开放。
 *
 * 三类 GS200 动作的 action_data 保存由 gw_gs200_protocol 生成的完整帧，
 * 调度器会同时校验帧内容与 action_type，防止持久化数据被错误解释。
 */
typedef struct {
    uint32_t id;                                      /*!< 稳定任务 ID，0 仅用于创建 */
    uint32_t revision;                                /*!< 配置版本，成功修改后自动递增 */
    bool enabled;                                     /*!< 是否启用 */
    char name[GW_SCHEDULER_NAME_MAX_BYTES + 1U];      /*!< UTF-8 任务名称，可为空 */
    gw_scheduler_task_type_t type;                    /*!< 任务类型 */
    union {
        gw_scheduler_duration_t duration;             /*!< type=duration/interval 时有效 */
        gw_scheduler_daily_t daily;                   /*!< type=daily 时有效 */
    } trigger;
    gw_scheduler_action_type_t action_type;           /*!< 到期动作类型 */
    uint8_t action_len;                               /*!< action_data 的有效长度 */
    uint8_t action_data[GW_SCHEDULER_ACTION_DATA_MAX_BYTES]; /*!< 动作二进制数据 */
} gw_scheduler_task_t;

/**
 * @brief `GW_EVT_SCHEDULE_DUE` 的堆负载。
 *
 * 事件投递成功后负载所有权归事件接收方，接收方应按 gw_event_t.release_cb
 * 约定释放。occurrence_id 与领取进度在投递前由单任务紧凑记录一次性持久化，
 * 后续事件不会复用，可用于动作执行去重；队列暂满时沿用同一编号重试。
 */
typedef struct {
    uint32_t task_id;                                 /*!< 到期任务 ID */
    uint32_t revision;                                /*!< 产生本次动作时的任务配置版本 */
    uint64_t occurrence_id;                           /*!< 本次发生记录的唯一递增 ID */
    uint64_t scheduled_utc_ms;                        /*!< 理论到期 UTC 毫秒 */
    gw_scheduler_action_type_t action_type;           /*!< 到期动作类型 */
    uint8_t action_len;                               /*!< action_data 的有效长度 */
    uint8_t action_data[GW_SCHEDULER_ACTION_DATA_MAX_BYTES]; /*!< 动作二进制数据 */
} gw_scheduler_due_payload_t;

/** @brief 定时任务模块的原子配置快照。 */
typedef struct {
    uint32_t table_revision;                          /*!< 整张配置表的持久化版本 */
    int16_t timezone_offset_min;                      /*!< 本地时间相对 UTC 的分钟偏移 */
    uint16_t task_count;                              /*!< tasks 中的有效任务数 */
    gw_scheduler_task_t tasks[GW_SCHEDULER_MAX_TASKS]; /*!< 按保存顺序排列的任务 */
} gw_scheduler_snapshot_t;

/** @brief 定时任务模块运行状态。 */
typedef struct {
    bool initialized;                    /*!< 模块是否完成初始化 */
    bool persistence_dirty;              /*!< 发生编号或执行进度是否等待重试保存 */
    bool utc_trusted;                    /*!< 当前 UTC 是否可信 */
    uint16_t task_count;                 /*!< 已保存任务数量 */
    uint16_t enabled_count;              /*!< 已启用任务数量 */
    uint16_t duration_waiting_for_time;  /*!< 重启后等待可信 UTC 恢复的时长任务数 */
    uint16_t daily_waiting_for_time;     /*!< 等待可信 UTC 完成启动对账的每日任务数 */
    int16_t timezone_offset_min;          /*!< 本地时间相对 UTC 的分钟偏移 */
    uint32_t table_revision;             /*!< 配置表持久化版本 */
    uint64_t next_occurrence_id;          /*!< 下一次成功投递将使用的发生 ID */
    esp_err_t last_error;                /*!< 最近一次调度或持久化错误 */
} gw_scheduler_status_t;

/**
 * @brief 初始化定时任务模块并从加密 NVS 恢复任务表。
 *
 * 调用前必须完成 gw_storage_init() 和 gw_time_init()。重复调用安全。
 * 已启用的 duration 任务在同次启动内使用 uptime 判断到期；重启恢复时只为
 * 尚未到期的任务重建 uptime 截止点，已经越期的任务直接停用且不补触发。
 * daily 若启动时当天计划时刻已经过去，会跳过当天并等待下一天。
 *
 * @return ESP_OK 表示成功；持久化数据损坏或资源不足时返回对应错误。
 */
esp_err_t gw_scheduler_init(void);

/**
 * @brief 轮询并投递已到期任务。
 *
 * 建议由单一系统任务每秒调用一次。UTC 不可信时所有任务均被硬性禁止触发。
 * 持续开机期间，duration/interval 使用单调 uptime、daily 使用“当天是否已执行”标记，
 * 即使轮询短暂延迟也会在恢复后触发一次；重启后则先跳过已经越期的发生记录，
 * 不做历史补触发。发生编号和领取进度先持久化，再投递事件；掉电后不重放。
 *
 * @return ESP_OK 表示轮询完成；事件队列满或保存失败时返回对应错误。
 */
esp_err_t gw_scheduler_poll(void);

/**
 * @brief 开始一次与调度器原子协调的授时事务。
 *
 * 函数先串行化所有授时来源，再暂停到期轮询；随后调用方可更新时区、系统 UTC、
 * 外部 RTC 和 NVS。长耗时授时阶段不会一直占用调度表互斥锁。必须由同一任务与
 * gw_scheduler_time_change_finish() 成对调用；begin 失败时不得继续发布新 UTC。
 *
 * @return ESP_OK 表示调度已暂停；未初始化或资源异常时返回对应错误。
 */
esp_err_t gw_scheduler_time_change_begin(void);

/**
 * @brief 完成授时后的边界对账并恢复调度轮询。
 *
 * 无论候选时间是否被接受，成功 begin 的调用方都必须调用本函数。首次恢复可信
 * 时间或大幅跳时传 true，使 daily 把跨过的计划点记为 missed；普通微调传 false。
 * 已运行的 duration/interval 保持单调截止点不变，并把该截止点映射到新的持久 UTC
 * 时间轴，保证校时后重启仍延续同一剩余时长和 interval 相位。对账/NVS 失败时，
 * 函数仍解除暂停，
 * RAM 中的待对账标记会让后续 poll 失败关闭并重试。
 *
 * @param reset_daily_boundary 是否重建全部 daily 的本次时间边界。
 * @return ESP_OK 表示完成；持久化或对账失败时返回对应错误。
 */
esp_err_t gw_scheduler_time_change_finish(bool reset_daily_boundary);

/**
 * @brief 判断当前任务是否持有授时事务。
 *
 * 供统一时间模块验证低层候选时间入口，避免未来新增来源绕过暂停与边界对账。
 *
 * @return true 表示当前任务已成功 begin 且尚未 finish。
 */
bool gw_scheduler_time_change_owned_by_current_task(void);

/**
 * @brief 通知调度器可信时间状态发生变化。
 *
 * 该接口为重启后尚未获得可信 UTC 的任务完成首次对账：越期 duration 停用、
 * 越期 interval 从当前时刻开始下一周期、已过时的当日 daily 跳过，并为未来
 * duration/interval 重建 uptime 截止点。调用方确认发生
 * 大幅时间跳变时，可要求全部 daily 重新建立边界；普通小幅校准不会吞掉已经到期、
 * 正在等待事件队列的任务。同次启动 duration 始终使用单调 uptime。
 *
 * @param reset_daily_boundary true 表示大幅跳时或首次恢复可信时间，需要把全部
 *        daily 作为新时间边界重新对账；false 只处理已有待对账任务。
 * @return ESP_OK 表示处理完成；模块未初始化时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t gw_scheduler_notify_time_changed(bool reset_daily_boundary);

/**
 * @brief 获取模块运行状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功；模块未初始化时仍填写可用状态并返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t gw_scheduler_get_status(gw_scheduler_status_t *out);

/**
 * @brief 获取包含全部任务的原子快照。
 *
 * @param out 输出快照，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_scheduler_get_snapshot(gw_scheduler_snapshot_t *out);

/**
 * @brief 按 ID 读取单个任务。
 *
 * @param id 任务 ID，必须非 0。
 * @param out 输出任务，不能为空。
 * @return ESP_OK 表示成功；不存在时返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t gw_scheduler_get(uint32_t id, gw_scheduler_task_t *out);

/**
 * @brief 创建或更新任务并立即保存到加密 NVS。
 *
 * task.id=0 时创建并自动分配 ID；task.id 非 0 时只更新已存在任务。
 * 保存或更新一个已启用 duration 任务会从当前时刻重新开始倒计时，因此此时
 * 必须已有可信 UTC。成功后通过 out_task 返回设备生成的 id 和 revision。
 *
 * @param task 输入任务。
 * @param out_task 成功后的完整任务，可为空。
 * @return ESP_OK 表示保存成功；任务表已满、字段非法、版本冲突或时间不可信时返回错误。
 */
esp_err_t gw_scheduler_upsert(const gw_scheduler_task_t *task, gw_scheduler_task_t *out_task);

/**
 * @brief 删除一个任务并立即保存。
 *
 * @param id 任务 ID。
 * @return ESP_OK 表示删除成功；不存在时返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t gw_scheduler_delete(uint32_t id);

/**
 * @brief 启用或停用任务。
 *
 * duration 从 false 改为 true 时从当前时刻重新倒计时，要求 UTC 可信；停用会
 * 清除其未到期截止点。重复设置为当前值不会改变 revision。
 *
 * @param id 任务 ID。
 * @param enabled 目标启用状态。
 * @param expected_revision 期望任务版本，0 表示不校验。
 * @param out_task 成功后的任务，可为空。
 * @return ESP_OK 表示成功；版本冲突返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t gw_scheduler_set_enabled(uint32_t id,
                                   bool enabled,
                                   uint32_t expected_revision,
                                   gw_scheduler_task_t *out_task);

/**
 * @brief 设置设备本地时区相对 UTC 的分钟偏移。
 *
 * 合法范围为 UTC-12:00 到 UTC+14:00（-720 至 +840）。时区变化时会重置
 * daily 的“当日已执行”标记，使新时区下的自然日判断立即生效。
 *
 * @param offset_min 相对 UTC 的分钟偏移。
 * @return ESP_OK 表示保存成功。
 */
esp_err_t gw_scheduler_set_timezone_offset(int16_t offset_min);

#ifdef __cplusplus
}
#endif
