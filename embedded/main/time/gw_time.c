#include "gw_time.h"

#include <inttypes.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "gw_defaults.h"
#include "gw_param.h"
#include "gw_rtc_ysn8010.h"
#include "gw_scheduler.h"
#include "gw_storage.h"

#define GW_TIME_MIN_TRUSTED_UTC_MS     1767225600000ULL
#define GW_TIME_MAX_TRUSTED_UTC_MS     2429913599999ULL
#define GW_TIME_RTC_WRITE_SKIP_MS      2000ULL
#define GW_TIME_RTC_WRITE_RETRY_MS     60000ULL
#define GW_TIME_RTC_PROGRESS_CHECK_MS  1100ULL
#define GW_TIME_RTC_PROGRESS_SLACK_MS  2000ULL
#define GW_TIME_RECOVERY_RECORD_VERSION 1U

/** @brief 跨重启保存的时间恢复锁；外层安全存储已经提供 CRC 和主备副本。 */
typedef struct {
    uint8_t version;
    uint8_t required;
    uint8_t reason;
    uint8_t reserved;
} gw_time_recovery_record_t;

_Static_assert(sizeof(gw_time_recovery_record_t) == 4U,
               "time recovery record layout changed");

static const char *TAG = "gw_time";
static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_time_update_mutex_init_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_time_update_mutex;
static StaticSemaphore_t s_time_update_mutex_buffer;
static bool s_utc_trusted;
static uint64_t s_utc_base_ms;
static uint64_t s_uptime_base_ms;
static gw_time_source_t s_time_source = GW_TIME_SOURCE_NONE;
static uint64_t s_last_trusted_utc_ms;
static uint64_t s_last_sync_utc_ms;
static gw_time_source_t s_last_sync_source = GW_TIME_SOURCE_NONE;
static bool s_last_trusted_valid;
static bool s_rollback_detected;
static bool s_recovery_required;
static gw_time_recovery_reason_t s_recovery_reason = GW_TIME_RECOVERY_NONE;
static uint64_t s_rtc_utc_ms;
static bool s_rtc_present;
static bool s_rtc_valid;
static bool s_rtc_vlf_latched;
static bool s_rtc_stopped;
static bool s_rtc_write_attempted;
static bool s_rtc_last_write_performed;
static bool s_rtc_last_write_succeeded;
static uint64_t s_rtc_last_write_attempt_uptime_ms;
static esp_err_t s_rtc_last_read_error = ESP_ERR_INVALID_STATE;
static esp_err_t s_rtc_last_write_error = ESP_OK;
static esp_err_t s_last_error = ESP_OK;

static uint64_t uptime_ms_now(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static bool source_valid(gw_time_source_t source)
{
    return source == GW_TIME_SOURCE_RTC ||
           source == GW_TIME_SOURCE_4G ||
           source == GW_TIME_SOURCE_MQTT ||
           source == GW_TIME_SOURCE_HTTPS ||
           source == GW_TIME_SOURCE_SNTP ||
           source == GW_TIME_SOURCE_BLE_PHONE;
}

/** @brief 判断来源是否为可解除 RTC 故障锁定的网络权威时间。 */
static bool source_is_network_authoritative(gw_time_source_t source)
{
    return source == GW_TIME_SOURCE_4G ||
           source == GW_TIME_SOURCE_MQTT ||
           source == GW_TIME_SOURCE_HTTPS ||
           source == GW_TIME_SOURCE_SNTP;
}

static bool utc_in_trusted_range(uint64_t utc_ms)
{
    return utc_ms >= GW_TIME_MIN_TRUSTED_UTC_MS && utc_ms <= GW_TIME_MAX_TRUSTED_UTC_MS;
}

/** @brief 判断安全时间记录是否属于内容/类型损坏，可锁定后等待网络自愈。 */
static bool persisted_time_value_corrupted(esp_err_t err)
{
    return err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC ||
           err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NVS_TYPE_MISMATCH ||
           err == ESP_ERR_NVS_INVALID_LENGTH;
}

/** @brief 把外部 RTC 的 UTC 日历转换成 Unix 毫秒时间戳。 */
static bool rtc_datetime_to_utc_ms(const gw_rtc_ysn8010_datetime_t *datetime,
                                   uint64_t *out_utc_ms)
{
    if (datetime == NULL || out_utc_ms == NULL) {
        return false;
    }

    /* 直接按公历换算，避免受进程 TZ 设置影响，也不依赖非标准 timegm()。 */
    int32_t year = (int32_t)datetime->year;
    uint32_t month = datetime->month;
    year -= month <= 2U ? 1 : 0;
    int32_t era = (year >= 0 ? year : year - 399) / 400;
    uint32_t year_of_era = (uint32_t)(year - era * 400);
    uint32_t shifted_month = month > 2U ? month - 3U : month + 9U;
    uint32_t day_of_year = (153U * shifted_month + 2U) / 5U +
                           datetime->day - 1U;
    uint32_t day_of_era = year_of_era * 365U + year_of_era / 4U -
                          year_of_era / 100U + day_of_year;
    int64_t days_since_epoch = (int64_t)era * 146097LL + day_of_era - 719468LL;
    if (days_since_epoch < 0) {
        return false;
    }

    uint64_t seconds = (uint64_t)days_since_epoch * 86400ULL +
                       (uint64_t)datetime->hour * 3600ULL +
                       (uint64_t)datetime->minute * 60ULL + datetime->second;
    *out_utc_ms = seconds * 1000ULL;
    return true;
}

/** @brief 把 Unix 毫秒时间戳转换为外部 RTC 使用的 UTC 日历。 */
static bool utc_ms_to_rtc_datetime(uint64_t utc_ms,
                                   gw_rtc_ysn8010_datetime_t *out_datetime)
{
    if (out_datetime == NULL) {
        return false;
    }

    uint64_t total_seconds = utc_ms / 1000ULL;
    int64_t days_since_epoch = (int64_t)(total_seconds / 86400ULL);
    uint32_t seconds_of_day = (uint32_t)(total_seconds % 86400ULL);

    /* civil_from_days：把 1970-01-01 起的天数反解成公历日期。 */
    int64_t shifted_days = days_since_epoch + 719468LL;
    int64_t era = (shifted_days >= 0 ? shifted_days : shifted_days - 146096LL) /
                  146097LL;
    uint32_t day_of_era = (uint32_t)(shifted_days - era * 146097LL);
    uint32_t year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
         day_of_era / 146096U) /
        365U;
    int32_t year = (int32_t)year_of_era + (int32_t)era * 400;
    uint32_t day_of_year = day_of_era -
                           (365U * year_of_era + year_of_era / 4U -
                            year_of_era / 100U);
    uint32_t month_part = (5U * day_of_year + 2U) / 153U;
    uint32_t day = day_of_year - (153U * month_part + 2U) / 5U + 1U;
    int32_t month = (int32_t)month_part + (month_part < 10U ? 3 : -9);
    year += month <= 2 ? 1 : 0;
    if (year < 2000 || year > 2099) {
        return false;
    }

    int64_t weekday = (days_since_epoch + 4LL) % 7LL;
    if (weekday < 0) {
        weekday += 7LL;
    }
    *out_datetime = (gw_rtc_ysn8010_datetime_t) {
        .year = (uint16_t)year,
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .weekday = (uint8_t)weekday,
        .hour = (uint8_t)(seconds_of_day / 3600U),
        .minute = (uint8_t)((seconds_of_day % 3600U) / 60U),
        .second = (uint8_t)(seconds_of_day % 60U),
    };
    return true;
}

