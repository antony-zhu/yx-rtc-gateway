/**
 * @file gw_rtc_ysn8010.c
 * @brief YSN8010（按 RX8010SJ 时钟寄存器兼容）外部 RTC 驱动实现。
 */

#include "gw_rtc_ysn8010.h"

#include <stddef.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define GW_RTC_I2C_TIMEOUT_MS             100
#define GW_RTC_POWER_STABILIZE_MS         40U
#define GW_RTC_WRITE_VERIFY_ATTEMPTS      3U
#define GW_RTC_WRITE_VERIFY_RETRY_MS      10U

#define GW_RTC_REG_SECONDS                0x10U
#define GW_RTC_REG_FLAG                   0x1EU
#define GW_RTC_REG_CONTROL                0x1FU
#define GW_RTC_REG_DIGITAL_OFFSET         0x30U
#define GW_RTC_REG_RESERVED_31            0x31U
#define GW_RTC_REG_IRQ_CONTROL            0x32U

#define GW_RTC_FLAG_VLF                   0x02U
#define GW_RTC_CONTROL_STOP               0x40U

#define GW_RTC_STATUS_READ_COUNT          16U
#define GW_RTC_CLOCK_MODE_BLOCK_COUNT     14U
#define GW_RTC_MAX_WRITE_DATA_COUNT       GW_RTC_STATUS_READ_COUNT

#define GW_RTC_RESERVED_17_VALUE          0xD8U
#define GW_RTC_ALARM_DISABLED_VALUE       0x80U
#define GW_RTC_CLOCK_ONLY_EXTENSION       0x04U

static const char *TAG = "gw_rtc";

typedef struct {
    bool initialized;
    bool present;
    bool datetime_valid;
    bool voltage_low;
    bool stopped;
    uint8_t flag_register;
    uint8_t control_register;
    gw_rtc_ysn8010_datetime_t last_datetime;
    esp_err_t last_error;
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t device_handle;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buffer;
} gw_rtc_ysn8010_state_t;

static gw_rtc_ysn8010_state_t s_rtc;
static portMUX_TYPE s_rtc_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

/** @brief 获取模块互斥锁；驱动接口只允许在任务上下文调用。 */
static bool rtc_lock(void)
{
    portENTER_CRITICAL(&s_rtc_lock_init_mux);
    if (s_rtc.lock == NULL) {
        s_rtc.lock = xSemaphoreCreateMutexStatic(&s_rtc.lock_buffer);
    }
    SemaphoreHandle_t lock = s_rtc.lock;
    portEXIT_CRITICAL(&s_rtc_lock_init_mux);
    return lock != NULL && xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE;
}

/** @brief 释放模块互斥锁。 */
static void rtc_unlock(void)
{
    if (s_rtc.lock != NULL) {
        (void)xSemaphoreGive(s_rtc.lock);
    }
}

/** @brief 判断公历年份是否为闰年。 */
static bool rtc_is_leap_year(uint16_t year)
{
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

/** @brief 返回指定月份的天数；月份非法时返回 0。 */
static uint8_t rtc_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {31U, 28U, 31U, 30U, 31U, 30U,
                                   31U, 31U, 30U, 31U, 30U, 31U};
    if (month < 1U || month > 12U) {
        return 0U;
    }
    if (month == 2U && rtc_is_leap_year(year)) {
        return 29U;
    }
    return days[month - 1U];
}

/** @brief 计算公历星期，返回 0=星期日到 6=星期六。 */
static uint8_t rtc_calculate_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    static const uint8_t offsets[] = {0U, 3U, 2U, 5U, 0U, 3U,
                                      5U, 1U, 4U, 6U, 2U, 4U};
    uint32_t adjusted_year = year;
    if (month < 3U) {
        adjusted_year--;
    }
    return (uint8_t)((adjusted_year + adjusted_year / 4U - adjusted_year / 100U +
                      adjusted_year / 400U + offsets[month - 1U] + day) % 7U);
}

/** @brief 校验调用方提供的完整日历字段及星期一致性。 */
static bool rtc_datetime_is_valid(const gw_rtc_ysn8010_datetime_t *datetime)
{
    if (datetime == NULL || datetime->year < 2000U || datetime->year > 2099U ||
        datetime->month < 1U || datetime->month > 12U ||
        datetime->hour > 23U || datetime->minute > 59U || datetime->second > 59U ||
        datetime->weekday > 6U) {
        return false;
    }

    uint8_t max_day = rtc_days_in_month(datetime->year, datetime->month);
    return datetime->day >= 1U && datetime->day <= max_day &&
           datetime->weekday == rtc_calculate_weekday(datetime->year,
                                                       datetime->month,
                                                       datetime->day);
}

