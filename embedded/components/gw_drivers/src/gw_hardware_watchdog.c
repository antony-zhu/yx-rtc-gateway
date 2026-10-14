/**
 * @file gw_hardware_watchdog.c
 * @brief 外部硬件看门狗 GPIO 喂狗任务实现。
 */

#include "gw_hardware_watchdog.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_hw_config.h"

static const char *TAG = "gw_hdwg";

typedef struct {
    bool enabled;
    bool running;
    int gpio;
    uint32_t pulse_high_ms;
    uint32_t feed_interval_ms;
    TaskHandle_t task;
} gw_hardware_watchdog_state_t;

static gw_hardware_watchdog_state_t s_watchdog = {
    .enabled = GW_HARDWARE_WATCHDOG_ENABLED != 0,
    .running = false,
    .gpio = GW_HARDWARE_WATCHDOG_GPIO,
    .pulse_high_ms = GW_HARDWARE_WATCHDOG_PULSE_HIGH_MS,
    .feed_interval_ms = GW_HARDWARE_WATCHDOG_FEED_INTERVAL_MS,
    .task = NULL,
};

/**
 * @brief 校验外部看门狗编译期参数。
 *
 * 外部看门狗一旦启动就会参与系统自恢复，因此宁可启动失败，也不能在非法 GPIO 或
 * 非法时序下输出不可预期的脉冲。
 */
static esp_err_t validate_watchdog_config(const gw_hardware_watchdog_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!state->enabled) {
        return ESP_OK;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)state->gpio)) {
        ESP_LOGE(TAG, "hardware watchdog gpio invalid: %d", state->gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (state->pulse_high_ms == 0U ||
        state->feed_interval_ms == 0U ||
        state->pulse_high_ms >= state->feed_interval_ms) {
        ESP_LOGE(TAG,
                 "hardware watchdog timing invalid: pulse=%lu interval=%lu",
                 (unsigned long)state->pulse_high_ms,
                 (unsigned long)state->feed_interval_ms);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/** @brief 配置喂狗 GPIO，并把空闲电平固定为低电平。 */
static esp_err_t configure_watchdog_gpio(const gw_hardware_watchdog_state_t *state)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << (uint32_t)state->gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hardware watchdog gpio config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level((gpio_num_t)state->gpio, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hardware watchdog idle level failed: %s", esp_err_to_name(err));
    }
    return err;
}

/** @brief 周期性输出高电平脉冲的喂狗任务。 */
static void hardware_watchdog_task(void *arg)
{
    const gw_hardware_watchdog_state_t *state = (const gw_hardware_watchdog_state_t *)arg;

    for (;;) {
        (void)gpio_set_level((gpio_num_t)state->gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(state->pulse_high_ms));
        (void)gpio_set_level((gpio_num_t)state->gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(state->feed_interval_ms));
    }
}

esp_err_t gw_hardware_watchdog_start(void)
{
    if (s_watchdog.task != NULL) {
        return ESP_OK;
    }

    esp_err_t err = validate_watchdog_config(&s_watchdog);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_watchdog.enabled) {
        ESP_LOGI(TAG, "hardware watchdog disabled");
        return ESP_OK;
    }

    err = configure_watchdog_gpio(&s_watchdog);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(hardware_watchdog_task,
                                "gw_hdwg",
                                GW_HARDWARE_WATCHDOG_TASK_STACK,
                                &s_watchdog,
                                GW_HARDWARE_WATCHDOG_TASK_PRIORITY,
                                &s_watchdog.task);
    if (ok != pdPASS) {
        s_watchdog.task = NULL;
        ESP_LOGE(TAG, "create hardware watchdog task failed");
        return ESP_ERR_NO_MEM;
    }

    s_watchdog.running = true;
    ESP_LOGI(TAG,
             "hardware watchdog started: gpio=%d pulse=%lums interval=%lums",
             s_watchdog.gpio,
             (unsigned long)s_watchdog.pulse_high_ms,
             (unsigned long)s_watchdog.feed_interval_ms);
    return ESP_OK;
}

esp_err_t gw_hardware_watchdog_get_info(gw_hardware_watchdog_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->enabled = s_watchdog.enabled;
    out->running = s_watchdog.running;
    out->gpio = s_watchdog.gpio;
    out->pulse_high_ms = s_watchdog.pulse_high_ms;
    out->feed_interval_ms = s_watchdog.feed_interval_ms;
    return ESP_OK;
}