/** @brief 获取或创建串行化授时流程的任务互斥锁。 */
static bool time_update_lock(void)
{
    portENTER_CRITICAL(&s_time_update_mutex_init_mux);
    if (s_time_update_mutex == NULL) {
        s_time_update_mutex = xSemaphoreCreateMutexStatic(&s_time_update_mutex_buffer);
    }
    SemaphoreHandle_t mutex = s_time_update_mutex;
    portEXIT_CRITICAL(&s_time_update_mutex_init_mux);
    return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

/** @brief 释放授时流程互斥锁。 */
static void time_update_unlock(void)
{
    if (s_time_update_mutex != NULL) {
        (void)xSemaphoreGive(s_time_update_mutex);
    }
}

/**
 * @brief 把可信 UTC 写入 ESP-IDF 系统时间。
 *
 * TLS 证书有效期校验读取的是 C library/system time，而不是 `gw_time_now()` 的
 * 内部基准。因此接受 MQTT、4G、SNTP 等可信候选时间时，必须先同步系统时钟。
 */
static esp_err_t apply_system_utc_ms(uint64_t utc_ms)
{
    struct timeval tv = {
        .tv_sec = (time_t)(utc_ms / 1000ULL),
        .tv_usec = (suseconds_t)((utc_ms % 1000ULL) * 1000ULL),
    };

    return settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t read_last_trusted_from_nvs(uint64_t *out_utc_ms, bool *out_valid)
{
    if (out_utc_ms == NULL || out_valid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_utc_ms = 0;
    *out_valid = false;

    uint64_t stored = 0;
    size_t out_len = 0;
    esp_err_t err = gw_storage_secure_get_blob(GW_NVS_NAMESPACE_TIME,
                                               GW_PARAM_KEY_LAST_TRUSTED_UTC,
                                               GW_STORAGE_VALUE_U64,
                                               &stored,
                                               sizeof(stored),
                                               &out_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (out_len != sizeof(stored) || !utc_in_trusted_range(stored)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_utc_ms = stored;
    *out_valid = true;
    return ESP_OK;
}

static esp_err_t save_last_trusted_to_nvs(uint64_t utc_ms)
{
    return gw_storage_secure_set_blob(GW_NVS_NAMESPACE_TIME,
                                      GW_PARAM_KEY_LAST_TRUSTED_UTC,
                                      GW_STORAGE_VALUE_U64,
                                      &utc_ms,
                                      sizeof(utc_ms));
}

/** @brief 读取跨重启恢复锁；键不存在表示从未检测到需要锁定的故障。 */
static esp_err_t read_recovery_from_nvs(bool *out_required,
                                        gw_time_recovery_reason_t *out_reason)
{
    if (out_required == NULL || out_reason == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_required = false;
    *out_reason = GW_TIME_RECOVERY_NONE;
    gw_time_recovery_record_t record = {0};
    size_t out_len = 0U;
    esp_err_t err = gw_storage_secure_get_blob(GW_NVS_NAMESPACE_TIME,
                                               GW_PARAM_KEY_TIME_RECOVERY,
                                               GW_STORAGE_VALUE_BLOB,
                                               &record,
                                               sizeof(record),
                                               &out_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (out_len != sizeof(record) || record.version != GW_TIME_RECOVERY_RECORD_VERSION ||
        record.required > 1U || record.reserved != 0U ||
        (record.required != 0U &&
         (record.reason == (uint8_t)GW_TIME_RECOVERY_NONE ||
          record.reason >= (uint8_t)GW_TIME_RECOVERY_REASON_COUNT))) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_required = record.required != 0U;
    *out_reason = *out_required ? (gw_time_recovery_reason_t)record.reason :
                                 GW_TIME_RECOVERY_NONE;
    return ESP_OK;
}

/** @brief 原子写入主备恢复锁；required=false 仅能在网络权威授时后使用。 */
static esp_err_t save_recovery_to_nvs(bool required,
                                      gw_time_recovery_reason_t reason)
{
    if (required &&
        (reason == GW_TIME_RECOVERY_NONE || reason >= GW_TIME_RECOVERY_REASON_COUNT)) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_time_recovery_record_t record = {
        .version = GW_TIME_RECOVERY_RECORD_VERSION,
        .required = required ? 1U : 0U,
        .reason = required ? (uint8_t)reason : (uint8_t)GW_TIME_RECOVERY_NONE,
        .reserved = 0U,
    };
    return gw_storage_secure_set_blob(GW_NVS_NAMESPACE_TIME,
                                      GW_PARAM_KEY_TIME_RECOVERY,
                                      GW_STORAGE_VALUE_BLOB,
                                      &record,
                                      sizeof(record));
}

/** @brief 持有授时互斥锁时，按策略保存最近可信时间。 */
static esp_err_t time_maybe_save_last_trusted_locked(bool force);

/**
 * @brief 把已接受的外部授时最佳努力回写到电池 RTC。
 *
 * 调用方必须持有 s_time_update_mutex。回写失败只影响掉电恢复能力，不能撤销
 * 已经生效的系统 UTC；详细结果通过 gw_time_status_t 暴露给 BLE 和诊断模块。
 */
static void time_sync_external_rtc_locked(uint64_t utc_ms, gw_time_source_t source)
{
    if (source == GW_TIME_SOURCE_RTC) {
        return;
    }

    uint64_t uptime_ms = uptime_ms_now();
    bool retry_blocked = false;
    portENTER_CRITICAL(&s_time_lock);
    retry_blocked = source != GW_TIME_SOURCE_BLE_PHONE && s_rtc_write_attempted &&
                    !s_rtc_last_write_succeeded &&
                    uptime_ms - s_rtc_last_write_attempt_uptime_ms <
                        GW_TIME_RTC_WRITE_RETRY_MS;
    portEXIT_CRITICAL(&s_time_lock);
    if (retry_blocked) {
        return;
    }

    gw_rtc_ysn8010_datetime_t current_datetime = {0};
    esp_err_t read_err = gw_rtc_ysn8010_read_datetime(&current_datetime);
    gw_rtc_ysn8010_status_t rtc_status = {0};
    esp_err_t status_err = gw_rtc_ysn8010_get_status(&rtc_status);
    uint64_t current_rtc_utc_ms = 0;
    bool current_rtc_valid = read_err == ESP_OK && status_err == ESP_OK &&
                             rtc_status.datetime_valid && !rtc_status.voltage_low &&
                             !rtc_status.stopped &&
                             rtc_datetime_to_utc_ms(&current_datetime,
                                                    &current_rtc_utc_ms);

    if (current_rtc_valid) {
        uint64_t difference_ms = current_rtc_utc_ms > utc_ms
                                     ? current_rtc_utc_ms - utc_ms
                                     : utc_ms - current_rtc_utc_ms;
        if (difference_ms <= GW_TIME_RTC_WRITE_SKIP_MS) {
            portENTER_CRITICAL(&s_time_lock);
            s_rtc_present = true;
            s_rtc_valid = true;
            s_rtc_vlf_latched = false;
            s_rtc_stopped = false;
            s_rtc_utc_ms = current_rtc_utc_ms;
            s_rtc_write_attempted = true;
            s_rtc_last_write_performed = false;
            s_rtc_last_write_succeeded = true;
            s_rtc_last_write_attempt_uptime_ms = uptime_ms;
            s_rtc_last_read_error = ESP_OK;
            s_rtc_last_write_error = ESP_OK;
            portEXIT_CRITICAL(&s_time_lock);
            return;
        }
    }

    gw_rtc_ysn8010_datetime_t target_datetime = {0};
    esp_err_t write_err = utc_ms_to_rtc_datetime(utc_ms, &target_datetime)
                              ? ESP_OK
                              : ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_time_lock);
    s_rtc_write_attempted = true;
    s_rtc_last_write_performed = write_err == ESP_OK;
    s_rtc_last_write_succeeded = false;
    s_rtc_last_write_attempt_uptime_ms = uptime_ms;
    portEXIT_CRITICAL(&s_time_lock);

    if (write_err == ESP_OK) {
        write_err = gw_rtc_ysn8010_write_datetime(&target_datetime);
    }

    memset(&rtc_status, 0, sizeof(rtc_status));
    status_err = gw_rtc_ysn8010_get_status(&rtc_status);
    uint64_t verified_utc_ms = 0;
    bool write_succeeded = write_err == ESP_OK && status_err == ESP_OK &&
                           rtc_status.datetime_valid && !rtc_status.voltage_low &&
                           !rtc_status.stopped &&
                           rtc_datetime_to_utc_ms(&rtc_status.last_datetime,
                                                  &verified_utc_ms);
    if (write_err == ESP_OK && !write_succeeded) {
        write_err = status_err != ESP_OK ? status_err : ESP_ERR_INVALID_RESPONSE;
    }

    portENTER_CRITICAL(&s_time_lock);
    s_rtc_present = rtc_status.present;
    s_rtc_valid = write_succeeded;
    s_rtc_vlf_latched = rtc_status.voltage_low;
    s_rtc_stopped = rtc_status.stopped;
    s_rtc_utc_ms = write_succeeded ? verified_utc_ms : 0;
    s_rtc_last_write_succeeded = write_succeeded;
    s_rtc_last_read_error = write_succeeded ? ESP_OK : read_err;
    s_rtc_last_write_error = write_err;
    portEXIT_CRITICAL(&s_time_lock);

    if (write_succeeded) {
        ESP_LOGI(TAG,
                 "external RTC synchronized: source=%s utc=%" PRIu64,
                 gw_time_source_to_string(source),
                 verified_utc_ms);
    } else {
        ESP_LOGW(TAG,
                 "external RTC synchronization failed: source=%s err=%s",
                 gw_time_source_to_string(source),
                 esp_err_to_name(write_err));
    }
}

esp_err_t gw_time_init(void)
{
    if (!time_update_lock()) {
        return ESP_ERR_NO_MEM;
    }

    uint64_t last_trusted_utc_ms = 0;
    bool last_trusted_valid = false;
    esp_err_t err = read_last_trusted_from_nvs(&last_trusted_utc_ms, &last_trusted_valid);

    bool nvs_value_invalid = persisted_time_value_corrupted(err);
    esp_err_t nvs_value_error = nvs_value_invalid ? err : ESP_OK;
    if (nvs_value_invalid) {
        ESP_LOGW(TAG,
                 "last trusted utc state invalid, require network recovery: %s",
                 esp_err_to_name(err));
        err = ESP_OK;
        last_trusted_utc_ms = 0;
        last_trusted_valid = false;
    }
    if (err != ESP_OK) {
        time_update_unlock();
        ESP_LOGE(TAG, "read last trusted utc failed: %s", esp_err_to_name(err));
        return err;
    }

    bool persisted_recovery_required = false;
    gw_time_recovery_reason_t persisted_recovery_reason = GW_TIME_RECOVERY_NONE;
    esp_err_t recovery_read_err = read_recovery_from_nvs(&persisted_recovery_required,
                                                         &persisted_recovery_reason);
    if (recovery_read_err != ESP_OK) {
        /* 恢复锁本身损坏时必须失败关闭，不能因读不到锁而重新启用定时任务。 */
        ESP_LOGE(TAG,
                 "read time recovery lock failed, keep scheduler locked: %s",
                 esp_err_to_name(recovery_read_err));
        persisted_recovery_required = true;
        persisted_recovery_reason = GW_TIME_RECOVERY_STATE_INVALID;
    }

    /* 外部 RTC 故障不能阻止网关启动，但必须锁住调度，等待网络重新授时。 */
    esp_err_t rtc_init_err = gw_rtc_ysn8010_init();
    gw_rtc_ysn8010_datetime_t rtc_datetime = {0};
    esp_err_t rtc_read_err = rtc_init_err;
    if (rtc_init_err == ESP_OK) {
        rtc_read_err = gw_rtc_ysn8010_read_datetime(&rtc_datetime);
    }
    gw_rtc_ysn8010_status_t rtc_status = {0};
    esp_err_t rtc_status_err = gw_rtc_ysn8010_get_status(&rtc_status);
    if (rtc_status_err != ESP_OK && rtc_read_err == ESP_OK) {
        rtc_read_err = rtc_status_err;
    }

    uint64_t rtc_utc_ms = 0;
    bool rtc_time_read = rtc_read_err == ESP_OK &&
                         !rtc_status.voltage_low &&
                         !rtc_status.stopped &&
                         rtc_datetime_to_utc_ms(&rtc_datetime, &rtc_utc_ms);
    bool rtc_progress_failed = false;
    if (rtc_time_read) {
        /*
         * VLF/STOP 都正常仍不足以证明晶振在走。启动时等待跨过一个秒边界再读一次，
         * 可识别寄存器保持着合法旧日期、但振荡器已经停走的隐蔽故障。
         */
        uint64_t progress_start_uptime_ms = uptime_ms_now();
        vTaskDelay(pdMS_TO_TICKS(GW_TIME_RTC_PROGRESS_CHECK_MS) + 1U);

        gw_rtc_ysn8010_datetime_t second_datetime = {0};
        esp_err_t second_read_err = gw_rtc_ysn8010_read_datetime(&second_datetime);
        gw_rtc_ysn8010_status_t second_status = {0};
        esp_err_t second_status_err = gw_rtc_ysn8010_get_status(&second_status);
        uint64_t second_utc_ms = 0;
        uint64_t progress_elapsed_ms = uptime_ms_now() - progress_start_uptime_ms;
        bool second_read_valid = second_read_err == ESP_OK &&
                                 second_status_err == ESP_OK &&
                                 !second_status.voltage_low &&
                                 !second_status.stopped &&
                                 rtc_datetime_to_utc_ms(&second_datetime,
                                                        &second_utc_ms);
        bool rtc_advanced = second_read_valid && second_utc_ms > rtc_utc_ms &&
                            second_utc_ms - rtc_utc_ms <=
                                progress_elapsed_ms + GW_TIME_RTC_PROGRESS_SLACK_MS;
        if (rtc_advanced) {
            rtc_datetime = second_datetime;
            rtc_status = second_status;
            rtc_utc_ms = second_utc_ms;
            rtc_read_err = ESP_OK;
        } else {
            rtc_progress_failed = second_read_valid;
            rtc_status = second_status;
            rtc_read_err = second_read_err != ESP_OK ? second_read_err :
                           (second_status_err != ESP_OK ? second_status_err :
                                                         ESP_ERR_INVALID_STATE);
            rtc_time_read = false;
        }
    }
    bool rtc_time_trusted = false;
    bool rtc_rollback = false;
    gw_time_recovery_reason_t recovery_reason = GW_TIME_RECOVERY_NONE;
    if (rtc_time_read && utc_in_trusted_range(rtc_utc_ms)) {
        if (last_trusted_valid &&
            rtc_utc_ms + (uint64_t)GW_TIME_ROLLBACK_TOLERANCE_MS < last_trusted_utc_ms) {
            rtc_rollback = true;
            recovery_reason = GW_TIME_RECOVERY_RTC_ROLLBACK;
        } else {
            rtc_time_trusted = true;
        }
    } else if (!rtc_status.present) {
        recovery_reason = GW_TIME_RECOVERY_RTC_MISSING;
    } else if (rtc_status.voltage_low) {
        /* VLF 是电池脱落、耗尽或保持电压不足的硬件锁存证据。 */
        recovery_reason = GW_TIME_RECOVERY_RTC_VOLTAGE_LOW;
    } else if (rtc_status.stopped) {
        recovery_reason = GW_TIME_RECOVERY_RTC_STOPPED;
    } else if (rtc_read_err == ESP_ERR_INVALID_RESPONSE) {
        recovery_reason = GW_TIME_RECOVERY_RTC_INVALID_CALENDAR;
    } else if (rtc_progress_failed) {
        recovery_reason = GW_TIME_RECOVERY_RTC_NOT_ADVANCING;
    } else if (rtc_read_err != ESP_OK) {
        recovery_reason = GW_TIME_RECOVERY_RTC_READ_FAILED;
    } else {
        recovery_reason = GW_TIME_RECOVERY_RTC_OUT_OF_RANGE;
    }

    bool rtc_time_valid = rtc_time_trusted;
    bool recovery_required = !rtc_time_valid || persisted_recovery_required ||
                             nvs_value_invalid;
    if (rtc_time_valid && (persisted_recovery_required || nvs_value_invalid)) {
        /* RTC 恢复正常或检查点损坏时都不能自行解锁，必须等待网络重建状态。 */
        rtc_time_trusted = false;
        recovery_reason = nvs_value_invalid ? GW_TIME_RECOVERY_STATE_INVALID :
                                              persisted_recovery_reason;
    }

    if (rtc_time_trusted) {
        err = apply_system_utc_ms(rtc_utc_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "apply external RTC time failed: %s", esp_err_to_name(err));
            rtc_time_trusted = false;
            recovery_required = true;
            recovery_reason = GW_TIME_RECOVERY_SYSTEM_CLOCK_FAILED;
        }
    }

    esp_err_t recovery_persist_err = ESP_OK;
    bool recovery_record_needs_update = recovery_required &&
                                        (recovery_read_err != ESP_OK ||
                                         !persisted_recovery_required ||
                                         persisted_recovery_reason != recovery_reason);
    if (recovery_record_needs_update) {
        recovery_persist_err = save_recovery_to_nvs(true, recovery_reason);
        if (recovery_persist_err != ESP_OK) {
            /* 写锁失败时 RAM 仍保持锁定，本次启动绝不降级为可调度。 */
            ESP_LOGE(TAG,
                     "persist time recovery lock failed: reason=%s err=%s",
                     gw_time_recovery_reason_to_string(recovery_reason),
                     esp_err_to_name(recovery_persist_err));
        }
    }

    uint64_t uptime_ms = uptime_ms_now();

    portENTER_CRITICAL(&s_time_lock);
    s_last_trusted_utc_ms = last_trusted_utc_ms;
    s_last_trusted_valid = last_trusted_valid;
    s_rollback_detected = rtc_rollback;
    s_recovery_required = recovery_required;
    s_recovery_reason = recovery_reason;
    s_rtc_utc_ms = rtc_time_read ? rtc_utc_ms : 0;
    s_rtc_present = rtc_status.present;
    s_rtc_valid = rtc_time_valid;
    s_rtc_vlf_latched = rtc_status.voltage_low;
    s_rtc_stopped = rtc_status.stopped;
    s_rtc_write_attempted = false;
    s_rtc_last_write_performed = false;
    s_rtc_last_write_succeeded = false;
    s_rtc_last_read_error = rtc_read_err;
    s_rtc_last_write_error = ESP_OK;
    if (rtc_time_trusted) {
        s_utc_trusted = true;
        s_utc_base_ms = rtc_utc_ms;
        s_uptime_base_ms = uptime_ms;
        s_time_source = GW_TIME_SOURCE_RTC;
        s_last_sync_utc_ms = rtc_utc_ms;
        s_last_sync_source = GW_TIME_SOURCE_RTC;
        s_last_error = ESP_OK;
    } else {
        s_utc_trusted = false;
        s_utc_base_ms = 0;
        s_uptime_base_ms = uptime_ms;
        s_time_source = GW_TIME_SOURCE_NONE;
        s_last_error = recovery_persist_err != ESP_OK ? recovery_persist_err :
                       (nvs_value_invalid ? nvs_value_error :
                                             ESP_ERR_INVALID_STATE);
    }
    portEXIT_CRITICAL(&s_time_lock);

    ESP_LOGI(TAG,
             "time initialized: last_trusted_valid=%s last_trusted_utc=%" PRIu64
             " rtc_present=%s rtc_read=%s rtc_valid=%s rtc_vlf=%s rtc_stop=%s"
             " rtc_utc=%" PRIu64 " rtc_err=%s recovery_required=%s reason=%s",
             last_trusted_valid ? "true" : "false",
             last_trusted_utc_ms,
             rtc_status.present ? "true" : "false",
             rtc_time_read ? "true" : "false",
             rtc_time_valid ? "true" : "false",
             rtc_status.voltage_low ? "true" : "false",
             rtc_status.stopped ? "true" : "false",
             rtc_utc_ms,
             esp_err_to_name(rtc_read_err),
             recovery_required ? "true" : "false",
             gw_time_recovery_reason_to_string(recovery_reason));
    time_update_unlock();
    return ESP_OK;
}

gw_time_point_t gw_time_now(void)
{
    uint64_t uptime_ms = uptime_ms_now();
    uint64_t utc_ms = 0;
    bool utc_trusted;
    uint64_t utc_base_ms;
    uint64_t uptime_base_ms;
    gw_time_source_t source;

    portENTER_CRITICAL(&s_time_lock);
    utc_trusted = s_utc_trusted;
    utc_base_ms = s_utc_base_ms;
    uptime_base_ms = s_uptime_base_ms;
    source = s_time_source;
    portEXIT_CRITICAL(&s_time_lock);

    if (utc_trusted) {
        utc_ms = utc_base_ms + (uptime_ms - uptime_base_ms);
    }

    return (gw_time_point_t) {
        .utc_ms = utc_ms,
        .uptime_ms = uptime_ms,
        .utc_trusted = utc_trusted,
        .source = source,
    };
}

void gw_time_set_untrusted(void)
{
    bool update_locked = time_update_lock();
    uint64_t uptime_ms = uptime_ms_now();
    gw_time_recovery_reason_t reason = GW_TIME_RECOVERY_MANUAL_INVALIDATION;

    portENTER_CRITICAL(&s_time_lock);
    s_utc_trusted = false;
    s_utc_base_ms = 0;
    s_uptime_base_ms = uptime_ms;
    s_time_source = GW_TIME_SOURCE_NONE;
    s_recovery_required = true;
    if (s_recovery_reason == GW_TIME_RECOVERY_NONE) {
        s_recovery_reason = GW_TIME_RECOVERY_MANUAL_INVALIDATION;
    }
    reason = s_recovery_reason;
    s_last_error = ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&s_time_lock);

    if (update_locked) {
        esp_err_t persist_err = save_recovery_to_nvs(true, reason);
        if (persist_err != ESP_OK) {
            portENTER_CRITICAL(&s_time_lock);
            s_last_error = persist_err;
            portEXIT_CRITICAL(&s_time_lock);
            ESP_LOGE(TAG, "persist manual time invalidation failed: %s",
                     esp_err_to_name(persist_err));
        }
        time_update_unlock();
    }
}

void gw_time_set_trusted(uint64_t utc_ms, gw_time_source_t source)
{
    esp_err_t err = gw_scheduler_time_change_begin();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pause scheduler before trusted time failed: %s",
                 esp_err_to_name(err));
        return;
    }

    gw_time_point_t before = gw_time_now();
    uint64_t difference_ms = before.utc_ms > utc_ms ? before.utc_ms - utc_ms
                                                     : utc_ms - before.utc_ms;
    bool reset_daily_boundary = !before.utc_trusted ||
                                difference_ms >
                                    (uint64_t)GW_TIME_ROLLBACK_TOLERANCE_MS;
    err = gw_time_accept_candidate(utc_ms, source);
    esp_err_t finish_err =
        gw_scheduler_time_change_finish(err == ESP_OK && reset_daily_boundary);
    if (finish_err != ESP_OK) {
        ESP_LOGW(TAG, "finish scheduler after trusted time failed: %s",
                 esp_err_to_name(finish_err));
    }
}

esp_err_t gw_time_accept_candidate(uint64_t utc_ms, gw_time_source_t source)
{
    if (!source_valid(source)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gw_scheduler_time_change_owned_by_current_task()) {
        /* 所有运行期授时必须先冻结调度，禁止发布“半完成边界”的新 UTC。 */
        ESP_LOGE(TAG,
                 "reject uncoordinated utc candidate: source=%s",
                 gw_time_source_to_string(source));
        return ESP_ERR_INVALID_STATE;
    }
    if (!time_update_lock()) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    gw_time_point_t current = gw_time_now();
    bool network_authoritative = source_is_network_authoritative(source);
    bool recovery_required = false;
    bool checkpoint_needs_rebase = false;

    portENTER_CRITICAL(&s_time_lock);
    uint64_t last_trusted_utc_ms = s_last_trusted_utc_ms;
    bool last_trusted_valid = s_last_trusted_valid;
    recovery_required = s_recovery_required;

    if (!source_valid(source)) {
        err = ESP_ERR_INVALID_ARG;
        s_last_error = err;
    } else if (!utc_in_trusted_range(utc_ms)) {
        err = ESP_ERR_INVALID_ARG;
        s_last_error = err;
    } else if (recovery_required && !network_authoritative) {
        /* RTC 故障锁定后，手机时间不能把定时调度重新打开。 */
        err = ESP_ERR_INVALID_STATE;
        s_last_error = err;
    } else if (!network_authoritative && last_trusted_valid &&
               utc_ms + (uint64_t)GW_TIME_ROLLBACK_TOLERANCE_MS < last_trusted_utc_ms) {
        err = ESP_ERR_INVALID_STATE;
        s_rollback_detected = true;
        s_last_error = err;
    } else if (!network_authoritative && current.utc_trusted &&
               utc_ms + (uint64_t)GW_TIME_ROLLBACK_TOLERANCE_MS < current.utc_ms) {
        err = ESP_ERR_INVALID_STATE;
        s_rollback_detected = true;
        s_last_error = err;
    }
    checkpoint_needs_rebase = network_authoritative &&
                              (recovery_required ||
                               (last_trusted_valid &&
                                utc_ms + (uint64_t)GW_TIME_ROLLBACK_TOLERANCE_MS <
                                    last_trusted_utc_ms));
    portEXIT_CRITICAL(&s_time_lock);

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "reject utc candidate: source=%s utc=%" PRIu64 " err=%s",
                 gw_time_source_to_string(source),
                 utc_ms,
                 esp_err_to_name(err));
        time_update_unlock();
        return err;
    }

    err = apply_system_utc_ms(utc_ms);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_time_lock);
        s_last_error = err;
        portEXIT_CRITICAL(&s_time_lock);
        ESP_LOGW(TAG,
                 "apply system utc failed: source=%s utc=%" PRIu64 " err=%s",
                 gw_time_source_to_string(source),
                 utc_ms,
                 esp_err_to_name(err));
        time_update_unlock();
        return err;
    }

    uint64_t uptime_ms = uptime_ms_now();

    if (!recovery_required) {
        portENTER_CRITICAL(&s_time_lock);
        s_utc_trusted = true;
        s_utc_base_ms = utc_ms;
        s_uptime_base_ms = uptime_ms;
        s_time_source = source;
        s_last_sync_utc_ms = utc_ms;
        s_last_sync_source = source;
        s_last_error = ESP_OK;
        portEXIT_CRITICAL(&s_time_lock);
    }

    time_sync_external_rtc_locked(utc_ms, source);
    esp_err_t persist_err = ESP_OK;
    if (checkpoint_needs_rebase) {
        /*
         * 权威网络时间允许向后修正错误检查点。若不重建 NVS，正确 RTC 会在下次
         * 启动再次被旧检查点判成回退，设备将永久要求联网恢复。
         */
        persist_err = save_last_trusted_to_nvs(utc_ms);
        portENTER_CRITICAL(&s_time_lock);
        if (persist_err == ESP_OK) {
            s_last_trusted_utc_ms = utc_ms;
            s_last_trusted_valid = true;
            s_last_error = ESP_OK;
        } else {
            s_last_error = persist_err;
        }
        portEXIT_CRITICAL(&s_time_lock);
    } else {
        persist_err = time_maybe_save_last_trusted_locked(false);
    }
    if (persist_err != ESP_OK) {
        /* 普通校时下检查点写失败不撤销 UTC；恢复锁仍按下面的独立写入结果处理。 */
        ESP_LOGW(TAG,
                 "save accepted utc failed: source=%s err=%s",
                 gw_time_source_to_string(source),
                 esp_err_to_name(persist_err));
    }

    if (recovery_required) {
        /*
         * 只有网络来源能走到这里。必须先把“已恢复”写入主备 NVS，再开放 RAM
         * 调度资格；若清锁写失败，本次启动也继续保持 UTC 不可信，促使网络重试。
         */
        esp_err_t clear_err = save_recovery_to_nvs(false, GW_TIME_RECOVERY_NONE);
        if (clear_err != ESP_OK) {
            portENTER_CRITICAL(&s_time_lock);
            s_utc_trusted = false;
            s_utc_base_ms = 0;
            s_uptime_base_ms = uptime_ms;
            s_time_source = GW_TIME_SOURCE_NONE;
            s_recovery_required = true;
            s_last_error = clear_err;
            portEXIT_CRITICAL(&s_time_lock);
            ESP_LOGE(TAG,
                     "network time applied but recovery lock clear failed: %s",
                     esp_err_to_name(clear_err));
            time_update_unlock();
            return clear_err;
        }

        portENTER_CRITICAL(&s_time_lock);
        s_utc_trusted = true;
        s_utc_base_ms = utc_ms;
        s_uptime_base_ms = uptime_ms;
        s_time_source = source;
        s_last_sync_utc_ms = utc_ms;
        s_last_sync_source = source;
        s_recovery_required = false;
        s_last_error = persist_err;
        portEXIT_CRITICAL(&s_time_lock);
        ESP_LOGI(TAG,
                 "persistent time recovery lock cleared by source=%s",
                 gw_time_source_to_string(source));
    }
    time_update_unlock();
    return ESP_OK;
}