/** @brief 将 0..99 转为 BCD。调用前必须完成范围校验。 */
static uint8_t rtc_binary_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

/** @brief 解码掩码后的 BCD 字节，同时拒绝非法十进制半字节。 */
static bool rtc_bcd_to_binary(uint8_t raw, uint8_t mask, uint8_t *out_value)
{
    if (out_value == NULL || (raw & (uint8_t)~mask) != 0U) {
        return false;
    }
    uint8_t bcd = raw & mask;
    uint8_t high = (bcd >> 4U) & 0x0FU;
    uint8_t low = bcd & 0x0FU;
    if (high > 9U || low > 9U) {
        return false;
    }
    *out_value = (uint8_t)(high * 10U + low);
    return true;
}

/** @brief 记录一次成功的硬件访问。 */
static void rtc_record_io_success_locked(void)
{
    s_rtc.present = true;
    s_rtc.last_error = ESP_OK;
}

/** @brief 记录底层 I2C 访问错误；失败后在线状态按未知/离线处理。 */
static void rtc_record_io_error_locked(esp_err_t err)
{
    s_rtc.present = false;
    s_rtc.datetime_valid = false;
    s_rtc.last_error = err;
}

/** @brief 在已持锁状态下探测 RTC 地址。 */
static esp_err_t rtc_probe_locked(void)
{
    if (!s_rtc.initialized || s_rtc.bus_handle == NULL) {
        s_rtc.last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = i2c_master_probe(s_rtc.bus_handle,
                                     GW_RTC_YSN8010_I2C_ADDRESS,
                                     GW_RTC_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        rtc_record_io_success_locked();
    } else {
        rtc_record_io_error_locked(err);
    }
    return err;
}

/** @brief 从指定寄存器连续读取数据，调用方必须持有模块锁。 */
static esp_err_t rtc_read_registers_locked(uint8_t start_register,
                                           uint8_t *data,
                                           size_t data_len)
{
    if (!s_rtc.initialized || s_rtc.device_handle == NULL) {
        s_rtc.last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || data_len == 0U) {
        s_rtc.last_error = ESP_ERR_INVALID_ARG;
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = i2c_master_transmit_receive(s_rtc.device_handle,
                                                &start_register,
                                                sizeof(start_register),
                                                data,
                                                data_len,
                                                GW_RTC_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        rtc_record_io_success_locked();
    } else {
        rtc_record_io_error_locked(err);
    }
    return err;
}

/** @brief 从指定寄存器开始连续写数据，调用方必须持有模块锁。 */
static esp_err_t rtc_write_registers_locked(uint8_t start_register,
                                            const uint8_t *data,
                                            size_t data_len)
{
    if (!s_rtc.initialized || s_rtc.device_handle == NULL) {
        s_rtc.last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || data_len == 0U || data_len > GW_RTC_MAX_WRITE_DATA_COUNT) {
        s_rtc.last_error = ESP_ERR_INVALID_ARG;
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[1U + GW_RTC_MAX_WRITE_DATA_COUNT];
    tx[0] = start_register;
    memcpy(&tx[1], data, data_len);

    esp_err_t err = i2c_master_transmit(s_rtc.device_handle,
                                        tx,
                                        data_len + 1U,
                                        GW_RTC_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        rtc_record_io_success_locked();
    } else {
        rtc_record_io_error_locked(err);
    }
    return err;
}

/** @brief 写单个寄存器，调用方必须持有模块锁。 */
static esp_err_t rtc_write_register_locked(uint8_t register_address, uint8_t value)
{
    return rtc_write_registers_locked(register_address, &value, sizeof(value));
}

/** @brief 从 RX8010SJ 星期单热码解出 0..6。 */
static bool rtc_decode_weekday(uint8_t raw, uint8_t *out_weekday)
{
    uint8_t bits = raw & 0x7FU;
    if (out_weekday == NULL || (raw & 0x80U) != 0U || bits == 0U ||
        (bits & (uint8_t)(bits - 1U)) != 0U) {
        return false;
    }

    for (uint8_t weekday = 0U; weekday <= 6U; ++weekday) {
        if (bits == (uint8_t)(1U << weekday)) {
            *out_weekday = weekday;
            return true;
        }
    }
    return false;
}

/** @brief 在已持锁状态下读取、解码并校验日历。 */
static esp_err_t rtc_read_datetime_locked(gw_rtc_ysn8010_datetime_t *out_datetime)
{
    uint8_t raw[GW_RTC_STATUS_READ_COUNT] = {0};
    memset(out_datetime, 0, sizeof(*out_datetime));

    esp_err_t err = rtc_read_registers_locked(GW_RTC_REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t flag = raw[GW_RTC_REG_FLAG - GW_RTC_REG_SECONDS];
    uint8_t control = raw[GW_RTC_REG_CONTROL - GW_RTC_REG_SECONDS];
    s_rtc.flag_register = flag;
    s_rtc.control_register = control;
    s_rtc.voltage_low = (flag & GW_RTC_FLAG_VLF) != 0U;
    s_rtc.stopped = (control & GW_RTC_CONTROL_STOP) != 0U;
    s_rtc.datetime_valid = false;

    /* VLF=1 表示数据丢失，STOP=1 表示计时未运行，两者均不得“猜测可用”。 */
    if (s_rtc.voltage_low || s_rtc.stopped) {
        memset(&s_rtc.last_datetime, 0, sizeof(s_rtc.last_datetime));
        s_rtc.last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    gw_rtc_ysn8010_datetime_t decoded = {0};
    uint8_t year_two_digits = 0U;
    bool bcd_ok = rtc_bcd_to_binary(raw[0], 0x7FU, &decoded.second) &&
                  rtc_bcd_to_binary(raw[1], 0x7FU, &decoded.minute) &&
                  rtc_bcd_to_binary(raw[2], 0x3FU, &decoded.hour) &&
                  rtc_decode_weekday(raw[3], &decoded.weekday) &&
                  rtc_bcd_to_binary(raw[4], 0x3FU, &decoded.day) &&
                  rtc_bcd_to_binary(raw[5], 0x1FU, &decoded.month) &&
                  rtc_bcd_to_binary(raw[6], 0xFFU, &year_two_digits);
    decoded.year = (uint16_t)(2000U + year_two_digits);

    if (!bcd_ok || !rtc_datetime_is_valid(&decoded)) {
        memset(&s_rtc.last_datetime, 0, sizeof(s_rtc.last_datetime));
        s_rtc.last_error = ESP_ERR_INVALID_RESPONSE;
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_datetime = decoded;
    s_rtc.last_datetime = decoded;
    s_rtc.datetime_valid = true;
    s_rtc.last_error = ESP_OK;
    return ESP_OK;
}

/** @brief 将合法日历转换为从 2000-01-01 起累计的秒，用于回读容差比较。 */
static uint64_t rtc_datetime_to_serial_seconds(const gw_rtc_ysn8010_datetime_t *datetime)
{
    uint32_t days = 0U;
    for (uint16_t year = 2000U; year < datetime->year; ++year) {
        days += rtc_is_leap_year(year) ? 366U : 365U;
    }
    for (uint8_t month = 1U; month < datetime->month; ++month) {
        days += rtc_days_in_month(datetime->year, month);
    }
    days += (uint32_t)datetime->day - 1U;

    return (uint64_t)days * 86400ULL + (uint64_t)datetime->hour * 3600ULL +
           (uint64_t)datetime->minute * 60ULL + datetime->second;
}

/** @brief 回读允许因任务抢占自然前进最多两秒，但不允许时间倒退。 */
static bool rtc_datetime_matches_write(const gw_rtc_ysn8010_datetime_t *expected,
                                       const gw_rtc_ysn8010_datetime_t *actual)
{
    uint64_t expected_seconds = rtc_datetime_to_serial_seconds(expected);
    uint64_t actual_seconds = rtc_datetime_to_serial_seconds(actual);
    return actual_seconds >= expected_seconds && actual_seconds - expected_seconds <= 2ULL;
}

esp_err_t gw_rtc_ysn8010_init(void)
{
    if (!rtc_lock()) {
        return ESP_ERR_NO_MEM;
    }

    if (s_rtc.initialized) {
        esp_err_t probe_err = rtc_probe_locked();
        rtc_unlock();
        return probe_err;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = GW_RTC_YSN8010_SDA_GPIO,
        .scl_io_num = GW_RTC_YSN8010_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7U,
        .intr_priority = 0,
        .trans_queue_depth = 0U,
        .flags.enable_internal_pullup = false,
        .flags.allow_pd = false,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_rtc.bus_handle);
    if (err != ESP_OK) {
        s_rtc.last_error = err;
        rtc_unlock();
        return err;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GW_RTC_YSN8010_I2C_ADDRESS,
        .scl_speed_hz = GW_RTC_YSN8010_I2C_SPEED_HZ,
        .scl_wait_us = 0U,
        .flags.disable_ack_check = false,
    };
    err = i2c_master_bus_add_device(s_rtc.bus_handle,
                                    &device_config,
                                    &s_rtc.device_handle);
    if (err != ESP_OK) {
        (void)i2c_del_master_bus(s_rtc.bus_handle);
        s_rtc.bus_handle = NULL;
        s_rtc.last_error = err;
        rtc_unlock();
        return err;
    }

    s_rtc.initialized = true;
    s_rtc.present = false;
    s_rtc.datetime_valid = false;
    s_rtc.voltage_low = false;
    s_rtc.stopped = false;
    s_rtc.flag_register = 0U;
    s_rtc.control_register = 0U;
    memset(&s_rtc.last_datetime, 0, sizeof(s_rtc.last_datetime));

    /* 满足 RX8010SJ 上电后内部振荡稳定时间；已由电池供电时只是保守等待。 */
    /* 多等一个 tick，避免在临近 tick 边界调用时实际等待不足手册要求的 40 ms。 */
    vTaskDelay(pdMS_TO_TICKS(GW_RTC_POWER_STABILIZE_MS) + 1U);
    err = rtc_probe_locked();
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "external RTC detected: address=0x%02X scl=%d sda=%d",
                 GW_RTC_YSN8010_I2C_ADDRESS,
                 GW_RTC_YSN8010_SCL_GPIO,
                 GW_RTC_YSN8010_SDA_GPIO);
    } else {
        ESP_LOGW(TAG, "external RTC probe failed: %s", esp_err_to_name(err));
    }

    rtc_unlock();
    return err;
}

esp_err_t gw_rtc_ysn8010_probe(bool *out_present)
{
    if (out_present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_present = false;
    if (!rtc_lock()) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = rtc_probe_locked();
    *out_present = err == ESP_OK;
    rtc_unlock();
    return err;
}

esp_err_t gw_rtc_ysn8010_read_datetime(gw_rtc_ysn8010_datetime_t *out_datetime)
{
    if (out_datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_datetime, 0, sizeof(*out_datetime));
    if (!rtc_lock()) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = rtc_read_datetime_locked(out_datetime);
    rtc_unlock();
    return err;
}

esp_err_t gw_rtc_ysn8010_write_datetime(const gw_rtc_ysn8010_datetime_t *datetime)
{
    if (!rtc_datetime_is_valid(datetime)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rtc_lock()) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_rtc.initialized || s_rtc.device_handle == NULL) {
        s_rtc.last_error = ESP_ERR_INVALID_STATE;
        rtc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t state_registers[2] = {0};
    esp_err_t err = rtc_read_registers_locked(GW_RTC_REG_FLAG,
                                              state_registers,
                                              sizeof(state_registers));
    if (err != ESP_OK) {
        rtc_unlock();
        return err;
    }
    bool requires_full_initialization = (state_registers[0] & GW_RTC_FLAG_VLF) != 0U;
    s_rtc.flag_register = state_registers[0];
    s_rtc.control_register = state_registers[1];
    s_rtc.voltage_low = requires_full_initialization;
    s_rtc.stopped = (state_registers[1] & GW_RTC_CONTROL_STOP) != 0U;

    /*
     * 0x10..0x1D 使用纯时钟模式：0x17 写厂商规定的 D8，三个闹钟字段
     * 设 AE=1，扩展寄存器使用 RX8010SJ 纯时钟初始化示例的 0x04。
     */
    uint8_t registers[GW_RTC_CLOCK_MODE_BLOCK_COUNT] = {
        rtc_binary_to_bcd(datetime->second),
        rtc_binary_to_bcd(datetime->minute),
        rtc_binary_to_bcd(datetime->hour),
        (uint8_t)(1U << datetime->weekday),
        rtc_binary_to_bcd(datetime->day),
        rtc_binary_to_bcd(datetime->month),
        rtc_binary_to_bcd((uint8_t)(datetime->year - 2000U)),
        GW_RTC_RESERVED_17_VALUE,
        GW_RTC_ALARM_DISABLED_VALUE,
        GW_RTC_ALARM_DISABLED_VALUE,
        GW_RTC_ALARM_DISABLED_VALUE,
        0x00U,
        0x00U,
        GW_RTC_CLOCK_ONLY_EXTENSION,
    };

    /* 先停表，再连续写日历和纯时钟模式配置，避免日期跨秒撕裂。 */
    bool stop_asserted = false;
    err = rtc_write_register_locked(GW_RTC_REG_CONTROL, GW_RTC_CONTROL_STOP);
    if (err == ESP_OK) {
        stop_asserted = true;
        s_rtc.control_register = GW_RTC_CONTROL_STOP;
        s_rtc.stopped = true;
        err = rtc_write_registers_locked(GW_RTC_REG_SECONDS,
                                         registers,
                                         sizeof(registers));
    }

    /*
     * VLF=1 代表其他寄存器内容也不可信。只有 0x30..0x32 全部初始化成功，
     * 才允许清除 VLF；否则保留失效标志，让下次启动仍能识别未完成初始化。
     */
    if (err == ESP_OK && requires_full_initialization) {
        err = rtc_write_register_locked(GW_RTC_REG_DIGITAL_OFFSET, 0x00U);
    }
    if (err == ESP_OK && requires_full_initialization) {
        err = rtc_write_register_locked(GW_RTC_REG_RESERVED_31, 0x08U);
    }
    if (err == ESP_OK && requires_full_initialization) {
        err = rtc_write_register_locked(GW_RTC_REG_IRQ_CONTROL, 0x00U);
    }
    if (err == ESP_OK) {
        err = rtc_write_register_locked(GW_RTC_REG_FLAG, 0x00U);
        if (err == ESP_OK) {
            s_rtc.flag_register = 0U;
            s_rtc.voltage_low = false;
        }
    }
    if (err == ESP_OK) {
        err = rtc_write_register_locked(GW_RTC_REG_CONTROL, 0x00U);
        if (err == ESP_OK) {
            s_rtc.control_register = 0U;
            s_rtc.stopped = false;
        }
    }
    if (err != ESP_OK) {
        /*
         * 已经停表后若只写入了一部分日历，必须保留 STOP=1 作为持久无效标志。
        * 否则下次启动可能把“新旧字段混合但碰巧合法”的日期误判为可信。
         */
        if (stop_asserted) {
            esp_err_t invalidate_err = rtc_write_register_locked(GW_RTC_REG_CONTROL,
                                                                  GW_RTC_CONTROL_STOP);
            if (invalidate_err == ESP_OK) {
                s_rtc.stopped = true;
                s_rtc.control_register = GW_RTC_CONTROL_STOP;
                ESP_LOGE(TAG, "RTC write failed; STOP kept set to invalidate partial calendar");
            } else {
                ESP_LOGE(TAG, "RTC write failed and STOP reassert failed: %s",
                         esp_err_to_name(invalidate_err));
            }
        }
        s_rtc.datetime_valid = false;
        s_rtc.last_error = err;
        rtc_unlock();
        return err;
    }

    gw_rtc_ysn8010_datetime_t verified = {0};
    for (uint32_t attempt = 0; attempt < GW_RTC_WRITE_VERIFY_ATTEMPTS; ++attempt) {
        err = rtc_read_datetime_locked(&verified);
        if (err == ESP_OK && rtc_datetime_matches_write(datetime, &verified)) {
            break;
        }
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        if (attempt + 1U < GW_RTC_WRITE_VERIFY_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(GW_RTC_WRITE_VERIFY_RETRY_MS) + 1U);
        }
    }
    if (err != ESP_OK) {
        /* 清 STOP 后的回读失败同样不能留下“看似有效”的运行中日历。 */
        esp_err_t invalidate_err = rtc_write_register_locked(GW_RTC_REG_CONTROL,
                                                              GW_RTC_CONTROL_STOP);
        if (invalidate_err == ESP_OK) {
            s_rtc.control_register = GW_RTC_CONTROL_STOP;
            s_rtc.stopped = true;
        } else {
            ESP_LOGE(TAG, "RTC verify failed and STOP reassert failed: %s",
                     esp_err_to_name(invalidate_err));
        }
        s_rtc.datetime_valid = false;
        s_rtc.last_error = err;
    }

    rtc_unlock();
    return err;
}

esp_err_t gw_rtc_ysn8010_get_status(gw_rtc_ysn8010_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rtc_lock()) {
        return ESP_ERR_NO_MEM;
    }

    *out_status = (gw_rtc_ysn8010_status_t) {
        .initialized = s_rtc.initialized,
        .present = s_rtc.present,
        .datetime_valid = s_rtc.datetime_valid,
        .voltage_low = s_rtc.voltage_low,
        .stopped = s_rtc.stopped,
        .flag_register = s_rtc.flag_register,
        .control_register = s_rtc.control_register,
        .last_datetime = s_rtc.last_datetime,
        .last_error = s_rtc.last_error,
    };

    rtc_unlock();
    return ESP_OK;
}
