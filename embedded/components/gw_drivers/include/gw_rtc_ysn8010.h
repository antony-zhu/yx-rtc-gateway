#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_rtc_ysn8010.h
 * @brief YSN8010 外部电池备份实时时钟接口。
 *
 * 当前驱动按 YSN8010 与 Epson RX8010SJ 的时钟寄存器兼容关系实现，固定使用
 * 7 位 I2C 地址 0x32。正常读写以 0x10 至 0x1F 为边界；仅当 VLF=1、必须完成
 * 掉电恢复初始化时，额外写入 0x30 至 0x32 的规定初值。硬件连接来自网关
 * 1.2.3 原理图：SCL 为 GPIO9，SDA 为 GPIO10。
 */

#define GW_RTC_YSN8010_I2C_ADDRESS       0x32U       /*!< 固定 7 位 I2C 从地址 */
#define GW_RTC_YSN8010_SCL_GPIO          9           /*!< 原理图 SCL 对应的 ESP32 GPIO */
#define GW_RTC_YSN8010_SDA_GPIO          10          /*!< 原理图 SDA 对应的 ESP32 GPIO */
#define GW_RTC_YSN8010_I2C_SPEED_HZ      100000U     /*!< 保守使用 100 kHz 标准模式 */

/**
 * @brief RTC 日历时间。
 *
 * 年份范围为 2000 至 2099。weekday 使用与 C `struct tm` 相同的编号：
 * 0 表示星期日，1 表示星期一，依此类推，6 表示星期六。
 */
typedef struct {
    uint16_t year;        /*!< 完整公历年份，范围 2000..2099 */
    uint8_t month;        /*!< 月，范围 1..12 */
    uint8_t day;          /*!< 日，范围取决于月份和闰年 */
    uint8_t weekday;      /*!< 星期，0=星期日，6=星期六 */
    uint8_t hour;         /*!< 小时，24 小时制，范围 0..23 */
    uint8_t minute;       /*!< 分钟，范围 0..59 */
    uint8_t second;       /*!< 秒，范围 0..59 */
} gw_rtc_ysn8010_datetime_t;

/**
 * @brief 外部 RTC 驱动状态。
 *
 * 该结构是最近一次操作后的缓存快照，不会在获取状态时再次访问 I2C。只有
 * `present`、`datetime_valid` 均为 true，且 `voltage_low`、`stopped` 均为
 * false 时，`last_datetime` 才能作为可信候选时间交给上层时间模块。
 */
typedef struct {
    bool initialized;                             /*!< I2C 总线和设备句柄是否已创建 */
    bool present;                                 /*!< 最近一次访问时 0x32 是否有响应 */
    bool datetime_valid;                          /*!< 最近读取的 BCD 日历是否完整有效 */
    bool voltage_low;                             /*!< Flag 寄存器 VLF 位是否为 1 */
    bool stopped;                                 /*!< Control 寄存器 STOP 位是否为 1 */
    uint8_t flag_register;                        /*!< 最近读取的 0x1E 原始值 */
    uint8_t control_register;                     /*!< 最近读取的 0x1F 原始值，测试位未解释 */
    gw_rtc_ysn8010_datetime_t last_datetime;      /*!< 最近一次通过校验的日历时间 */
    esp_err_t last_error;                         /*!< 最近一次初始化、I2C 或数据校验错误 */
} gw_rtc_ysn8010_status_t;

/**
 * @brief 初始化外部 RTC 驱动并探测芯片。
 *
 * 使用 ESP-IDF 5.5 新版 I2C master API 创建独占总线，固定配置 GPIO9/GPIO10、
 * 100 kHz 和 7 位地址 0x32。板上已有 4.7 kOhm 外部上拉，因此不会启用 ESP32
 * 内部弱上拉。初始化后会等待至少 40 ms，再执行地址探测。
 *
 * 本接口可重复调用；驱动已初始化时只重新探测芯片。该接口绝不会自动清 VLF
 * 或写入 RTC 寄存器。不得在中断上下文调用。
 *
 * @return ESP_OK 表示驱动已初始化且芯片应答；无应答或总线异常时返回底层
 *         ESP-IDF I2C 错误码。即使首次探测失败，总线仍保留，可稍后再次探测。
 */
