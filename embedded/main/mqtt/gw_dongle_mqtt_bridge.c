/**
 * @file gw_dongle_mqtt_bridge.c
 * @brief CH37X/YMZL 与业务 MQTT 的 C 层双向数据桥。
 */

#include "gw_dongle_mqtt_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "gw_business_mqtt.h"
#include "gw_ch37x_bridge.h"
#include "gw_gs200_protocol.h"
#include "gw_ymzl_protocol.h"

#define GW_DONGLE_UPLINK_QUEUE_CAPACITY      32U
#define GW_DONGLE_SCENE_QUEUE_CAPACITY        4U
#define GW_DONGLE_WORKER_STACK_BYTES         (4U * 1024U)
#define GW_DONGLE_WORKER_STACK_DEPTH         \
    (GW_DONGLE_WORKER_STACK_BYTES / sizeof(StackType_t))
#define GW_DONGLE_WORKER_PRIORITY            6U
#define GW_DONGLE_WORKER_POLL_MS             20U
#define GW_DONGLE_BATCH_SIZE                 8U
#define GW_DONGLE_STATUS_INTERVAL_MS          30000U
#define GW_DONGLE_SEND_INTERVAL_US           (30ULL * 1000ULL)
#define GW_DONGLE_SEND_TIMEOUT_MS            1000U
#define GW_DONGLE_DOWNLINK_RETRY_BASE_US      (200ULL * 1000ULL)
#define GW_DONGLE_DOWNLINK_RETRY_MAX_US       (2000ULL * 1000ULL)

static const char *TAG = "dongle_mqtt";

/** @brief CH37X host 回调投递给普通 worker 的完整帧。 */
typedef struct {
    uint8_t len;
    uint8_t data[GW_YMZL_MAX_FRAME_LEN];
} dongle_uplink_item_t;

/** @brief 已从 MQTT 领取、尚未成功写入 dongle 的下行帧。 */
typedef struct {
    bool active;
    uint8_t len;
    uint8_t attempts;
    uint64_t next_retry_us;
    uint8_t data[GW_YMZL_MAX_FRAME_LEN];
} dongle_downlink_pending_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_running;
static gw_dongle_mqtt_bridge_status_t s_status;
static gw_ymzl_parser_t s_parser;
static SemaphoreHandle_t s_parser_mutex;
static StaticSemaphore_t s_parser_mutex_buffer;

static QueueHandle_t s_uplink_queue;
static StaticQueue_t s_uplink_queue_buffer;
static uint8_t s_uplink_queue_storage[GW_DONGLE_UPLINK_QUEUE_CAPACITY *
                                      sizeof(dongle_uplink_item_t)];

/*
 * 场景读取与 MQTT 上行共享同一份已校验帧。此队列只保存结构化状态；满时
 * 丢弃最旧项并保留最新状态，避免历史广播阻塞正在等待的 BLE 测试事务。
 */
static QueueHandle_t s_scene_queue;
static StaticQueue_t s_scene_queue_buffer;
static uint8_t s_scene_queue_storage[GW_DONGLE_SCENE_QUEUE_CAPACITY *
                                     sizeof(gw_gs200_scene_status_t)];
static SemaphoreHandle_t s_scene_query_mutex;
static StaticSemaphore_t s_scene_query_mutex_buffer;

static SemaphoreHandle_t s_tx_mutex;
static StaticSemaphore_t s_tx_mutex_buffer;
static uint64_t s_last_send_us;
static dongle_downlink_pending_t s_downlink_pending;

static TaskHandle_t s_worker_task;
static StaticTask_t s_worker_task_buffer;
static StackType_t s_worker_stack[GW_DONGLE_WORKER_STACK_DEPTH];

