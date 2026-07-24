#include "gw_business_mqtt.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "gw_business_mqtt_register.h"
#include "gw_board.h"
#include "gw_defaults.h"
#include "gw_mqtt_client.h"
#include "gw_net.h"
#include "gw_result.h"
#include "gw_state.h"
#include "gw_storage.h"

static const char *TAG = "gw_business_mqtt";

#define GW_BUSINESS_MQTT_EVENT_QUEUE_CAPACITY       8U
#define GW_BUSINESS_MQTT_DATA_QUEUE_CAPACITY       64U
#define GW_BUSINESS_MQTT_WORKER_STACK_BYTES         (8U * 1024U)
#define GW_BUSINESS_MQTT_WORKER_STACK_DEPTH         \
    (GW_BUSINESS_MQTT_WORKER_STACK_BYTES / sizeof(StackType_t))
#define GW_BUSINESS_MQTT_WORKER_PRIORITY            7U
#define GW_BUSINESS_MQTT_WORKER_POLL_MS             100U
#define GW_BUSINESS_MQTT_NETWORK_SETTLE_MS           1500U
#define GW_BUSINESS_MQTT_CONNECT_TIMEOUT_MS         15000U
#define GW_BUSINESS_MQTT_SUBSCRIBE_TIMEOUT_MS       15000U
#define GW_BUSINESS_MQTT_PUBLISH_TIMEOUT_MS         30000U
#define GW_BUSINESS_MQTT_RETRY_MIN_MS               500U
#define GW_BUSINESS_MQTT_RETRY_MAX_MS               300000U
#define GW_BUSINESS_MQTT_AUTH_RETRY_MIN_MS          5000U
#define GW_BUSINESS_MQTT_AUTH_RETRY_MAX_MS          3600000U
#define GW_BUSINESS_MQTT_MQTT_STACK_MIN_BYTES       (4U * 1024U)
#define GW_BUSINESS_MQTT_MQTT_STACK_MAX_BYTES       (32U * 1024U)
#define GW_BUSINESS_MQTT_STOPPED_BIT                BIT0
#define GW_BUSINESS_MQTT_NAME                       "business_mqtt"

/** @brief 回调到 worker 的固定内存事件。 */
typedef struct {
    gw_mqtt_client_t *source_client;
    uint32_t generation;
    gw_mqtt_client_event_id_t event_id;
    int msg_id;
    int esp_err;
    int connect_return_code;
    int topic_len;
    int data_len;
    int total_data_len;
    int current_data_offset;
    bool subscribe_failed;
    char topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    uint8_t data[GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN];
} business_mqtt_event_t;

/** @brief 上行 FIFO 条目；发布和移除期间由 outbound_mutex 保证队首稳定。 */
typedef struct {
    size_t payload_len;
    uint8_t payload[GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN];
} business_mqtt_outbound_t;

/** @brief MQTT DATA 多片重组状态，只由业务 worker 访问。 */
typedef struct {
    bool active;
    uint32_t generation;
    int msg_id;
    size_t total_len;
    size_t received_len;
    char topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    uint8_t payload[GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN];
} business_mqtt_fragment_t;

/** @brief 每一代底层 client 的回调标识。 */
typedef struct {
    uint32_t generation;
} business_mqtt_callback_binding_t;

/** @brief 复制后的服务配置，避免持有调用方字符串指针。 */
typedef struct {
    char register_url[256];
    char register_group_id[32];
    char topic_group_id[32];
    uint32_t register_timeout_ms;
    size_t register_max_response_bytes;
    uint32_t retry_ms;
    uint32_t auth_retry_ms;
    size_t mqtt_task_stack_bytes;
    bool allow_untrusted_https;
} business_mqtt_config_copy_t;

/**
 * @brief 单例服务上下文。
 *
 * client、凭据、重组器和状态机只允许 worker 访问。回调按控制事件和 DATA 分队列投递；外部发送者
 * 只在 outbound_mutex 保护下写上行环形队列。
 */
typedef struct {
    portMUX_TYPE lock;
    bool initialized;
    bool run_requested;
    bool stop_requested;
    bool credentials_valid;
    bool online;
    gw_business_mqtt_state_t state;

    bool retry_pending;
    bool auth_retry_pending;
    bool network_ready_seen;
    TickType_t next_retry_tick;
    TickType_t network_settle_deadline_tick;
    TickType_t connect_deadline_tick;
    TickType_t subscribe_deadline_tick;

    uint32_t register_attempts;
    uint32_t connect_attempts;
    uint32_t connected_count;
    uint32_t subscribe_count;
    uint32_t published_count;
    uint32_t received_count;
    uint32_t outbound_dropped_count;
    uint32_t downlink_dropped_count;
    uint32_t event_dropped_count;
    int last_esp_err;
    int last_connect_return_code;
    char last_reason[64];

    char business_device_id[32];
    char up_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    char down_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    business_mqtt_config_copy_t config;

    gw_mqtt_client_t *client;
    uint32_t client_generation;
    int subscribe_msg_id;
    business_mqtt_callback_binding_t callback_binding;
    gw_business_mqtt_credentials_t credentials;
    business_mqtt_fragment_t fragment;

    business_mqtt_outbound_t *outbound;
    size_t outbound_head;
    size_t outbound_count;
    bool outbound_inflight;
    uint32_t outbound_generation;
    int outbound_msg_id;
    TickType_t outbound_deadline_tick;
    SemaphoreHandle_t outbound_mutex;
    StaticSemaphore_t outbound_mutex_buffer;

    QueueHandle_t event_queue;
    StaticQueue_t event_queue_buffer;
    uint8_t *event_queue_storage;

    QueueHandle_t data_queue;
    StaticQueue_t data_queue_buffer;
    uint8_t *data_queue_storage;

    QueueHandle_t downlink_queue;
    StaticQueue_t downlink_queue_buffer;
    uint8_t *downlink_queue_storage;

    EventGroupHandle_t control_events;
    StaticEventGroup_t control_events_buffer;
    TaskHandle_t worker_task;
    StaticTask_t worker_task_buffer;
    StackType_t worker_task_stack[GW_BUSINESS_MQTT_WORKER_STACK_DEPTH];
} business_mqtt_context_t;

static business_mqtt_context_t s_business = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/** @brief 释放初始化阶段申请的 PSRAM；仅允许在 worker 创建前调用。 */
static void free_psram_storage(void)
{
    heap_caps_free(s_business.downlink_queue_storage);
    heap_caps_free(s_business.data_queue_storage);
    heap_caps_free(s_business.event_queue_storage);
    heap_caps_free(s_business.outbound);
    s_business.downlink_queue_storage = NULL;
    s_business.data_queue_storage = NULL;
    s_business.event_queue_storage = NULL;
    s_business.outbound = NULL;
}

/** @brief 判断 TickType_t 截止时间是否到达，兼容计数器回绕。 */
static bool tick_reached(TickType_t now, TickType_t target)
{
    return (int32_t)(now - target) >= 0;
}