esp_err_t gw_rtc_ysn8010_init(void);

/**
 * @brief 探测固定 I2C 地址 0x32。
 *
 * @param out_present 输出是否收到 ACK，不能为空。发生超时或 NACK 时该值为 false。
 * @return ESP_OK 表示收到 ACK；否则返回底层 I2C 错误码。驱动尚未初始化时返回
 *         ESP_ERR_INVALID_STATE。
 */
esp_err_t gw_rtc_ysn8010_probe(bool *out_present);

/**
 * @brief 读取并校验 RTC 日期时间。
 *
 * 驱动连续读取 0x10 至 0x1F，以获得一致的日历快照并检查 VLF 与 STOP。
 * BCD、日期、星期任一项非法时拒绝该时间；VLF=1 表示芯片曾掉到保持电压以下，
 * STOP=1 表示计时已经停止，这两种状态下寄存器时间均不得交给上层使用。
 *
 * @param out_datetime 输出日历时间，不能为空；失败时内容清零。
 * @return ESP_OK 表示日历有效、VLF=0 且 STOP=0；VLF=1 或 STOP=1 返回
 *         ESP_ERR_INVALID_STATE；BCD、日期或星期非法返回
 *         ESP_ERR_INVALID_RESPONSE；I2C 失败时返回底层错误码。不得在中断上下文调用。
 */
esp_err_t gw_rtc_ysn8010_read_datetime(gw_rtc_ysn8010_datetime_t *out_datetime);

/**
 * @brief 写入 RTC 日期时间并启动计时。
 *
 * 输入会先进行完整公历校验，weekday 必须与 year/month/day 实际星期一致。
 * 写入过程先设置 STOP，再按 RX8010SJ 的纯时钟模式初始化 0x10 至 0x1D：
 * 写入 BCD 日历、固定保留寄存器 0x17=0xD8，并关闭芯片内部闹钟/计时器中断。
 * 若写入前检测到 VLF=1，还会依次写 0x30=0x00、0x31=0x08、0x32=0x00；
 * 这些写入全部成功后才清 VLF，最后清除 STOP 启动计时。成功后回读验证。
 *
 * @note 该操作会重置 RTC 芯片自身的闹钟、倒计时和中断配置；网关定时任务由
 *       MCU 调度器执行，不依赖这些 RTC 内部功能。
 * @param datetime 待写入的完整日历时间，不能为空。
 * @return ESP_OK 表示写入及回读校验成功；参数非法返回 ESP_ERR_INVALID_ARG；
 *         回读不一致返回 ESP_ERR_INVALID_RESPONSE；其他情况返回底层 I2C 错误码。
 *         不得在中断上下文调用。
 */
esp_err_t gw_rtc_ysn8010_write_datetime(const gw_rtc_ysn8010_datetime_t *datetime);

/**
 * @brief 获取驱动最近一次操作后的状态快照。
 *
 * 本接口不产生 I2C 访问，可用于日志和健康检查。若要刷新硬件状态，应先调用
 * `gw_rtc_ysn8010_probe()` 或 `gw_rtc_ysn8010_read_datetime()`。
 *
 * @param out_status 输出状态，不能为空。
 * @return ESP_OK 表示成功；参数为空返回 ESP_ERR_INVALID_ARG；互斥锁创建失败返回
 *         ESP_ERR_NO_MEM。
 */
esp_err_t gw_rtc_ysn8010_get_status(gw_rtc_ysn8010_status_t *out_status);

#ifdef __cplusplus
}
#endif
