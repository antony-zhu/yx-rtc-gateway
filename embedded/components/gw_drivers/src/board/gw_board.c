/**
 * @file gw_board.c
 * @brief 板级 LED 和 RS485 固定外设初始化与访问实现。
 */

#include "gw_board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "gw_board";

#define GW_BOARD_RS485_UART_NUM UART_NUM_1

typedef struct {
    bool initialized;
    bool led_ready;
    bool rs485_ready;
    bool led_state[GW_BOARD_LED_COUNT];
    bool identify_active;
    bool identify_restore_on;
    int64_t identify_deadline_us;
    esp_timer_handle_t identify_timer;
    uint32_t rs485_baudrate;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buffer;
} gw_board_state_t;

static const int s_led_gpio[GW_BOARD_LED_COUNT] = {
    GW_BOARD_LED0_GPIO,
    GW_BOARD_LED1_GPIO,
    GW_BOARD_LED2_GPIO,
};

_Static_assert(GW_BOARD_IDENTIFY_LED_INDEX < GW_BOARD_LED_COUNT,
               "identify LED index must reference a configured LED");
_Static_assert(GW_BOARD_MQTT_LED_INDEX < GW_BOARD_LED_COUNT,
               "MQTT LED index must reference a configured LED");
_Static_assert(GW_BOARD_TASK_LED_INDEX < GW_BOARD_LED_COUNT,
               "task LED index must reference a configured LED");

static gw_board_state_t s_board = {
    .rs485_baudrate = GW_BOARD_RS485_BAUDRATE,
};

static void board_identify_timer_cb(void *arg);

/** @brief 将 LED 逻辑状态转换为原理图对应的低电平点亮 GPIO 电平。 */
static uint32_t board_led_output_level(bool on)
{
    return on ? GW_BOARD_LED_ACTIVE_LEVEL : GW_BOARD_LED_INACTIVE_LEVEL;
}

/** @brief 获取板级外设互斥锁，首次使用时静态创建。 */
static bool board_lock(void)
{
    if (s_board.lock == NULL) {
        s_board.lock = xSemaphoreCreateMutexStatic(&s_board.lock_buffer);
    }
    return s_board.lock != NULL && xSemaphoreTake(s_board.lock, portMAX_DELAY) == pdTRUE;
}

/** @brief 释放板级外设互斥锁。 */
static void board_unlock(void)
{
    if (s_board.lock != NULL) {
        (void)xSemaphoreGive(s_board.lock);
    }
}

/** @brief 在持锁状态下恢复识别灯并清理闪灯状态。 */
static esp_err_t board_identify_restore_locked(void)
{
    size_t index = GW_BOARD_IDENTIFY_LED_INDEX;
    esp_err_t err = gpio_set_level((gpio_num_t)s_led_gpio[index],
                                   board_led_output_level(s_board.identify_restore_on));
    if (err == ESP_OK) {
        s_board.led_state[index] = s_board.identify_restore_on;
    }
    s_board.identify_active = false;
    s_board.identify_deadline_us = 0;
    return err;
}

/** @brief esp_timer 任务上下文中的识别灯恢复回调。 */
static void board_identify_timer_cb(void *arg)
{
    (void)arg;
    if (!board_lock()) {
        return;
    }
    if (!s_board.identify_active) {
        board_unlock();
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us < s_board.identify_deadline_us) {
        /* 重复点击会延长截止时间；已激活的新定时器无需再次启动。 */
        if (!esp_timer_is_active(s_board.identify_timer)) {
            uint64_t remain_us = (uint64_t)(s_board.identify_deadline_us - now_us);
            if (esp_timer_start_once(s_board.identify_timer, remain_us) != ESP_OK) {
                (void)board_identify_restore_locked();
            }
        }
        board_unlock();
        return;
    }

    (void)board_identify_restore_locked();
    board_unlock();
}

/** @brief 初始化三个板载 LED GPIO，默认全部熄灭。 */
static esp_err_t board_led_init(void)
{
    uint64_t mask = 0ULL;

    for (size_t i = 0U; i < GW_BOARD_LED_COUNT; ++i) {
        mask |= (1ULL << (uint32_t)s_led_gpio[i]);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led gpio config failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0U; i < GW_BOARD_LED_COUNT; ++i) {
        (void)gpio_set_level((gpio_num_t)s_led_gpio[i],
                             board_led_output_level(false));
        s_board.led_state[i] = false;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = board_identify_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "identify_led",
    };
    err = esp_timer_create(&timer_args, &s_board.identify_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "identify timer create failed: %s", esp_err_to_name(err));
        return err;
    }
    s_board.led_ready = true;
    return ESP_OK;
}

