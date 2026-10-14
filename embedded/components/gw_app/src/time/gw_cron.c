#include "gw_cron.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"

#define GW_CRON_FIELD_COUNT 5U
#define GW_CRON_MS_PER_MINUTE 60000LL

typedef struct {
    int min;
    int max;
    bool sunday_alias;
} cron_field_rule_t;

static const cron_field_rule_t FIELD_RULES[GW_CRON_FIELD_COUNT] = {
    {0, 59, false},
    {0, 23, false},
    {1, 31, false},
    {1, 12, false},
    {0, 7, true},
};

static esp_err_t parse_uint(const char **cursor, int *out)
{
    if (cursor == NULL || *cursor == NULL || out == NULL ||
        !isdigit((unsigned char)**cursor)) {
        return ESP_ERR_INVALID_ARG;
    }

    int value = 0;
    do {
        int digit = **cursor - '0';
        if (value > (INT_MAX - digit) / 10) {
            return ESP_ERR_INVALID_SIZE;
        }
        value = value * 10 + digit;
        ++(*cursor);
    } while (isdigit((unsigned char)**cursor));

    *out = value;
    return ESP_OK;
}

static void set_value_bit(size_t field, int value, gw_cron_schedule_t *out)
{
    if (field == 4U && value == 7) {
        value = 0;
    }
    switch (field) {
    case 0:
        out->minute_mask |= 1ULL << value;
        break;
    case 1:
        out->hour_mask |= 1UL << value;
        break;
    case 2:
        out->day_of_month_mask |= 1UL << value;
        break;
    case 3:
        out->month_mask |= (uint16_t)(1U << value);
        break;
    case 4:
        out->day_of_week_mask |= (uint8_t)(1U << value);
        break;
    default:
        break;
    }
}

static esp_err_t parse_item(const char **cursor,
                            size_t field,
                            gw_cron_schedule_t *out)
{
    const cron_field_rule_t *rule = &FIELD_RULES[field];
    int first = rule->min;
    int last = rule->max;
    int step = 1;
    bool wildcard = false;

    if (**cursor == '*') {
        wildcard = true;
        ++(*cursor);
    } else {
        ESP_RETURN_ON_ERROR(parse_uint(cursor, &first), "gw_cron", "invalid value");
        last = first;
        if (**cursor == '-') {
            ++(*cursor);
            ESP_RETURN_ON_ERROR(parse_uint(cursor, &last), "gw_cron", "invalid range");
        }
    }

    if (**cursor == '/') {
        ++(*cursor);
        ESP_RETURN_ON_ERROR(parse_uint(cursor, &step), "gw_cron", "invalid step");
    }

    if (first < rule->min || first > rule->max ||
        last < rule->min || last > rule->max || first > last || step <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wildcard && first == last && step != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int value = first; value <= last; value += step) {
        set_value_bit(field, value, out);
        if (value > last - step) {
            break;
        }
    }
    return ESP_OK;
}

static esp_err_t parse_field(const char *text,
                             size_t length,
                             size_t field,
                             gw_cron_schedule_t *out)
{
    if (text == NULL || length == 0U || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char buffer[GW_CRON_EXPRESSION_MAX_BYTES + 1U];
    if (length >= sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';

    if (field == 2U) {
        out->day_of_month_any = strcmp(buffer, "*") == 0;
    } else if (field == 4U) {
        out->day_of_week_any = strcmp(buffer, "*") == 0;
    }

    const char *cursor = buffer;
    while (*cursor != '\0') {
        ESP_RETURN_ON_ERROR(parse_item(&cursor, field, out), "gw_cron", "invalid item");
        if (*cursor == '\0') {
            break;
        }
        if (*cursor != ',') {
            return ESP_ERR_INVALID_ARG;
        }
        ++cursor;
        if (*cursor == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t gw_cron_parse(const char *expression, gw_cron_schedule_t *out)
{
    if (expression == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = strnlen(expression, GW_CRON_EXPRESSION_MAX_BYTES + 1U);
    if (length == 0U || length > GW_CRON_EXPRESSION_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out, 0, sizeof(*out));
    const char *cursor = expression;
    for (size_t field = 0; field < GW_CRON_FIELD_COUNT; ++field) {
        while (*cursor == ' ') {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ') {
            ++cursor;
        }
        ESP_RETURN_ON_ERROR(parse_field(start, (size_t)(cursor - start), field, out),
                            "gw_cron",
                            "invalid field");
        if (field + 1U < GW_CRON_FIELD_COUNT && *cursor == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    return *cursor == '\0' ? ESP_OK : ESP_ERR_INVALID_ARG;
}

bool gw_cron_matches(const gw_cron_schedule_t *schedule,
                     uint64_t utc_ms,
                     int16_t timezone_offset_min,
                     int64_t *out_local_minute)
{
    if (schedule == NULL || utc_ms > (uint64_t)INT64_MAX) {
        return false;
    }

    int64_t utc_minutes = (int64_t)(utc_ms / (uint64_t)GW_CRON_MS_PER_MINUTE);
    if ((timezone_offset_min > 0 && utc_minutes > INT64_MAX - timezone_offset_min) ||
        (timezone_offset_min < 0 && utc_minutes < INT64_MIN - timezone_offset_min)) {
        return false;
    }
    int64_t local_minutes = utc_minutes + timezone_offset_min;
    if (local_minutes < 0 || local_minutes > (int64_t)INT64_MAX / 60) {
        return false;
    }

    time_t local_seconds = (time_t)(local_minutes * 60);
    struct tm calendar = {0};
    if (gmtime_r(&local_seconds, &calendar) == NULL) {
        return false;
    }

    bool minute_match = (schedule->minute_mask & (1ULL << calendar.tm_min)) != 0U;
    bool hour_match = (schedule->hour_mask & (1UL << calendar.tm_hour)) != 0U;
    bool month_match =
        (schedule->month_mask & (uint16_t)(1U << (calendar.tm_mon + 1))) != 0U;
    bool dom_match =
        (schedule->day_of_month_mask & (1UL << calendar.tm_mday)) != 0U;
    bool dow_match =
        (schedule->day_of_week_mask & (uint8_t)(1U << calendar.tm_wday)) != 0U;

    bool day_match;
    if (schedule->day_of_month_any && schedule->day_of_week_any) {
        day_match = true;
    } else if (schedule->day_of_month_any) {
        day_match = dow_match;
    } else if (schedule->day_of_week_any) {
        day_match = dom_match;
    } else {
        /* 与常见 Linux cron 一致：日和星期同时受限时采用 OR。 */
        day_match = dom_match || dow_match;
    }

    if (out_local_minute != NULL) {
        *out_local_minute = local_minutes;
    }
    return minute_match && hour_match && month_match && day_match;
}
