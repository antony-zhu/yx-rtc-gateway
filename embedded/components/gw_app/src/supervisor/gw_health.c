#include "gw_health.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "gw_time.h"

static portMUX_TYPE s_health_lock = portMUX_INITIALIZER_UNLOCKED;
static gw_health_heartbeat_t s_last_heartbeat;

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }

    if (src == NULL) {
        src = "";
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

esp_err_t gw_health_init(void)
{
    portENTER_CRITICAL(&s_health_lock);
    memset(&s_last_heartbeat, 0, sizeof(s_last_heartbeat));
    portEXIT_CRITICAL(&s_health_lock);

    return ESP_OK;
}

esp_err_t gw_health_report_heartbeat(const char *object, const char *stage)
{
    if (object == NULL || object[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    gw_time_point_t now = gw_time_now();

    portENTER_CRITICAL(&s_health_lock);
    copy_text(s_last_heartbeat.object, sizeof(s_last_heartbeat.object), object);
    copy_text(s_last_heartbeat.stage, sizeof(s_last_heartbeat.stage), stage);
    s_last_heartbeat.uptime_ms = now.uptime_ms;
    ++s_last_heartbeat.count;
    portEXIT_CRITICAL(&s_health_lock);

    return ESP_OK;
}

esp_err_t gw_health_get_last_heartbeat(gw_health_heartbeat_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_health_lock);
    *out = s_last_heartbeat;
    portEXIT_CRITICAL(&s_health_lock);

    return ESP_OK;
}
