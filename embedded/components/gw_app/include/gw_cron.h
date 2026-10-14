#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_CRON_EXPRESSION_MAX_BYTES 64U

typedef struct {
    uint64_t minute_mask;
    uint32_t hour_mask;
    uint32_t day_of_month_mask;
    uint16_t month_mask;
    uint8_t day_of_week_mask;
    bool day_of_month_any;
    bool day_of_week_any;
} gw_cron_schedule_t;

/** 解析标准五段 Cron：分、时、日、月、星期。 */
esp_err_t gw_cron_parse(const char *expression, gw_cron_schedule_t *out);

/**
 * 判断指定 UTC 分钟是否匹配。时区使用固定分钟偏移，不处理夏令时。
 * out_local_minute 返回本地时间轴上的分钟编号，可用于同一分钟去重。
 */
bool gw_cron_matches(const gw_cron_schedule_t *schedule,
                     uint64_t utc_ms,
                     int16_t timezone_offset_min,
                     int64_t *out_local_minute);

#ifdef __cplusplus
}
#endif