static esp_err_t time_maybe_save_last_trusted_locked(bool force)
{
    gw_time_point_t now = gw_time_now();
    if (!now.utc_trusted) {
        return ESP_OK;
    }

    bool should_save = false;
    uint64_t last_trusted_utc_ms = 0;
    bool last_trusted_valid = false;

    portENTER_CRITICAL(&s_time_lock);
    last_trusted_utc_ms = s_last_trusted_utc_ms;
    last_trusted_valid = s_last_trusted_valid;
    if (!last_trusted_valid) {
        should_save = true;
    } else if (now.utc_ms > last_trusted_utc_ms) {
        uint64_t advance_ms = now.utc_ms - last_trusted_utc_ms;
        should_save = force ||
                      advance_ms >= (uint64_t)GW_TIME_SAVE_ADVANCE_INTERVAL_SEC * 1000ULL;
    } else {
        s_last_error = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_time_lock);

    if (!should_save) {
        return ESP_OK;
    }

    esp_err_t err = save_last_trusted_to_nvs(now.utc_ms);

    portENTER_CRITICAL(&s_time_lock);
    if (err == ESP_OK) {
        s_last_trusted_utc_ms = now.utc_ms;
        s_last_trusted_valid = true;
        s_last_error = ESP_OK;
    } else {
        s_last_error = err;
    }
    portEXIT_CRITICAL(&s_time_lock);

    return err;
}