/** @brief 将毫秒转为至少一个 tick，避免非零短超时被截断成非阻塞。 */
static TickType_t timeout_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return 0;
    }
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks > 0 ? ticks : 1;
}

/** @brief 复制非空字符串并检查目标容量。 */
static esp_err_t copy_required_string(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0U || value == NULL || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return strlcpy(out, value, out_size) < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/** @brief 使用传入值或默认值填充内部字符串。 */
static esp_err_t copy_default_string(char *out,
                                     size_t out_size,
                                     const char *value,
                                     const char *default_value)
{
    const char *selected = value != NULL && value[0] != '\0' ? value : default_value;
    return copy_required_string(out, out_size, selected);
}

/** @brief 校验 storage 返回的原始 MAC 是否为 12 位十六进制文本。 */
static bool valid_mac_text(const char *mac)
{
    if (mac == NULL || strlen(mac) != 12U) {
        return false;
    }
    for (size_t i = 0U; i < 12U; ++i) {
        if (!isxdigit((unsigned char)mac[i])) {
            return false;
        }
    }
    return true;
}

/** @brief 更新状态和脱敏原因。 */
static void set_state(gw_business_mqtt_state_t state, const char *reason)
{
    portENTER_CRITICAL(&s_business.lock);
    s_business.state = state;
    if (reason != NULL) {
        strlcpy(s_business.last_reason, reason, sizeof(s_business.last_reason));
    }
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 更新底层错误快照，不记录用户名、密码或注册 URL。 */
static void set_last_error(int esp_err, int connect_return_code, const char *reason)
{
    portENTER_CRITICAL(&s_business.lock);
    s_business.last_esp_err = esp_err;
    s_business.last_connect_return_code = connect_return_code;
    if (reason != NULL) {
        strlcpy(s_business.last_reason, reason, sizeof(s_business.last_reason));
    }
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 仅在普通联网状态间同步设备主状态，避免覆盖 OTA、恢复出厂等互斥状态。 */
static void sync_device_online_state(bool online)
{
    gw_state_snapshot_t snapshot;
    if (gw_state_get_snapshot(&snapshot) != ESP_OK ||
        snapshot.device_status == GW_DEVICE_STATUS_BOOTING ||
        snapshot.device_status == GW_DEVICE_STATUS_OFFLINE ||
        snapshot.device_status == GW_DEVICE_STATUS_ONLINE) {
        gw_state_set_device_status(online ? GW_DEVICE_STATUS_ONLINE :
                                            GW_DEVICE_STATUS_OFFLINE);
    }
}

/** @brief 设置 online 标志。 */
static void set_online(bool online)
{
    portENTER_CRITICAL(&s_business.lock);
    s_business.online = online;
    portEXIT_CRITICAL(&s_business.lock);

    /*
     * 当前量产运行时只启用原生 business MQTT，因此公共状态应反映这条真实业务链路。
     * 这里只同步状态，不发布旧 dev_mqtt 的登录/属性报文。
     */
    gw_state_set_mqtt_status(online ? GW_MQTT_STATUS_CONNECTED :
                                      GW_MQTT_STATUS_DISCONNECTED);
    sync_device_online_state(online);

    /* 完成下行订阅后才算业务在线；重连、停机和异常路径统一熄灭 GPIO40。 */
    esp_err_t led_err = gw_board_mqtt_led_set(online);
    if (led_err != ESP_OK && led_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "set MQTT LED failed: %s", esp_err_to_name(led_err));
    }
}

/** @brief 设置凭据有效标志。 */
static void set_credentials_valid(bool valid)
{
    portENTER_CRITICAL(&s_business.lock);
    s_business.credentials_valid = valid;
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 读取 start/stop 控制标志。 */
static void read_control_flags(bool *run_requested, bool *stop_requested)
{
    portENTER_CRITICAL(&s_business.lock);
    *run_requested = s_business.run_requested;
    *stop_requested = s_business.stop_requested;
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 增加 MQTT 回调事件丢弃计数。 */
static void count_event_drop(const char *reason)
{
    portENTER_CRITICAL(&s_business.lock);
    ++s_business.event_dropped_count;
    if (reason != NULL) {
        strlcpy(s_business.last_reason, reason, sizeof(s_business.last_reason));
    }
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 增加下行丢弃计数。 */
static void count_downlink_drop(const char *reason)
{
    portENTER_CRITICAL(&s_business.lock);
    ++s_business.downlink_dropped_count;
    if (reason != NULL) {
        strlcpy(s_business.last_reason, reason, sizeof(s_business.last_reason));
    }
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 清除当前未完成的 MQTT DATA 重组。 */
static void clear_fragment(void)
{
    memset(&s_business.fragment, 0, sizeof(s_business.fragment));
}

/**
 * @brief 清除当前 MQTT 会话的在途标记，但保留业务 FIFO 队首等待下次会话重发。
 */
static void clear_outbound_inflight(void)
{
    if (s_business.outbound_mutex == NULL ||
        xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_business.outbound_inflight = false;
    s_business.outbound_generation = 0U;
    s_business.outbound_msg_id = -1;
    s_business.outbound_deadline_tick = 0;
    xSemaphoreGive(s_business.outbound_mutex);
}

/**
 * @brief 底层 MQTT 回调。
 *
 * 这里严格只复制事件并投递固定队列；不会做注册、销毁 client、发布、订阅、分片重组
 * 或 dongle I/O。来源 client 和 generation 会随事件复制，供 worker 丢弃旧实例事件。
 */
static void business_mqtt_event_cb(gw_mqtt_client_t *client,
                                   const gw_mqtt_client_event_t *event,
                                   void *user_ctx)
{
    const business_mqtt_callback_binding_t *binding =
        (const business_mqtt_callback_binding_t *)user_ctx;
    if (client == NULL || event == NULL || binding == NULL ||
        !s_business.initialized || s_business.event_queue == NULL ||
        s_business.data_queue == NULL) {
        return;
    }

    if (event->topic_len < 0 || event->data_len < 0 || event->total_data_len < 0 ||
        event->current_data_offset < 0 ||
        (size_t)event->topic_len >= GW_BUSINESS_MQTT_TOPIC_MAX_LEN ||
        (size_t)event->data_len > GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN ||
        (event->topic_len > 0 && event->topic == NULL) ||
        (event->data_len > 0 && event->data == NULL)) {
        count_event_drop("callback_event_invalid");
        return;
    }

    business_mqtt_event_t copied = {
        .source_client = client,
        .generation = binding->generation,
        .event_id = event->event_id,
        .msg_id = event->msg_id,
        .esp_err = event->esp_err,
        .connect_return_code = event->connect_return_code,
        .topic_len = event->topic_len,
        .data_len = event->data_len,
        .total_data_len = event->total_data_len,
        .current_data_offset = event->current_data_offset,
        .subscribe_failed = event->subscribe_failed,
    };
    if (event->topic_len > 0) {
        memcpy(copied.topic, event->topic, (size_t)event->topic_len);
        copied.topic[event->topic_len] = '\0';
    }
    if (event->data_len > 0) {
        memcpy(copied.data, event->data, (size_t)event->data_len);
    }

    /*
     * DATA 与连接控制事件分离，避免一阵下行流量挤掉 SUBACK、PUBACK 或断线事件。
     * DATA 允许短暂等待，由更高优先级的业务 worker 及时排空，形成有限背压。
     */
    QueueHandle_t target_queue = event->event_id == GW_MQTT_CLIENT_EVENT_DATA ?
                                 s_business.data_queue : s_business.event_queue;
    TickType_t queue_wait = event->event_id == GW_MQTT_CLIENT_EVENT_DATA ?
                            timeout_ticks(20U) : 0;
    if (xQueueSend(target_queue, &copied, queue_wait) != pdTRUE) {
        count_event_drop(event->event_id == GW_MQTT_CLIENT_EVENT_DATA ?
                         "callback_data_queue_full" : "callback_control_queue_full");
        return;
    }
    if (s_business.worker_task != NULL) {
        xTaskNotifyGive(s_business.worker_task);
    }
}

/** @brief 判断一段 topic 是否精确等于固定 down Topic。 */
static bool is_down_topic(const char *topic, int topic_len)
{
    size_t expected_len = strlen(s_business.down_topic);
    return topic != NULL && topic_len >= 0 && (size_t)topic_len == expected_len &&
           memcmp(topic, s_business.down_topic, expected_len) == 0;
}

/** @brief 将完整下行投递给公开 recv 队列；队列满时丢最旧一条。 */
static void enqueue_downlink(const char *topic, const uint8_t *payload, size_t payload_len)
{
    gw_business_mqtt_message_t message = {
        .payload_len = payload_len,
    };
    strlcpy(message.topic, topic, sizeof(message.topic));
    if (payload_len > 0U) {
        memcpy(message.payload, payload, payload_len);
    }

    if (xQueueSend(s_business.downlink_queue, &message, 0) != pdTRUE) {
        gw_business_mqtt_message_t discarded;
        if (xQueueReceive(s_business.downlink_queue, &discarded, 0) == pdTRUE) {
            count_downlink_drop("downlink_queue_drop_oldest");
        }
        if (xQueueSend(s_business.downlink_queue, &message, 0) != pdTRUE) {
            count_downlink_drop("downlink_queue_full");
            return;
        }
    }

    portENTER_CRITICAL(&s_business.lock);
    ++s_business.received_count;
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 在 worker 中严格按 offset 顺序重组一个 MQTT DATA 事件。 */
static void process_data_event(const business_mqtt_event_t *event)
{
    size_t total_len = (size_t)event->total_data_len;
    size_t data_len = (size_t)event->data_len;
    size_t offset = (size_t)event->current_data_offset;

    if (total_len > GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN || offset > total_len ||
        data_len > total_len - offset) {
        clear_fragment();
        count_downlink_drop("downlink_fragment_size_invalid");
        return;
    }

    if (offset == 0U) {
        clear_fragment();
        if (!is_down_topic(event->topic, event->topic_len)) {
            count_downlink_drop("downlink_topic_mismatch");
            return;
        }

        if (total_len == data_len) {
            enqueue_downlink(s_business.down_topic, event->data, data_len);
            return;
        }

        s_business.fragment.active = true;
        s_business.fragment.generation = event->generation;
        s_business.fragment.msg_id = event->msg_id;
        s_business.fragment.total_len = total_len;
        s_business.fragment.received_len = data_len;
        strlcpy(s_business.fragment.topic,
                s_business.down_topic,
                sizeof(s_business.fragment.topic));
        if (data_len > 0U) {
            memcpy(s_business.fragment.payload, event->data, data_len);
        }
        return;
    }

    business_mqtt_fragment_t *fragment = &s_business.fragment;
    bool topic_matches = event->topic_len == 0 || is_down_topic(event->topic, event->topic_len);
    if (!fragment->active || fragment->generation != event->generation ||
        fragment->msg_id != event->msg_id || fragment->total_len != total_len ||
        fragment->received_len != offset || !topic_matches) {
        clear_fragment();
        count_downlink_drop("downlink_fragment_sequence_invalid");
        return;
    }

    if (data_len > 0U) {
        memcpy(fragment->payload + offset, event->data, data_len);
    }
    fragment->received_len += data_len;
    if (fragment->received_len == fragment->total_len) {
        enqueue_downlink(fragment->topic, fragment->payload, fragment->total_len);
        clear_fragment();
    }
}

/** @brief 销毁当前底层 client；必须由 worker 调用。 */
static void destroy_client(void)
{
    gw_mqtt_client_t *client = s_business.client;
    s_business.client = NULL;
    s_business.subscribe_msg_id = -1;
    s_business.connect_deadline_tick = 0;
    s_business.subscribe_deadline_tick = 0;
    set_online(false);
    clear_fragment();
    clear_outbound_inflight();
    if (client != NULL) {
        (void)gw_mqtt_client_destroy(client);
    }
}

/** @brief 清除注册凭据；必须由 worker 调用。 */
static void clear_credentials(void)
{
    gw_business_mqtt_credentials_clear(&s_business.credentials);
    set_credentials_valid(false);
}

/** @brief 安排普通或鉴权重试，并按规则处理凭据。 */
static void schedule_retry(bool authentication_failed,
                           int esp_err,
                           int connect_return_code,
                           const char *reason)
{
    destroy_client();
    if (authentication_failed) {
        clear_credentials();
        gw_state_set_mqtt_status(GW_MQTT_STATUS_AUTH_FAILED);
    }

    uint32_t delay_ms = authentication_failed ?
                        s_business.config.auth_retry_ms :
                        s_business.config.retry_ms;
    portENTER_CRITICAL(&s_business.lock);
    s_business.retry_pending = true;
    s_business.auth_retry_pending = authentication_failed;
    s_business.next_retry_tick = xTaskGetTickCount() + timeout_ticks(delay_ms);
    s_business.last_esp_err = esp_err;
    s_business.last_connect_return_code = connect_return_code;
    portEXIT_CRITICAL(&s_business.lock);
    set_state(GW_BUSINESS_MQTT_STATE_RETRY_WAIT,
              reason != NULL ? reason :
              (authentication_failed ? "auth_retry_wait" : "retry_wait"));

    ESP_LOGW(TAG,
             "%s; retry in %lu ms (esp_err=%d return_code=%d)",
             authentication_failed ? "business MQTT authentication failed" :
                                     "business MQTT session failed",
             (unsigned long)delay_ms,
             esp_err,
             connect_return_code);
}

/** @brief 清除已到期的重试标志。 */
static void clear_retry(void)
{
    portENTER_CRITICAL(&s_business.lock);
    s_business.retry_pending = false;
    s_business.auth_retry_pending = false;
    s_business.next_retry_tick = 0;
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 查询 4G 是否已经获得可用 IP。 */
static bool network_ready(void)
{
    gw_net_status_snapshot_t status = {0};
    return gw_net_get_status(&status) == ESP_OK &&
           status.profile == GW_NET_PROFILE_4G &&
           status.public_status == GW_NETWORK_STATUS_CONNECTED;
}

/** @brief 提交 down Topic 订阅并开始等待 SUBACK。 */
static esp_err_t begin_subscribe(void)
{
    if (s_business.client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = -1;
    esp_err_t err = gw_mqtt_client_subscribe_with_msg_id(s_business.client,
                                                         s_business.down_topic,
                                                         0,
                                                         &msg_id);
    if (err != ESP_OK) {
        return err;
    }

    s_business.subscribe_msg_id = msg_id;
    s_business.subscribe_deadline_tick =
        xTaskGetTickCount() + timeout_ticks(GW_BUSINESS_MQTT_SUBSCRIBE_TIMEOUT_MS);
    portENTER_CRITICAL(&s_business.lock);
    ++s_business.subscribe_count;
    portEXIT_CRITICAL(&s_business.lock);
    set_state(GW_BUSINESS_MQTT_STATE_SUBSCRIBING, "subscribing");
    return ESP_OK;
}

/** @brief 使用当前注册凭据创建并启动独立业务 client。 */
static esp_err_t begin_connect(void)
{
    if (!s_business.credentials_valid || s_business.client != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ++s_business.client_generation;
    if (s_business.client_generation == 0U) {
        ++s_business.client_generation;
    }
    s_business.callback_binding.generation = s_business.client_generation;

    gw_mqtt_client_config_t mqtt_config = {
        .role = GW_MQTT_CLIENT_ROLE_BUSINESS,
        .name = GW_BUSINESS_MQTT_NAME,
        .broker_uri = s_business.credentials.broker_uri,
        .client_id = s_business.credentials.client_id,
        .username = s_business.credentials.username[0] != '\0' ?
                    s_business.credentials.username : NULL,
        .password = s_business.credentials.password[0] != '\0' ?
                    s_business.credentials.password : NULL,
        .keepalive_sec = 60,
        .qos_default = 0,
        .clean_session = true,
        .retain_default = false,
        .last_will_enabled = false,
        .reconnect_enabled = false,
        .reconnect_min_sec = 0,
        .reconnect_max_sec = 0,
        /* ESP-MQTT 缓冲还要容纳 Topic 和协议头，公开 API 的入队上限仍严格为 256。 */
        .max_payload_size = GW_BUSINESS_MQTT_PAYLOAD_SIZE,
        .task_stack_bytes = s_business.config.mqtt_task_stack_bytes,
    };

    esp_err_t err = gw_mqtt_client_create(&mqtt_config,
                                           business_mqtt_event_cb,
                                           &s_business.callback_binding,
                                           &s_business.client);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&s_business.lock);
    ++s_business.connect_attempts;
    portEXIT_CRITICAL(&s_business.lock);
    gw_state_set_mqtt_status(GW_MQTT_STATUS_CONNECTING);
    sync_device_online_state(false);
    set_state(GW_BUSINESS_MQTT_STATE_CONNECTING, "connecting");
    s_business.connect_deadline_tick =
        xTaskGetTickCount() + timeout_ticks(GW_BUSINESS_MQTT_CONNECT_TIMEOUT_MS);
    err = gw_mqtt_client_connect(s_business.client);
    if (err != ESP_OK) {
        destroy_client();
        return err;
    }
    return ESP_OK;
}

/** @brief 同步执行一次 HTTPS 注册；仅由 worker 调用。 */
static esp_err_t register_credentials(void)
{
    gw_business_mqtt_register_config_t register_config = {
        .register_url = s_business.config.register_url,
        .business_device_id = s_business.business_device_id,
        .register_group_id = s_business.config.register_group_id,
        .timeout_ms = s_business.config.register_timeout_ms,
        .max_response_bytes = s_business.config.register_max_response_bytes,
        .allow_untrusted_https = s_business.config.allow_untrusted_https,
    };
    gw_result_t result = gw_result_ok();

    portENTER_CRITICAL(&s_business.lock);
    ++s_business.register_attempts;
    portEXIT_CRITICAL(&s_business.lock);
    set_state(GW_BUSINESS_MQTT_STATE_REGISTERING, "https_registering");

    clear_credentials();
    esp_err_t err = gw_business_mqtt_register(&register_config,
                                               &s_business.credentials,
                                               &result);
    if (err != ESP_OK) {
        set_last_error(err,
                       0,
                       result.reason_code != NULL ? result.reason_code : "register_failed");
        return err;
    }

    set_credentials_valid(true);
    clear_retry();
    set_last_error(ESP_OK, 0, "register_ok");
    ESP_LOGI(TAG,
             "business MQTT registration succeeded: device_id=%s client_id_len=%u",
             s_business.business_device_id,
             (unsigned int)strlen(s_business.credentials.client_id));
    return ESP_OK;
}

/**
 * @brief 用匹配的 QoS1 PUBACK 完成业务 FIFO 队首所有权转移。
 *
 * 队首只有收到当前 client generation、当前 message id 的确认后才会移除。旧会话迟到
 * 的确认会被忽略，断线后该队首仍由业务 FIFO 持有并在新会话中重发。
 */
static void complete_outbound_publish(const business_mqtt_event_t *event)
{
    bool completed = false;
    if (event == NULL || s_business.outbound_mutex == NULL ||
        xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_business.outbound_inflight &&
        s_business.outbound_count > 0U &&
        s_business.outbound_generation == event->generation &&
        s_business.outbound_msg_id == event->msg_id) {
        business_mqtt_outbound_t *item =
            &s_business.outbound[s_business.outbound_head];
        memset(item, 0, sizeof(*item));
        s_business.outbound_head =
            (s_business.outbound_head + 1U) % GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY;
        --s_business.outbound_count;
        s_business.outbound_inflight = false;
        s_business.outbound_generation = 0U;
        s_business.outbound_msg_id = -1;
        s_business.outbound_deadline_tick = 0;
        completed = true;
    }
    xSemaphoreGive(s_business.outbound_mutex);

    if (completed) {
        portENTER_CRITICAL(&s_business.lock);
        ++s_business.published_count;
        portEXIT_CRITICAL(&s_business.lock);
        xTaskNotifyGive(s_business.worker_task);
    }
}

/**
 * @brief 处理单个底层事件。
 *
 * 返回 false 表示事件触发了 client 销毁，调用方应停止处理本轮剩余事件。
 */
static bool process_mqtt_event(const business_mqtt_event_t *event)
{
    if (event->source_client != s_business.client ||
        event->generation != s_business.client_generation) {
        return true;
    }

    set_last_error(event->esp_err, event->connect_return_code, NULL);
    switch (event->event_id) {
    case GW_MQTT_CLIENT_EVENT_CONNECTED: {
        if (s_business.state != GW_BUSINESS_MQTT_STATE_CONNECTING) {
            return true;
        }
        portENTER_CRITICAL(&s_business.lock);
        ++s_business.connected_count;
        portEXIT_CRITICAL(&s_business.lock);
        esp_err_t err = begin_subscribe();
        if (err != ESP_OK) {
            schedule_retry(false, err, 0, "subscribe_submit_failed");
            return false;
        }
        return true;
    }
    case GW_MQTT_CLIENT_EVENT_SUBSCRIBED:
        if (s_business.state == GW_BUSINESS_MQTT_STATE_SUBSCRIBING &&
            event->msg_id == s_business.subscribe_msg_id) {
            if (event->subscribe_failed) {
                /*
                 * SUBACK 的 failure code 属于 Topic/ACL 授权失败。清除本轮凭据，避免
                 * 用同一凭据永久假在线并反复连接。
                 */
                schedule_retry(true,
                               ESP_ERR_INVALID_RESPONSE,
                               0,
                               "subscribe_rejected");
                return false;
            }
            set_online(true);
            set_state(GW_BUSINESS_MQTT_STATE_ONLINE, "online");
            ESP_LOGI(TAG, "business MQTT online: down_topic=%s", s_business.down_topic);
        }
        return true;
    case GW_MQTT_CLIENT_EVENT_DATA:
        if (s_business.state == GW_BUSINESS_MQTT_STATE_SUBSCRIBING ||
            s_business.state == GW_BUSINESS_MQTT_STATE_ONLINE) {
            process_data_event(event);
        }
        return true;
    case GW_MQTT_CLIENT_EVENT_AUTH_FAILED:
        schedule_retry(true,
                       event->esp_err,
                       event->connect_return_code,
                       "auth_failed");
        return false;
    case GW_MQTT_CLIENT_EVENT_DISCONNECTED:
        schedule_retry(false,
                       event->esp_err,
                       event->connect_return_code,
                       "disconnected");
        return false;
    case GW_MQTT_CLIENT_EVENT_ERROR:
        /* 明确的用户名/授权错误已由通用层转换为 AUTH_FAILED。 */
        schedule_retry(false,
                       event->esp_err,
                       event->connect_return_code,
                       "mqtt_error");
        return false;
    case GW_MQTT_CLIENT_EVENT_PUBLISHED:
        complete_outbound_publish(event);
        return true;
    case GW_MQTT_CLIENT_EVENT_UNSUBSCRIBED:
    default:
        return true;
    }
}

/** @brief 排空回调事件队列；遇到 client 切换时停止本轮业务处理。 */
static void process_event_queue(void)
{
    business_mqtt_event_t event;
    while (xQueueReceive(s_business.event_queue, &event, 0) == pdTRUE) {
        if (!process_mqtt_event(&event)) {
            break;
        }
    }
}

/**
 * @brief 排空独立 DATA 队列。
 *
 * DATA 先于控制队列处理，可确保同一连接中已经到达本机的消息不会因为随后到达的断线
 * 事件而被提前销毁。source client 与 generation 校验仍在统一事件处理函数中完成。
 */
static void process_data_queue(void)
{
    business_mqtt_event_t event;
    while (xQueueReceive(s_business.data_queue, &event, 0) == pdTRUE) {
        (void)process_mqtt_event(&event);
    }
}

/**
 * @brief 用底层状态快照兜底回调队列丢事件。
 *
 * CONNECTED 丢失时补订阅；断开、ERROR 和 AUTH_FAILED 丢失时仍能进入对应重试。SUBACK
 * 无法从快照恢复，因此 SUBSCRIBING 另有 15 秒截止时间，超时后重建会话。
 */
static void reconcile_client_status(void)
{
    if (s_business.client == NULL) {
        return;
    }

    gw_mqtt_client_status_snapshot_t status = {0};
    esp_err_t err = gw_mqtt_client_get_status(s_business.client, &status);
    if (err != ESP_OK) {
        schedule_retry(false, err, 0, "mqtt_status_failed");
        return;
    }
    set_last_error(status.last_esp_err, status.last_connect_return_code, NULL);

    if (status.status == GW_MQTT_CLIENT_STATUS_AUTH_FAILED) {
        schedule_retry(true,
                       status.last_esp_err,
                       status.last_connect_return_code,
                       "auth_failed_status");
        return;
    }
    if (status.status == GW_MQTT_CLIENT_STATUS_DISCONNECTED ||
        status.status == GW_MQTT_CLIENT_STATUS_ERROR) {
        schedule_retry(false,
                       status.last_esp_err,
                       status.last_connect_return_code,
                       "disconnected_status");
        return;
    }
    if (status.status == GW_MQTT_CLIENT_STATUS_CONNECTED &&
        s_business.state == GW_BUSINESS_MQTT_STATE_CONNECTING) {
        portENTER_CRITICAL(&s_business.lock);
        ++s_business.connected_count;
        portEXIT_CRITICAL(&s_business.lock);
        err = begin_subscribe();
        if (err != ESP_OK) {
            schedule_retry(false, err, 0, "subscribe_submit_failed");
        }
    }
}

/**
 * @brief 提交当前 FIFO 队首，并等待 QoS1 PUBACK 后再转移所有权。
 *
 * 同一时刻只允许一条业务上行在途。这样断线销毁 ESP-MQTT outbox 时，业务 FIFO 仍持有
 * 原始 payload，可在新会话中重发。PUBACK 超时会主动重建会话，避免队首永久卡住。
 */
static void flush_outbound(void)
{
    if (s_business.client == NULL ||
        s_business.state != GW_BUSINESS_MQTT_STATE_ONLINE) {
        return;
    }
    if (xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_business.outbound_count == 0U) {
        xSemaphoreGive(s_business.outbound_mutex);
        return;
    }
    if (s_business.outbound_inflight) {
        bool timed_out = tick_reached(xTaskGetTickCount(),
                                      s_business.outbound_deadline_tick);
        xSemaphoreGive(s_business.outbound_mutex);
        if (timed_out) {
            schedule_retry(false, ESP_ERR_TIMEOUT, 0, "publish_ack_timeout");
        }
        return;
    }

    business_mqtt_outbound_t *item =
        &s_business.outbound[s_business.outbound_head];
    int msg_id = -1;
    esp_err_t err = gw_mqtt_client_publish_with_msg_id(s_business.client,
                                                       s_business.up_topic,
                                                       item->payload,
                                                       item->payload_len,
                                                       1,
                                                       false,
                                                       &msg_id);
    if (err != ESP_OK) {
        xSemaphoreGive(s_business.outbound_mutex);
        schedule_retry(false, err, 0, "publish_failed");
        return;
    }

    s_business.outbound_inflight = true;
    s_business.outbound_generation = s_business.client_generation;
    s_business.outbound_msg_id = msg_id;
    s_business.outbound_deadline_tick =
        xTaskGetTickCount() + timeout_ticks(GW_BUSINESS_MQTT_PUBLISH_TIMEOUT_MS);
    xSemaphoreGive(s_business.outbound_mutex);
}

/** @brief 清空上行 FIFO，仅用于显式 stop。 */
static void clear_outbound(void)
{
    if (xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) == pdTRUE) {
        memset(s_business.outbound,
               0,
               GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY * sizeof(*s_business.outbound));
        s_business.outbound_head = 0U;
        s_business.outbound_count = 0U;
        s_business.outbound_inflight = false;
        s_business.outbound_generation = 0U;
        s_business.outbound_msg_id = -1;
        s_business.outbound_deadline_tick = 0;
        xSemaphoreGive(s_business.outbound_mutex);
    }
}

/** @brief 完成显式 stop 的资源和敏感信息清理。 */
static void handle_explicit_stop(void)
{
    destroy_client();
    clear_credentials();
    clear_retry();
    clear_outbound();
    xQueueReset(s_business.event_queue);
    xQueueReset(s_business.data_queue);
    xQueueReset(s_business.downlink_queue);

    portENTER_CRITICAL(&s_business.lock);
    s_business.run_requested = false;
    s_business.stop_requested = false;
    s_business.network_ready_seen = false;
    s_business.network_settle_deadline_tick = 0;
    s_business.subscribe_deadline_tick = 0;
    portEXIT_CRITICAL(&s_business.lock);
    set_state(GW_BUSINESS_MQTT_STATE_STOPPED, "stopped");
    xEventGroupSetBits(s_business.control_events, GW_BUSINESS_MQTT_STOPPED_BIT);
}

/** @brief worker 的一次非阻塞状态推进。 */
static void worker_step(void)
{
    bool run_requested = false;
    bool stop_requested = false;
    read_control_flags(&run_requested, &stop_requested);
    if (stop_requested) {
        handle_explicit_stop();
        return;
    }
    if (!run_requested) {
        return;
    }

    if (s_business.state == GW_BUSINESS_MQTT_STATE_STOPPED) {
        set_state(GW_BUSINESS_MQTT_STATE_WAIT_NETWORK, "wait_network");
    }

    process_data_queue();
    process_event_queue();
    read_control_flags(&run_requested, &stop_requested);
    if (stop_requested || !run_requested) {
        return;
    }

    if (!network_ready()) {
        s_business.network_ready_seen = false;
        s_business.network_settle_deadline_tick = 0;
        if (s_business.client != NULL) {
            destroy_client();
        }
        set_state(GW_BUSINESS_MQTT_STATE_WAIT_NETWORK, "wait_4g");
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (!s_business.network_ready_seen) {
        /*
         * NET_READY 后先给 supervisor 一个短窗口创建 dev_mqtt。HTTPS/TLS 与 MQTT
         * task 同时申请内部 RAM 会造成连续块竞争，业务通道稍后启动但仍保持独立。
         */
        s_business.network_ready_seen = true;
        s_business.network_settle_deadline_tick =
            now + timeout_ticks(GW_BUSINESS_MQTT_NETWORK_SETTLE_MS);
        set_state(GW_BUSINESS_MQTT_STATE_WAIT_NETWORK, "network_settle");
        return;
    }
    if (!tick_reached(now, s_business.network_settle_deadline_tick)) {
        return;
    }

    bool retry_pending;
    TickType_t retry_tick;
    portENTER_CRITICAL(&s_business.lock);
    retry_pending = s_business.retry_pending;
    retry_tick = s_business.next_retry_tick;
    portEXIT_CRITICAL(&s_business.lock);
    if (retry_pending && !tick_reached(now, retry_tick)) {
        set_state(GW_BUSINESS_MQTT_STATE_RETRY_WAIT, "retry_wait");
        return;
    }
    if (retry_pending) {
        clear_retry();
    }

    if (!s_business.credentials_valid) {
        esp_err_t err = register_credentials();
        if (err != ESP_OK) {
            schedule_retry(false, err, 0, "register_retry_wait");
            return;
        }
    }

    if (s_business.client == NULL) {
        esp_err_t err = begin_connect();
        if (err != ESP_OK) {
            schedule_retry(false, err, 0, "connect_start_failed");
        }
        return;
    }

    reconcile_client_status();
    if (s_business.client == NULL) {
        return;
    }
    if (s_business.state == GW_BUSINESS_MQTT_STATE_CONNECTING &&
        tick_reached(xTaskGetTickCount(), s_business.connect_deadline_tick)) {
        schedule_retry(false, ESP_ERR_TIMEOUT, 0, "connect_timeout");
        return;
    }
    if (s_business.state == GW_BUSINESS_MQTT_STATE_SUBSCRIBING &&
        tick_reached(xTaskGetTickCount(), s_business.subscribe_deadline_tick)) {
        schedule_retry(false, ESP_ERR_TIMEOUT, 0, "subscribe_timeout");
        return;
    }
    if (s_business.state == GW_BUSINESS_MQTT_STATE_ONLINE) {
        flush_outbound();
    }
}

/** @brief 常驻静态 worker；显式 stop 后休眠，后续允许再次 start。 */
static void business_mqtt_worker(void *arg)
{
    (void)arg;
    for (;;) {
        worker_step();

        bool run_requested = false;
        bool stop_requested = false;
        read_control_flags(&run_requested, &stop_requested);
        uint32_t wait_ms = (!run_requested && !stop_requested) ?
                           1000U : GW_BUSINESS_MQTT_WORKER_POLL_MS;
        (void)ulTaskNotifyTake(pdTRUE, timeout_ticks(wait_ms));
    }
}

/** @brief 复制并验证初始化配置和业务设备身份。 */
static esp_err_t initialize_config(const gw_business_mqtt_config_t *config)
{
    esp_err_t err = copy_default_string(
        s_business.config.register_url,
        sizeof(s_business.config.register_url),
        config != NULL ? config->register_url : NULL,
        GW_BUSINESS_MQTT_REGISTER_DEFAULT_URL);
    if (err == ESP_OK) {
        err = copy_default_string(s_business.config.register_group_id,
                                  sizeof(s_business.config.register_group_id),
                                  config != NULL ? config->register_group_id : NULL,
                                  GW_BUSINESS_MQTT_REGISTER_DEFAULT_GROUP_ID);
    }
    if (err == ESP_OK) {
        err = copy_default_string(s_business.config.topic_group_id,
                                  sizeof(s_business.config.topic_group_id),
                                  config != NULL ? config->topic_group_id : NULL,
                                  GW_BUSINESS_MQTT_DEFAULT_TOPIC_GROUP_ID);
    }
    if (err != ESP_OK) {
        return err;
    }

    s_business.config.register_timeout_ms =
        config != NULL && config->register_timeout_ms != 0U ?
        config->register_timeout_ms : GW_BUSINESS_MQTT_REGISTER_DEFAULT_TIMEOUT_MS;
    s_business.config.register_max_response_bytes =
        config != NULL && config->register_max_response_bytes != 0U ?
        config->register_max_response_bytes : GW_BUSINESS_MQTT_REGISTER_DEFAULT_MAX_BYTES;
    if (s_business.config.register_timeout_ms < GW_BUSINESS_MQTT_REGISTER_MIN_TIMEOUT_MS ||
        s_business.config.register_timeout_ms > GW_BUSINESS_MQTT_REGISTER_MAX_TIMEOUT_MS ||
        s_business.config.register_max_response_bytes <
            GW_BUSINESS_MQTT_REGISTER_MIN_RESPONSE_BYTES ||
        s_business.config.register_max_response_bytes >
            GW_BUSINESS_MQTT_REGISTER_MAX_RESPONSE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    s_business.config.retry_ms = config != NULL && config->retry_ms != 0U ?
                                 config->retry_ms : GW_BUSINESS_MQTT_DEFAULT_RETRY_MS;
    s_business.config.auth_retry_ms = config != NULL && config->auth_retry_ms != 0U ?
                                      config->auth_retry_ms :
                                      GW_BUSINESS_MQTT_DEFAULT_AUTH_RETRY_MS;
    s_business.config.mqtt_task_stack_bytes =
        config != NULL && config->mqtt_task_stack_bytes != 0U ?
        config->mqtt_task_stack_bytes : GW_BUSINESS_MQTT_TASK_STACK_BYTES;
    s_business.config.allow_untrusted_https = config != NULL ?
                                                config->allow_untrusted_https : true;
    if (s_business.config.retry_ms < GW_BUSINESS_MQTT_RETRY_MIN_MS ||
        s_business.config.retry_ms > GW_BUSINESS_MQTT_RETRY_MAX_MS ||
        s_business.config.auth_retry_ms < GW_BUSINESS_MQTT_AUTH_RETRY_MIN_MS ||
        s_business.config.auth_retry_ms > GW_BUSINESS_MQTT_AUTH_RETRY_MAX_MS ||
        s_business.config.mqtt_task_stack_bytes <
            GW_BUSINESS_MQTT_MQTT_STACK_MIN_BYTES ||
        s_business.config.mqtt_task_stack_bytes >
            GW_BUSINESS_MQTT_MQTT_STACK_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config != NULL && config->business_device_id != NULL &&
        config->business_device_id[0] != '\0') {
        err = copy_required_string(s_business.business_device_id,
                                   sizeof(s_business.business_device_id),
                                   config->business_device_id);
    } else {
        gw_storage_identity_t identity = {0};
        err = gw_storage_get_identity(&identity);
        if (err != ESP_OK) {
            return err;
        }
        if (!valid_mac_text(identity.mac)) {
            return ESP_ERR_INVALID_STATE;
        }
        err = strlcpy(s_business.business_device_id,
                      identity.mac,
                      sizeof(s_business.business_device_id)) <
                      sizeof(s_business.business_device_id) ?
              ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        return err;
    }

    int written = snprintf(s_business.up_topic,
                           sizeof(s_business.up_topic),
                           "/%s/%s/up",
                           s_business.config.topic_group_id,
                           s_business.business_device_id);
    if (written <= 0 || (size_t)written >= sizeof(s_business.up_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(s_business.down_topic,
                       sizeof(s_business.down_topic),
                       "/%s/%s/down",
                       s_business.config.topic_group_id,
                       s_business.business_device_id);
    return written > 0 && (size_t)written < sizeof(s_business.down_topic) ?
           ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t gw_business_mqtt_init(const gw_business_mqtt_config_t *config)
{
    portENTER_CRITICAL(&s_business.lock);
    bool initialized = s_business.initialized;
    portEXIT_CRITICAL(&s_business.lock);
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = initialize_config(config);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * 离线 FIFO 及三个 FreeRTOS 队列都只在普通任务上下文访问，统一放入 PSRAM，
     * 为 dev_mqtt 与 business_mqtt 的 8KB 内部任务栈保留足够大的连续内存块。
     */
    s_business.outbound = (business_mqtt_outbound_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY,
        sizeof(*s_business.outbound),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.event_queue_storage = (uint8_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_EVENT_QUEUE_CAPACITY,
        sizeof(business_mqtt_event_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.data_queue_storage = (uint8_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_DATA_QUEUE_CAPACITY,
        sizeof(business_mqtt_event_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.downlink_queue_storage = (uint8_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY,
        sizeof(gw_business_mqtt_message_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_business.outbound == NULL || s_business.event_queue_storage == NULL ||
        s_business.data_queue_storage == NULL ||
        s_business.downlink_queue_storage == NULL) {
        free_psram_storage();
        return ESP_ERR_NO_MEM;
    }

    s_business.outbound_mutex =
        xSemaphoreCreateMutexStatic(&s_business.outbound_mutex_buffer);
    s_business.event_queue =
        xQueueCreateStatic(GW_BUSINESS_MQTT_EVENT_QUEUE_CAPACITY,
                           sizeof(business_mqtt_event_t),
                           s_business.event_queue_storage,
                           &s_business.event_queue_buffer);
    s_business.data_queue =
        xQueueCreateStatic(GW_BUSINESS_MQTT_DATA_QUEUE_CAPACITY,
                           sizeof(business_mqtt_event_t),
                           s_business.data_queue_storage,
                           &s_business.data_queue_buffer);
    s_business.downlink_queue =
        xQueueCreateStatic(GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY,
                           sizeof(gw_business_mqtt_message_t),
                           s_business.downlink_queue_storage,
                           &s_business.downlink_queue_buffer);
    s_business.control_events =
        xEventGroupCreateStatic(&s_business.control_events_buffer);
    if (s_business.outbound_mutex == NULL || s_business.event_queue == NULL ||
        s_business.data_queue == NULL ||
        s_business.downlink_queue == NULL || s_business.control_events == NULL) {
        free_psram_storage();
        return ESP_ERR_NO_MEM;
    }

    s_business.state = GW_BUSINESS_MQTT_STATE_STOPPED;
    s_business.subscribe_msg_id = -1;
    s_business.outbound_msg_id = -1;
    strlcpy(s_business.last_reason, "initialized", sizeof(s_business.last_reason));
    s_business.worker_task =
        xTaskCreateStatic(business_mqtt_worker,
                          "business_mqtt",
                          GW_BUSINESS_MQTT_WORKER_STACK_DEPTH,
                          NULL,
                          GW_BUSINESS_MQTT_WORKER_PRIORITY,
                          s_business.worker_task_stack,
                          &s_business.worker_task_buffer);
    if (s_business.worker_task == NULL) {
        free_psram_storage();
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_business.lock);
    s_business.initialized = true;
    portEXIT_CRITICAL(&s_business.lock);
    xEventGroupSetBits(s_business.control_events, GW_BUSINESS_MQTT_STOPPED_BIT);
    ESP_LOGI(TAG,
             "business MQTT initialized: device_id=%s up=%s down=%s",
             s_business.business_device_id,
             s_business.up_topic,
             s_business.down_topic);
    return ESP_OK;
}

esp_err_t gw_business_mqtt_start(void)
{
    portENTER_CRITICAL(&s_business.lock);
    if (!s_business.initialized) {
        portEXIT_CRITICAL(&s_business.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_business.stop_requested) {
        portEXIT_CRITICAL(&s_business.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_business.run_requested = true;
    portEXIT_CRITICAL(&s_business.lock);

    xEventGroupClearBits(s_business.control_events, GW_BUSINESS_MQTT_STOPPED_BIT);
    xTaskNotifyGive(s_business.worker_task);
    return ESP_OK;
}

esp_err_t gw_business_mqtt_stop(uint32_t timeout_ms)
{
    portENTER_CRITICAL(&s_business.lock);
    if (!s_business.initialized) {
        portEXIT_CRITICAL(&s_business.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_business.state == GW_BUSINESS_MQTT_STATE_STOPPED &&
        !s_business.run_requested && !s_business.stop_requested) {
        portEXIT_CRITICAL(&s_business.lock);
        return ESP_OK;
    }
    s_business.run_requested = false;
    s_business.stop_requested = true;
    portEXIT_CRITICAL(&s_business.lock);

    xTaskNotifyGive(s_business.worker_task);
    EventBits_t bits = xEventGroupWaitBits(s_business.control_events,
                                           GW_BUSINESS_MQTT_STOPPED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           timeout_ticks(timeout_ms));
    return (bits & GW_BUSINESS_MQTT_STOPPED_BIT) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t gw_business_mqtt_send(const void *payload, size_t payload_len)
{
    if ((payload == NULL && payload_len > 0U) ||
        payload_len > GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN) {
        return payload_len > GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN ?
               ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }
    if (s_business.outbound == NULL || s_business.outbound_mutex == NULL ||
        xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_business.lock);
    bool can_send = s_business.initialized && s_business.run_requested &&
                    !s_business.stop_requested;
    portEXIT_CRITICAL(&s_business.lock);
    if (!can_send) {
        xSemaphoreGive(s_business.outbound_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_business.outbound_count == GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY) {
        const char *drop_reason = "outbound_queue_drop_oldest";
        if (s_business.outbound_inflight) {
            /*
             * QoS1 在途队首仍由业务层持有，不能被满队列策略覆盖。删除它后面最旧的等待
             * 条目并压紧逻辑环形队列，确保迟到 PUBACK 只会完成原队首。
             */
            for (size_t offset = 1U;
                 offset + 1U < s_business.outbound_count;
                 ++offset) {
                size_t dst = (s_business.outbound_head + offset) %
                             GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY;
                size_t src = (s_business.outbound_head + offset + 1U) %
                             GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY;
                s_business.outbound[dst] = s_business.outbound[src];
            }
            size_t last = (s_business.outbound_head +
                           s_business.outbound_count - 1U) %
                          GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY;
            memset(&s_business.outbound[last], 0, sizeof(s_business.outbound[last]));
            drop_reason = "outbound_queue_drop_oldest_waiting";
        } else {
            s_business.outbound_head =
                (s_business.outbound_head + 1U) %
                GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY;
        }
        --s_business.outbound_count;
        portENTER_CRITICAL(&s_business.lock);
        ++s_business.outbound_dropped_count;
        strlcpy(s_business.last_reason,
                drop_reason,
                sizeof(s_business.last_reason));
        portEXIT_CRITICAL(&s_business.lock);
    }

    size_t tail = (s_business.outbound_head + s_business.outbound_count) %
                  GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY;
    business_mqtt_outbound_t *item = &s_business.outbound[tail];
    /* 槽位可能来自“满队列丢最旧”，先擦除旧二进制数据再复用。 */
    memset(item, 0, sizeof(*item));
    item->payload_len = payload_len;
    if (payload_len > 0U) {
        memcpy(item->payload, payload, payload_len);
    }
    ++s_business.outbound_count;
    xSemaphoreGive(s_business.outbound_mutex);

    xTaskNotifyGive(s_business.worker_task);
    return ESP_OK;
}

esp_err_t gw_business_mqtt_recv(gw_business_mqtt_message_t *out_message,
                                uint32_t timeout_ms)
{
    if (out_message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_message, 0, sizeof(*out_message));

    portENTER_CRITICAL(&s_business.lock);
    bool initialized = s_business.initialized;
    portEXIT_CRITICAL(&s_business.lock);
    if (!initialized || s_business.downlink_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueReceive(s_business.downlink_queue,
                         out_message,
                         timeout_ticks(timeout_ms)) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t gw_business_mqtt_get_status(gw_business_mqtt_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));

    bool retry_pending;
    TickType_t retry_tick;
    portENTER_CRITICAL(&s_business.lock);
    if (!s_business.initialized) {
        portEXIT_CRITICAL(&s_business.lock);
        return ESP_ERR_INVALID_STATE;
    }
    out_status->initialized = true;
    out_status->start_requested = s_business.run_requested;
    out_status->credentials_valid = s_business.credentials_valid;
    out_status->online = s_business.online;
    out_status->state = s_business.state;
    out_status->register_attempts = s_business.register_attempts;
    out_status->connect_attempts = s_business.connect_attempts;
    out_status->connected_count = s_business.connected_count;
    out_status->subscribe_count = s_business.subscribe_count;
    out_status->published_count = s_business.published_count;
    out_status->received_count = s_business.received_count;
    out_status->outbound_dropped_count = s_business.outbound_dropped_count;
    out_status->downlink_dropped_count = s_business.downlink_dropped_count;
    out_status->event_dropped_count = s_business.event_dropped_count;
    out_status->last_esp_err = s_business.last_esp_err;
    out_status->last_connect_return_code = s_business.last_connect_return_code;
    strlcpy(out_status->business_device_id,
            s_business.business_device_id,
            sizeof(out_status->business_device_id));
    strlcpy(out_status->up_topic, s_business.up_topic, sizeof(out_status->up_topic));
    strlcpy(out_status->down_topic, s_business.down_topic, sizeof(out_status->down_topic));
    strlcpy(out_status->last_reason,
            s_business.last_reason,
            sizeof(out_status->last_reason));
    retry_pending = s_business.retry_pending;
    retry_tick = s_business.next_retry_tick;
    portEXIT_CRITICAL(&s_business.lock);

    if (xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) == pdTRUE) {
        out_status->outbound_pending = s_business.outbound_count;
        xSemaphoreGive(s_business.outbound_mutex);
    }
    out_status->downlink_pending = (size_t)uxQueueMessagesWaiting(s_business.downlink_queue);

    TickType_t now = xTaskGetTickCount();
    if (retry_pending && !tick_reached(now, retry_tick)) {
        TickType_t remaining = retry_tick - now;
        uint64_t remaining_ms = ((uint64_t)remaining * 1000ULL) / configTICK_RATE_HZ;
        out_status->retry_remaining_ms = remaining_ms > UINT32_MAX ?
                                         UINT32_MAX : (uint32_t)remaining_ms;
    }
    return ESP_OK;
}

const char *gw_business_mqtt_state_to_string(gw_business_mqtt_state_t state)
{
    switch (state) {
    case GW_BUSINESS_MQTT_STATE_STOPPED:
        return "stopped";
    case GW_BUSINESS_MQTT_STATE_WAIT_NETWORK:
        return "wait_network";
    case GW_BUSINESS_MQTT_STATE_REGISTERING:
        return "registering";
    case GW_BUSINESS_MQTT_STATE_CONNECTING:
        return "connecting";
    case GW_BUSINESS_MQTT_STATE_SUBSCRIBING:
        return "subscribing";
    case GW_BUSINESS_MQTT_STATE_ONLINE:
        return "online";
    case GW_BUSINESS_MQTT_STATE_RETRY_WAIT:
        return "retry_wait";
    default:
        return "unknown";
    }
}