/** @brief 将微秒等待时间向上取整为 tick，确保帧间隔不会因 tick 截断而缩短。 */
static TickType_t timeout_us_to_ticks_ceil(uint64_t timeout_us)
{
    if (timeout_us == 0U) {
        return 0;
    }

    uint64_t ticks = (timeout_us * (uint64_t)configTICK_RATE_HZ + 999999ULL) / 1000000ULL;
    if (ticks == 0U) {
        ticks = 1U;
    }
    if (ticks >= (uint64_t)portMAX_DELAY) {
        ticks = (uint64_t)portMAX_DELAY - 1ULL;
    }
    return (TickType_t)ticks;
}

/** @brief 在临界区内递增状态计数，避免 host、worker 和 supervisor 并发覆盖。 */
static void increment_counter(uint32_t *counter)
{
    portENTER_CRITICAL(&s_state_lock);
    ++(*counter);
    portEXIT_CRITICAL(&s_state_lock);
}

/** @brief USB 连接变化时只丢弃协议残包；已经切出的完整帧继续由 MQTT 队列持有。 */
static void dongle_on_reset(void *ctx)
{
    (void)ctx;
    if (s_parser_mutex == NULL ||
        xSemaphoreTake(s_parser_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    gw_ymzl_parser_reset(&s_parser);
    xSemaphoreGive(s_parser_mutex);
    increment_counter(&s_status.reset_count);
}

/**
 * @brief CH37X host 上下文中的收包入口。
 *
 * 这里只做固定内存解析和非阻塞队列投递，绝不执行 MQTT 或 dongle 写操作。
 */
static void dongle_on_data(uint8_t interface_num,
                           const uint8_t *data,
                           size_t data_len,
                           void *ctx)
{
    (void)interface_num;
    (void)ctx;

    /* reset 由独立 event dispatch task 回调，必须与 host 收包任务串行访问解析器。 */
    if (s_parser_mutex == NULL || xSemaphoreTake(s_parser_mutex, 0) != pdTRUE) {
        return;
    }
    if (gw_ymzl_parser_feed(&s_parser, data, data_len) != ESP_OK) {
        xSemaphoreGive(s_parser_mutex);
        return;
    }

    dongle_uplink_item_t item = {0};
    size_t frame_len = 0U;
    while (gw_ymzl_parser_next(&s_parser, item.data, &frame_len)) {
        item.len = (uint8_t)frame_len;
        gw_gs200_scene_status_t scene_status;
        if (s_scene_queue != NULL &&
            gw_gs200_parse_scene_status_frame(item.data,
                                              frame_len,
                                              &scene_status) == ESP_OK) {
            if (xQueueSend(s_scene_queue, &scene_status, 0) != pdTRUE) {
                gw_gs200_scene_status_t discarded;
                (void)xQueueReceive(s_scene_queue, &discarded, 0);
                (void)xQueueSend(s_scene_queue, &scene_status, 0);
            }
        }
        if (xQueueSend(s_uplink_queue, &item, 0) != pdTRUE) {
            increment_counter(&s_status.uplink_queue_drop_count);
        }
        frame_len = 0U;
    }
    xSemaphoreGive(s_parser_mutex);
}

/** @brief 将完整帧编码为业务约定的大写 ASCII hex。 */
static size_t encode_upper_hex(const uint8_t *frame, size_t frame_len, uint8_t *out)
{
    static const uint8_t digits[] = "0123456789ABCDEF";
    for (size_t i = 0U; i < frame_len; ++i) {
        out[i * 2U] = digits[(frame[i] >> 4U) & 0x0FU];
        out[i * 2U + 1U] = digits[frame[i] & 0x0FU];
    }
    return frame_len * 2U;
}

static int hex_nibble(uint8_t value)
{
    if (value >= (uint8_t)'0' && value <= (uint8_t)'9') {
        return value - (uint8_t)'0';
    }
    if (value >= (uint8_t)'a' && value <= (uint8_t)'f') {
        return value - (uint8_t)'a' + 10;
    }
    if (value >= (uint8_t)'A' && value <= (uint8_t)'F') {
        return value - (uint8_t)'A' + 10;
    }
    return -1;
}

static bool is_hex_separator(uint8_t value)
{
    return value == (uint8_t)' ' || value == (uint8_t)'\r' ||
           value == (uint8_t)'\n' || value == (uint8_t)'\t' ||
           value == (uint8_t)':' || value == (uint8_t)'-';
}

/**
 * @brief 兼容现网下行：可用 hex 文本发送，也可直接发送完整二进制帧。
 *
 * 只有所有非分隔符字符都是十六进制且位数为偶数时才按 hex 解码；否则保留原始
 * payload，随后仍必须通过完整 YMZL 帧校验。
 */
static esp_err_t decode_downlink(const uint8_t *payload,
                                 size_t payload_len,
                                 uint8_t out_frame[GW_YMZL_MAX_FRAME_LEN],
                                 size_t *out_len)
{
    if (payload == NULL || payload_len == 0U || out_frame == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t nibble_count = 0U;
    bool all_hex_text = true;
    for (size_t i = 0U; i < payload_len; ++i) {
        if (is_hex_separator(payload[i])) {
            continue;
        }
        if (hex_nibble(payload[i]) < 0) {
            all_hex_text = false;
            break;
        }
        ++nibble_count;
    }

    if (all_hex_text && nibble_count > 0U && (nibble_count & 1U) == 0U) {
        size_t decoded_len = nibble_count / 2U;
        if (decoded_len > GW_YMZL_MAX_FRAME_LEN) {
            return ESP_ERR_INVALID_SIZE;
        }

        int high = -1;
        size_t written = 0U;
        for (size_t i = 0U; i < payload_len; ++i) {
            if (is_hex_separator(payload[i])) {
                continue;
            }
            int nibble = hex_nibble(payload[i]);
            if (high < 0) {
                high = nibble;
            } else {
                out_frame[written++] = (uint8_t)((high << 4U) | nibble);
                high = -1;
            }
        }
        *out_len = written;
        return ESP_OK;
    }

    if (payload_len > GW_YMZL_MAX_FRAME_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out_frame, payload, payload_len);
    *out_len = payload_len;
    return ESP_OK;
}

/** @brief 按指数退避计算 MQTT 下行写 dongle 的下一次重试间隔。 */
static uint64_t downlink_retry_delay_us(uint8_t attempts)
{
    uint64_t delay = GW_DONGLE_DOWNLINK_RETRY_BASE_US;
    for (uint8_t i = 1U; i < attempts && delay < GW_DONGLE_DOWNLINK_RETRY_MAX_US; ++i) {
        delay *= 2U;
        if (delay > GW_DONGLE_DOWNLINK_RETRY_MAX_US) {
            delay = GW_DONGLE_DOWNLINK_RETRY_MAX_US;
        }
    }
    return delay;
}

/** @brief 处理每轮最多八条 MQTT 下行，并保留一个临时写失败的队首按序重试。 */
static void process_downlinks(void)
{
    for (size_t i = 0U; i < GW_DONGLE_BATCH_SIZE; ++i) {
        if (!s_downlink_pending.active) {
            gw_business_mqtt_message_t message;
            esp_err_t recv_err = gw_business_mqtt_recv(&message, 0U);
            if (recv_err == ESP_ERR_TIMEOUT) {
                return;
            }
            if (recv_err != ESP_OK) {
                return;
            }

            size_t frame_len = 0U;
            esp_err_t decode_err = decode_downlink(message.payload,
                                                   message.payload_len,
                                                   s_downlink_pending.data,
                                                   &frame_len);
            if (decode_err == ESP_OK) {
                decode_err = gw_ymzl_frame_validate(s_downlink_pending.data, frame_len);
            }
            if (decode_err != ESP_OK) {
                uint32_t invalid_count;
                increment_counter(&s_status.downlink_invalid_count);
                portENTER_CRITICAL(&s_state_lock);
                invalid_count = s_status.downlink_invalid_count;
                portEXIT_CRITICAL(&s_state_lock);
                if ((invalid_count & 0x3FU) == 1U) {
                    ESP_LOGW(TAG, "reject MQTT downlink: err=%s count=%lu",
                             esp_err_to_name(decode_err),
                             (unsigned long)invalid_count);
                }
                memset(&s_downlink_pending, 0, sizeof(s_downlink_pending));
                continue;
            }

            s_downlink_pending.active = true;
            s_downlink_pending.len = (uint8_t)frame_len;
            s_downlink_pending.attempts = 0U;
            s_downlink_pending.next_retry_us = 0U;
        }

        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (s_downlink_pending.next_retry_us != 0U &&
            now_us < s_downlink_pending.next_retry_us) {
            return;
        }

        esp_err_t err = gw_dongle_mqtt_bridge_send_frame(
            s_downlink_pending.data,
            s_downlink_pending.len,
            GW_DONGLE_SEND_TIMEOUT_MS);
        if (err == ESP_OK) {
            increment_counter(&s_status.downlink_frame_count);
            memset(&s_downlink_pending, 0, sizeof(s_downlink_pending));
            continue;
        }

        /*
         * 已通过协议校验的下行命令必须保持队首，直到 dongle 恢复并成功写入。
         * USB 重新枚举可能远超数秒，固定次数上限会让已经触发的定时任务静默丢失。
         * attempts 饱和后仍以 2 秒上限持续重试，仅用于计算退避和诊断。
         */
        if (s_downlink_pending.attempts < UINT8_MAX) {
            ++s_downlink_pending.attempts;
        }

        increment_counter(&s_status.downlink_retry_count);
        s_downlink_pending.next_retry_us =
            (uint64_t)esp_timer_get_time() +
            downlink_retry_delay_us(s_downlink_pending.attempts);
        if (s_downlink_pending.attempts == 1U ||
            (s_downlink_pending.attempts & 0x0FU) == 0U) {
            ESP_LOGW(TAG,
                     "write MQTT downlink failed, retry=%u: %s",
                     (unsigned)s_downlink_pending.attempts,
                     esp_err_to_name(err));
        }
        return;
    }
}

/** @brief 发布一条已由 host 回调完成校验和切帧的上行。 */
static void publish_uplink(const dongle_uplink_item_t *item)
{
    uint8_t payload[GW_YMZL_MAX_FRAME_LEN * 2U];
    size_t payload_len = encode_upper_hex(item->data, item->len, payload);
    esp_err_t err = gw_business_mqtt_send(payload, payload_len);
    if (err == ESP_OK) {
        increment_counter(&s_status.uplink_frame_count);
    } else {
        increment_counter(&s_status.uplink_mqtt_error_count);
    }
}

static void dongle_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "CH37X/MQTT data bridge worker started");
    TickType_t next_status_tick = xTaskGetTickCount() +
                                  pdMS_TO_TICKS(GW_DONGLE_STATUS_INTERVAL_MS);

    for (;;) {
        process_downlinks();

        dongle_uplink_item_t item;
        if (xQueueReceive(s_uplink_queue,
                          &item,
                          pdMS_TO_TICKS(GW_DONGLE_WORKER_POLL_MS)) == pdTRUE) {
            publish_uplink(&item);
            for (size_t i = 1U; i < GW_DONGLE_BATCH_SIZE; ++i) {
                if (xQueueReceive(s_uplink_queue, &item, 0) != pdTRUE) {
                    break;
                }
                publish_uplink(&item);
            }
        }

        process_downlinks();

        TickType_t now_tick = xTaskGetTickCount();
        if ((int32_t)(now_tick - next_status_tick) >= 0) {
            gw_dongle_mqtt_bridge_status_t bridge_status;
            gw_business_mqtt_status_t mqtt_status;
            gw_ch37x_bridge_info_t ch37x_status;
            if (gw_dongle_mqtt_bridge_get_status(&bridge_status) == ESP_OK &&
                gw_business_mqtt_get_status(&mqtt_status) == ESP_OK &&
                gw_ch37x_bridge_get_info(&ch37x_status) == ESP_OK) {
                ESP_LOGI(TAG,
                         "status: uplink=%lu queue_drop=%lu mqtt_err=%lu "
                         "downlink=%lu invalid=%lu retry=%lu down_drop=%lu "
                         "tx_err=%lu raw_pending=%u "
                         "raw_drop=%lu mqtt=%s published=%lu",
                         (unsigned long)bridge_status.uplink_frame_count,
                         (unsigned long)bridge_status.uplink_queue_drop_count,
                         (unsigned long)bridge_status.uplink_mqtt_error_count,
                         (unsigned long)bridge_status.downlink_frame_count,
                         (unsigned long)bridge_status.downlink_invalid_count,
                         (unsigned long)bridge_status.downlink_retry_count,
                         (unsigned long)bridge_status.downlink_drop_count,
                         (unsigned long)bridge_status.tx_error_count,
                         (unsigned int)ch37x_status.pending_bytes,
                         (unsigned long)ch37x_status.rx_drop_count,
                         gw_business_mqtt_state_to_string(mqtt_status.state),
                         (unsigned long)mqtt_status.published_count);
            }
            next_status_tick = now_tick + pdMS_TO_TICKS(GW_DONGLE_STATUS_INTERVAL_MS);
        }
    }
}

esp_err_t gw_dongle_mqtt_bridge_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool already_initialized = s_initialized;
    portEXIT_CRITICAL(&s_state_lock);
    if (already_initialized) {
        return ESP_OK;
    }

    gw_ymzl_parser_init(&s_parser);
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_downlink_pending, 0, sizeof(s_downlink_pending));
    s_uplink_queue = xQueueCreateStatic(GW_DONGLE_UPLINK_QUEUE_CAPACITY,
                                        sizeof(dongle_uplink_item_t),
                                        s_uplink_queue_storage,
                                        &s_uplink_queue_buffer);
    s_scene_queue = xQueueCreateStatic(GW_DONGLE_SCENE_QUEUE_CAPACITY,
                                       sizeof(gw_gs200_scene_status_t),
                                       s_scene_queue_storage,
                                       &s_scene_queue_buffer);
    s_scene_query_mutex =
        xSemaphoreCreateMutexStatic(&s_scene_query_mutex_buffer);
    s_tx_mutex = xSemaphoreCreateMutexStatic(&s_tx_mutex_buffer);
    s_parser_mutex = xSemaphoreCreateMutexStatic(&s_parser_mutex_buffer);
    if (s_uplink_queue == NULL || s_scene_queue == NULL ||
        s_scene_query_mutex == NULL || s_tx_mutex == NULL ||
        s_parser_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gw_ch37x_bridge_observer_t observer = {
        .on_data = dongle_on_data,
        .on_reset = dongle_on_reset,
        .ctx = NULL,
    };
    esp_err_t err = gw_ch37x_bridge_set_observer(&observer);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_initialized = true;
    s_status.initialized = true;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "C dongle data bridge initialized; RX owned by C runtime");
    return ESP_OK;
}