esp_err_t gw_time_maybe_save_last_trusted(bool force)
{
    if (!time_update_lock()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = time_maybe_save_last_trusted_locked(force);
    time_update_unlock();
    return err;
}

esp_err_t gw_time_get_status(gw_time_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->now = gw_time_now();

    portENTER_CRITICAL(&s_time_lock);
    out->last_trusted_utc_ms = s_last_trusted_utc_ms;
    out->last_sync_utc_ms = s_last_sync_utc_ms;
    out->rtc_utc_ms = s_rtc_utc_ms;
    out->last_sync_source = s_last_sync_source;
    out->recovery_reason = s_recovery_reason;
    out->last_trusted_valid = s_last_trusted_valid;
    out->rollback_detected = s_rollback_detected;
    out->recovery_required = s_recovery_required;
    out->scheduler_time_trusted = out->now.utc_trusted && !s_recovery_required;
    out->rtc_present = s_rtc_present;
    out->rtc_valid = s_rtc_valid;
    out->rtc_vlf_latched = s_rtc_vlf_latched;
    out->rtc_stopped = s_rtc_stopped;
    out->rtc_write_attempted = s_rtc_write_attempted;
    out->rtc_last_write_performed = s_rtc_last_write_performed;
    out->rtc_last_write_succeeded = s_rtc_last_write_succeeded;
    out->rtc_last_read_error = s_rtc_last_read_error;
    out->rtc_last_write_error = s_rtc_last_write_error;
    out->last_error = s_last_error;
    portEXIT_CRITICAL(&s_time_lock);

    return ESP_OK;
}

const char *gw_time_source_to_string(gw_time_source_t source)
{
    switch (source) {
    case GW_TIME_SOURCE_NONE:
        return "none";
    case GW_TIME_SOURCE_RTC:
        return "rtc";
    case GW_TIME_SOURCE_4G:
        return "module_4g";
    case GW_TIME_SOURCE_MQTT:
        return "mqtt";
    case GW_TIME_SOURCE_HTTPS:
        return "https";
    case GW_TIME_SOURCE_SNTP:
        return "sntp";
    case GW_TIME_SOURCE_BLE_PHONE:
        return "ble_phone";
    default:
        return "unknown";
    }
}

const char *gw_time_recovery_reason_to_string(gw_time_recovery_reason_t reason)
{
    switch (reason) {
    case GW_TIME_RECOVERY_NONE:
        return "none";
    case GW_TIME_RECOVERY_RTC_MISSING:
        return "rtc_missing";
    case GW_TIME_RECOVERY_RTC_VOLTAGE_LOW:
        return "rtc_voltage_low";
    case GW_TIME_RECOVERY_RTC_STOPPED:
        return "rtc_stopped";
    case GW_TIME_RECOVERY_RTC_INVALID_CALENDAR:
        return "rtc_invalid_calendar";
    case GW_TIME_RECOVERY_RTC_READ_FAILED:
        return "rtc_read_failed";
    case GW_TIME_RECOVERY_RTC_NOT_ADVANCING:
        return "rtc_not_advancing";
    case GW_TIME_RECOVERY_RTC_OUT_OF_RANGE:
        return "rtc_out_of_range";
    case GW_TIME_RECOVERY_RTC_ROLLBACK:
        return "rtc_rollback";
    case GW_TIME_RECOVERY_SYSTEM_CLOCK_FAILED:
        return "system_clock_failed";
    case GW_TIME_RECOVERY_MANUAL_INVALIDATION:
        return "manual_invalidation";
    case GW_TIME_RECOVERY_STATE_INVALID:
        return "recovery_state_invalid";
    case GW_TIME_RECOVERY_REASON_COUNT:
        return "unknown";
    default:
        return "unknown";
    }
}