/** @brief 初始化 RS485 UART；当前硬件没有单独 DE/RE 控制脚。 */
static esp_err_t board_rs485_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = GW_BOARD_RS485_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(GW_BOARD_RS485_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rs485 uart config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(GW_BOARD_RS485_UART_NUM,
                       GW_BOARD_RS485_TX_GPIO,
                       GW_BOARD_RS485_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rs485 uart set pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(GW_BOARD_RS485_UART_NUM,
                              GW_BOARD_RS485_RX_BUF_BYTES,
                              GW_BOARD_RS485_TX_BUF_BYTES,
                              0,
                              NULL,
                              0);
    if (err != ESP_OK && err != ESP_FAIL) {
        ESP_LOGE(TAG, "rs485 uart driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    (void)uart_flush(GW_BOARD_RS485_UART_NUM);
    s_board.rs485_baudrate = GW_BOARD_RS485_BAUDRATE;
    s_board.rs485_ready = true;
    return ESP_OK;
}

esp_err_t gw_board_init(void)
{
    if (!board_lock()) {
        return ESP_FAIL;
    }
    if (s_board.initialized) {
        board_unlock();
        return ESP_OK;
    }

    esp_err_t err = board_led_init();
    if (err == ESP_OK) {
        err = board_rs485_init();
    }
    if (err == ESP_OK) {
        s_board.initialized = true;
        ESP_LOGI(TAG,
                 "board ready: rs485 uart=%d tx=%d rx=%d baud=%lu led=%d,%d,%d",
                 GW_BOARD_RS485_UART,
                 GW_BOARD_RS485_TX_GPIO,
                 GW_BOARD_RS485_RX_GPIO,
                 (unsigned long)s_board.rs485_baudrate,
                 s_led_gpio[0],
                 s_led_gpio[1],
                 s_led_gpio[2]);
    }

    board_unlock();
    return err;
}

esp_err_t gw_board_get_info(gw_board_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!board_lock()) {
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    out->led_ready = s_board.led_ready;
    out->rs485_ready = s_board.rs485_ready;
    memcpy(out->led_gpio, s_led_gpio, sizeof(out->led_gpio));
    out->rs485_uart = GW_BOARD_RS485_UART;
    out->rs485_tx_gpio = GW_BOARD_RS485_TX_GPIO;
    out->rs485_rx_gpio = GW_BOARD_RS485_RX_GPIO;
    out->rs485_baudrate = s_board.rs485_baudrate;

    board_unlock();
    return ESP_OK;
}

esp_err_t gw_board_led_set(size_t index, bool on)
{
    if (index >= GW_BOARD_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!board_lock()) {
        return ESP_FAIL;
    }
    if (!s_board.led_ready) {
        board_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    if (index == GW_BOARD_IDENTIFY_LED_INDEX && s_board.identify_active) {
        /* 显式板级控制优先于识别动画，避免定时回调覆盖调用者的新状态。 */
        (void)esp_timer_stop(s_board.identify_timer);
        s_board.identify_active = false;
        s_board.identify_deadline_us = 0;
    }

    esp_err_t err = gpio_set_level((gpio_num_t)s_led_gpio[index],
                                   board_led_output_level(on));
    if (err == ESP_OK) {
        s_board.led_state[index] = on;
    }
    board_unlock();
    return err;
}

esp_err_t gw_board_led_get(size_t index, bool *out_on)
{
    if (index >= GW_BOARD_LED_COUNT || out_on == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!board_lock()) {
        return ESP_FAIL;
    }
    *out_on = s_board.led_state[index];
    board_unlock();
    return ESP_OK;
}

esp_err_t gw_board_led_toggle(size_t index)
{
    bool current = false;
    esp_err_t err = gw_board_led_get(index, &current);
    if (err != ESP_OK) {
        return err;
    }
    return gw_board_led_set(index, !current);
}

esp_err_t gw_board_mqtt_led_set(bool online)
{
    esp_err_t err = gw_board_led_set(GW_BOARD_MQTT_LED_INDEX, online);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT LED: gpio=%d online=%s", GW_BOARD_LED0_GPIO, online ? "true" : "false");
    }
    return err;
}

esp_err_t gw_board_task_led_set(bool on)
{
    esp_err_t err = gw_board_led_set(GW_BOARD_TASK_LED_INDEX, on);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "task LED: gpio=%d on=%s", GW_BOARD_LED1_GPIO, on ? "true" : "false");
    }
    return err;
}

esp_err_t gw_board_identify_blink(void)
{
    if (GW_BOARD_IDENTIFY_LED_INDEX >= GW_BOARD_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!board_lock()) {
        return ESP_FAIL;
    }
    if (!s_board.led_ready || s_board.identify_timer == NULL) {
        board_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    size_t index = GW_BOARD_IDENTIFY_LED_INDEX;
    if (!s_board.identify_active) {
        s_board.identify_restore_on = s_board.led_state[index];
    } else {
        esp_err_t stop_err = esp_timer_stop(s_board.identify_timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            board_unlock();
            return stop_err;
        }
    }

    bool identify_level = !s_board.identify_restore_on;
    esp_err_t err = gpio_set_level((gpio_num_t)s_led_gpio[index],
                                   board_led_output_level(identify_level));
    if (err == ESP_OK) {
        s_board.led_state[index] = identify_level;
        s_board.identify_active = true;
        s_board.identify_deadline_us = esp_timer_get_time() +
            ((int64_t)GW_BOARD_IDENTIFY_BLINK_MS * 1000LL);
        err = esp_timer_start_once(s_board.identify_timer,
                                   (uint64_t)GW_BOARD_IDENTIFY_BLINK_MS * 1000ULL);
    }
    if (err != ESP_OK) {
        (void)board_identify_restore_locked();
    }
    board_unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "identify blink: led=%u gpio=%d duration=%ums",
                 (unsigned int)index,
                 s_led_gpio[index],
                 (unsigned int)GW_BOARD_IDENTIFY_BLINK_MS);
    }
    return err;
}