esp_err_t gw_dongle_mqtt_bridge_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool initialized = s_initialized;
    bool running = s_running;
    portEXIT_CRITICAL(&s_state_lock);
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (running) {
        return ESP_OK;
    }

    TaskHandle_t handle = xTaskCreateStatic(dongle_worker,
                                            "dongle_mqtt",
                                            GW_DONGLE_WORKER_STACK_DEPTH,
                                            NULL,
                                            GW_DONGLE_WORKER_PRIORITY,
                                            s_worker_stack,
                                            &s_worker_task_buffer);
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_worker_task = handle;
    s_running = true;
    s_status.running = true;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t gw_dongle_mqtt_bridge_send_frame(const uint8_t *frame,
                                           size_t frame_len,
                                           uint32_t timeout_ms)
{
    esp_err_t err = gw_ymzl_frame_validate(frame, frame_len);
    if (err != ESP_OK) {
        increment_counter(&s_status.tx_error_count);
        return err;
    }
    if (s_tx_mutex == NULL) {
        increment_counter(&s_status.tx_error_count);
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t start_us = (uint64_t)esp_timer_get_time();
    uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;
    uint64_t deadline_us = start_us + timeout_us;
    TickType_t lock_wait = timeout_us_to_ticks_ceil(timeout_us);
    if (xSemaphoreTake(s_tx_mutex, lock_wait) != pdTRUE) {
        increment_counter(&s_status.tx_error_count);
        return ESP_ERR_TIMEOUT;
    }

    uint64_t now_us = 0U;
    for (;;) {
        now_us = (uint64_t)esp_timer_get_time();
        uint64_t earliest_us = s_last_send_us + GW_DONGLE_SEND_INTERVAL_US;
        if (s_last_send_us == 0U || now_us >= earliest_us) {
            break;
        }
        if (timeout_ms == 0U || now_us >= deadline_us || earliest_us >= deadline_us) {
            xSemaphoreGive(s_tx_mutex);
            increment_counter(&s_status.tx_error_count);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(timeout_us_to_ticks_ceil(earliest_us - now_us));
    }

    now_us = (uint64_t)esp_timer_get_time();
    if (timeout_ms > 0U && now_us >= deadline_us) {
        xSemaphoreGive(s_tx_mutex);
        increment_counter(&s_status.tx_error_count);
        return ESP_ERR_TIMEOUT;
    }
    uint32_t remaining_ms = 0U;
    if (timeout_ms > 0U && now_us < deadline_us) {
        remaining_ms = (uint32_t)((deadline_us - now_us + 999ULL) / 1000ULL);
    }
    err = gw_ch37x_bridge_write(frame, frame_len, remaining_ms);
    /*
     * 即使分包发送中途失败，也必须从本次尝试结束后重新计算间隔，避免下一帧紧跟
     * 在残帧之后写入 dongle。
     */
    s_last_send_us = (uint64_t)esp_timer_get_time();
    xSemaphoreGive(s_tx_mutex);

    if (err != ESP_OK) {
        increment_counter(&s_status.tx_error_count);
    }
    return err;
}

esp_err_t gw_dongle_mqtt_bridge_read_scene(
    const uint8_t *frame,
    size_t frame_len,
    uint16_t expected_source_addr,
    uint16_t expected_network_id,
    uint8_t *out_scene_no,
    uint32_t timeout_ms)
{
    if (frame == NULL || out_scene_no == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_scene_queue == NULL || s_scene_query_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t start_us = (uint64_t)esp_timer_get_time();
    uint64_t deadline_us = start_us + (uint64_t)timeout_ms * 1000ULL;
    if (xSemaphoreTake(s_scene_query_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* 旧状态不能作为本次读取结果；清空后再发送读取指令。 */
    gw_gs200_scene_status_t status;
    while (xQueueReceive(s_scene_queue, &status, 0) == pdTRUE) {
    }

    uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (now_us >= deadline_us) {
        xSemaphoreGive(s_scene_query_mutex);
        return ESP_ERR_TIMEOUT;
    }
    uint32_t remaining_ms =
        (uint32_t)((deadline_us - now_us + 999ULL) / 1000ULL);
    esp_err_t err = gw_dongle_mqtt_bridge_send_frame(frame,
                                                      frame_len,
                                                      remaining_ms);
    if (err != ESP_OK) {
        xSemaphoreGive(s_scene_query_mutex);
        return err;
    }

    for (;;) {
        now_us = (uint64_t)esp_timer_get_time();
        if (now_us >= deadline_us) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        TickType_t wait_ticks =
            timeout_us_to_ticks_ceil(deadline_us - now_us);
        if (xQueueReceive(s_scene_queue, &status, wait_ticks) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (status.source_addr == expected_source_addr &&
            status.network_id == expected_network_id) {
            *out_scene_no = status.scene_no;
            err = ESP_OK;
            break;
        }
        /* 其他灯具状态仍在 MQTT 上行，本次读取只忽略其结构化副本。 */
    }

    xSemaphoreGive(s_scene_query_mutex);
    return err;
}

esp_err_t gw_dongle_mqtt_bridge_get_status(gw_dongle_mqtt_bridge_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);
    *out = s_status;
    bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_state_lock);
    return initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}
