#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UTC 可信来源。
 */
typedef enum {
    GW_TIME_SOURCE_NONE = 0,
    GW_TIME_SOURCE_RTC,
    GW_TIME_SOURCE_4G,
    GW_TIME_SOURCE_MQTT,
    GW_TIME_SOURCE_HTTPS,
    GW_TIME_SOURCE_SNTP,
    GW_TIME_SOURCE_BLE_PHONE,
} gw_time_source_t;

/**
 * @brief 启动时间进入“必须网络恢复”状态的原因。
 *
 * 恢复锁会写入加密 NVS 并跨重启保持，用于诊断 RTC 电池脱落、停钟、日历
 * 损坏等问题。网络授时成功且持久化清锁成功后 `recovery_required` 才会清除，
 * 但这里仍保留最初故障原因。
 */
typedef enum {
    GW_TIME_RECOVERY_NONE = 0,              /*!< RTC 启动校验通过，无需恢复 */
    GW_TIME_RECOVERY_RTC_MISSING,           /*!< RTC 不存在或 I2C 无响应 */
    GW_TIME_RECOVERY_RTC_VOLTAGE_LOW,       /*!< RTC VLF 置位，曾低于保持电压 */
    GW_TIME_RECOVERY_RTC_STOPPED,           /*!< RTC STOP 置位，振荡/计时已停止 */
    GW_TIME_RECOVERY_RTC_INVALID_CALENDAR,  /*!< RTC BCD、日期或星期字段非法 */
    GW_TIME_RECOVERY_RTC_READ_FAILED,       /*!< RTC 读取出现其他硬件错误 */
    GW_TIME_RECOVERY_RTC_NOT_ADVANCING,     /*!< 标志位正常但 RTC 读秒未向前走 */
    GW_TIME_RECOVERY_RTC_OUT_OF_RANGE,      /*!< RTC 时间超出产品可信年份范围 */
    GW_TIME_RECOVERY_RTC_ROLLBACK,          /*!< RTC 早于 NVS 防回退检查点 */
    GW_TIME_RECOVERY_SYSTEM_CLOCK_FAILED,   /*!< RTC 正常但写系统时钟失败 */
    GW_TIME_RECOVERY_MANUAL_INVALIDATION,   /*!< 运行中被显式标记为不可信 */
    GW_TIME_RECOVERY_STATE_INVALID,         /*!< 持久化恢复锁损坏，按不可信处理 */
    GW_TIME_RECOVERY_REASON_COUNT,          /*!< 枚举数量，不作为实际故障原因 */
} gw_time_recovery_reason_t;

/**
 * @brief 跨模块时间点。
 *
 * 超时和重试必须使用 uptime_ms；协议时间戳只有在 utc_trusted 为 true
 * 时才使用 utc_ms，否则协议 timestamp 填 0。
 */
typedef struct {
    uint64_t utc_ms;              /*!< UTC 毫秒时间戳；不可信时为 0 */
    uint64_t uptime_ms;           /*!< 单调运行时间，单位毫秒 */
    bool utc_trusted;             /*!< UTC 是否可信 */
    gw_time_source_t source;      /*!< 当前可信 UTC 来源 */
} gw_time_point_t;

/**
 * @brief 时间模块状态。
 */
typedef struct {
    gw_time_point_t now;              /*!< 当前时间点 */
    uint64_t last_trusted_utc_ms;     /*!< NVS 中最近可信 UTC；无记录时为 0 */
    uint64_t last_sync_utc_ms;        /*!< 本次启动最近一次接受的可信 UTC；无记录时为 0 */
    uint64_t rtc_utc_ms;              /*!< 最近一次有效外部 RTC UTC；无有效值时为 0 */
    gw_time_source_t last_sync_source;/*!< 本次启动最近一次授时来源 */
    gw_time_recovery_reason_t recovery_reason; /*!< 本次启动首次时间故障原因 */
    bool last_trusted_valid;          /*!< last_trusted_utc_ms 是否有效 */
    bool rollback_detected;           /*!< 本次启动是否检测到候选时间回退 */
    bool recovery_required;           /*!< 是否锁定为必须由网络权威时间恢复 */
    bool scheduler_time_trusted;      /*!< 调度器当前是否允许按 UTC 触发任务 */
    bool rtc_present;                 /*!< 外部 RTC 是否能通过 I2C 通信 */
    bool rtc_valid;                   /*!< 外部 RTC 日期和状态是否通过可信校验 */
    bool rtc_vlf_latched;             /*!< RTC 的 VLF 掉压/数据无效标志是否置位 */
    bool rtc_stopped;                 /*!< RTC 的 STOP 停钟标志是否置位 */
    bool rtc_write_attempted;         /*!< 本次启动是否尝试过回写外部 RTC */
    bool rtc_last_write_performed;    /*!< 最近一次同步是否实际写了 RTC 寄存器 */
    bool rtc_last_write_succeeded;    /*!< 最近一次 RTC 回写及读回校验是否成功 */
    esp_err_t rtc_last_read_error;    /*!< 最近一次 RTC 读取错误 */
    esp_err_t rtc_last_write_error;   /*!< 最近一次 RTC 写入错误 */
    esp_err_t last_error;             /*!< 最近一次时间校验或 NVS 操作错误 */
} gw_time_status_t;