esp_err_t gw_board_rs485_set_baudrate(uint32_t baudrate)
{
    if (baudrate == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!board_lock()) {
        return ESP_FAIL;
    }
    if (!s_board.rs485_ready) {
        board_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = uart_set_baudrate(GW_BOARD_RS485_UART_NUM, baudrate);
    if (err == ESP_OK) {
        s_board.rs485_baudrate = baudrate;
    }
    board_unlock();
    return err;
}

uint32_t gw_board_rs485_get_baudrate(void)
{
    uint32_t baudrate = GW_BOARD_RS485_BAUDRATE;
    if (!board_lock()) {
        return baudrate;
    }
    baudrate = s_board.rs485_baudrate;
    board_unlock();
    return baudrate;
}

esp_err_t gw_board_rs485_write(const uint8_t *data,
                               size_t len,
                               uint32_t timeout_ms,
                               size_t *out_written)
{
    if (out_written != NULL) {
        *out_written = 0U;
    }
    if (data == NULL && len > 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_board.rs485_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0U) {
        return ESP_OK;
    }

    int written = uart_write_bytes(GW_BOARD_RS485_UART_NUM, data, len);
    if (written < 0) {
        return ESP_FAIL;
    }

    esp_err_t err = uart_wait_tx_done(GW_BOARD_RS485_UART_NUM,
                                      pdMS_TO_TICKS(timeout_ms > 0U ?
                                                      timeout_ms :
                                                      GW_BOARD_RS485_WAIT_TX_MS));
    if (out_written != NULL) {
        *out_written = (size_t)written;
    }
    return err;
}

esp_err_t gw_board_rs485_read(uint8_t *data,
                              size_t max_len,
                              uint32_t timeout_ms,
                              size_t *out_len)
{
    if (out_len == NULL || (data == NULL && max_len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    if (!s_board.rs485_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (max_len == 0U) {
        return ESP_OK;
    }

    int read_len = uart_read_bytes(GW_BOARD_RS485_UART_NUM,
                                   data,
                                   max_len,
                                   pdMS_TO_TICKS(timeout_ms));
    if (read_len < 0) {
        return ESP_FAIL;
    }
    *out_len = (size_t)read_len;
    return ESP_OK;
}

esp_err_t gw_board_rs485_flush(void)
{
    esp_err_t err = uart_wait_tx_done(GW_BOARD_RS485_UART_NUM,
                                      pdMS_TO_TICKS(GW_BOARD_RS485_WAIT_TX_MS));
    if (err != ESP_OK) {
        return err;
    }
    return uart_flush(GW_BOARD_RS485_UART_NUM);
}