/**
 * @brief 初始化时间模块。
 *
 * 该接口应在 NVS 初始化后调用，用于读取加密 NVS 中的 last_trusted_utc，并
 * 初始化 GPIO9/GPIO10 上的 YSN8010 外部电池 RTC。如果 RTC 的 VLF/STOP、
 * BCD 日历、连续读秒、可信年份和回退校验全部通过，则在调度器启动前恢复系统 UTC。
 * RTC 不存在或内容无效时会降级为 UTC 不可信并把 recovery_required 锁存到
 * 加密 NVS，网关仍会继续启动，但调度器不得触发任务。后续重启即使 RTC 恢复
 * 正常也不会自行清锁；该锁只能由 4G/MQTT/HTTPS/SNTP 网络授时解除，BLE
 * 手机时间不能解除。
 *
 * @return ESP_OK 表示初始化完成；读取不到 last_trusted_utc 也返回 ESP_OK。
 */
esp_err_t gw_time_init(void);

/**
 * @brief 获取当前时间点。
 *
 * @return 当前 uptime 和 UTC 状态；UTC 不可信时 utc_ms 为 0。
 */
gw_time_point_t gw_time_now(void);

/**
 * @brief 将当前 UTC 状态标记为不可信，并要求重新进行网络授时。
 *
 * 调用后 BLE 手机时间不能恢复调度资格；必须接受 4G/MQTT/HTTPS/SNTP
 * 任一网络权威来源的时间。接口会同步写入加密 NVS 恢复锁，可能阻塞，必须在
 * 任务上下文调用，不得在中断或持有时间模块内部锁时调用。
 */
void gw_time_set_untrusted(void);

/**
 * @brief 设置可信 UTC。
 *
 * @param utc_ms 可信 UTC 毫秒时间戳。
 * @param source 可信来源。
 */
void gw_time_set_trusted(uint64_t utc_ms, gw_time_source_t source);

/**
 * @brief 校验并接受候选 UTC。
 *
 * 候选时间必须位于 2026-2046 年范围内。RTC 或 BLE 候选不得早于
 * last_trusted_utc - GW_TIME_ROLLBACK_TOLERANCE_MS；网络权威来源允许纠正错误的
 * 旧 NVS 检查点和当前系统时间，并在必要时重建 NVS 检查点。
 *
 * 启动检测到 RTC 缺失、VLF、STOP、日历非法、超范围或回退后，接口只接受
 * 4G/MQTT/HTTPS/SNTP 来源来解除 recovery_required；BLE 手机时间会返回
 * ESP_ERR_INVALID_STATE，且不会让调度器获得可信时间。
 * 接受后会更新系统 UTC，并最佳努力回写外部 RTC 和 NVS。普通校时的 RTC/NVS
 * 检查点写失败不会撤销已经生效的系统时间；但解除 recovery_required 时，只有
 * 恢复锁成功写回 NVS 后才允许调度，清锁写失败会返回错误并保持 UTC 不可信。
 * 这是运行期低层入口：调用方必须先成功调用 gw_scheduler_time_change_begin()，
 * 并在所有成功/失败路径调用 gw_scheduler_time_change_finish()；未持有事务时拒绝。
 * 调用方可通过 gw_time_get_status() 查询持久化状态。
 *
 * @param utc_ms 候选 UTC 毫秒时间戳。
 * @param source 候选来源。
 * @return ESP_OK 表示已接受并更新当前可信 UTC。
 */
esp_err_t gw_time_accept_candidate(uint64_t utc_ms, gw_time_source_t source);

/**
 * @brief 按策略保存当前可信 UTC 到 NVS。
 *
 * 首次可信 UTC 会立即保存；当前可信 UTC 比 NVS 值推进超过 24 小时也会保存；
 * force=true 时只要当前 UTC 可信且大于 NVS 值就保存。
 *
 * @param force 是否强制按关键事件保存。
 * @return ESP_OK 表示无需保存或保存成功。
 */
esp_err_t gw_time_maybe_save_last_trusted(bool force);

/**
 * @brief 获取时间模块状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_time_get_status(gw_time_status_t *out);

/**
 * @brief 获取时间来源字符串。
 *
 * @param source 时间来源枚举。
 * @return 稳定英文短字符串。
 */
const char *gw_time_source_to_string(gw_time_source_t source);

/**
 * @brief 获取时间恢复原因字符串。
 *
 * @param reason 时间恢复原因枚举。
 * @return 稳定英文短字符串，可用于日志和协议状态字段。
 */
const char *gw_time_recovery_reason_to_string(gw_time_recovery_reason_t reason);

#ifdef __cplusplus
}
#endif
