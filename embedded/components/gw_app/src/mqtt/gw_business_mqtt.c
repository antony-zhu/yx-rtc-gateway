#include "gw_business_mqtt.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
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
#define GW_BUSINESS_MQTT_REGISTER_RETRY_MS           10000U
#define GW_BUSINESS_MQTT_OFFLINE_REBOOT_AFTER_MS    (30U * 60U * 1000U)
#define GW_BUSINESS_MQTT_RETRY_MIN_MS               500U
#define GW_BUSINESS_MQTT_RETRY_MAX_MS               300000U
#define GW_BUSINESS_MQTT_AUTH_RETRY_MIN_MS          5000U
#define GW_BUSINESS_MQTT_AUTH_RETRY_MAX_MS          3600000U
#define GW_BUSINESS_MQTT_MQTT_STACK_MIN_BYTES       (4U * 1024U)
#define GW_BUSINESS_MQTT_MQTT_STACK_MAX_BYTES       (32U * 1024U)
#define GW_BUSINESS_MQTT_STOPPED_BIT                BIT0
#define GW_BUSINESS_MQTT_NAME                       "dev_mqtt"
#define GW_BUSINESS_MQTT_GATEWAY_QUEUE_CAPACITY     32U
#define GW_BUSINESS_MQTT_GATEWAY_INTERVAL_US        1000000LL
#define GW_BUSINESS_MQTT_GATEWAY_STACK_BYTES        (4U * 1024U)
#define GW_BUSINESS_MQTT_GATEWAY_STACK_DEPTH        \
    (GW_BUSINESS_MQTT_GATEWAY_STACK_BYTES / sizeof(StackType_t))

/* These QoS values are part of the fixed Lua-compatible business contract. */
enum {
    GW_BUSINESS_MQTT_PUBLISH_QOS = 0,
    GW_BUSINESS_MQTT_SUBSCRIBE_QOS = 1,
};

/**
 * @brief 回调到 worker 的事件描述。
 *
 * 事件本体和 DATA/topic 缓冲均在 PSRAM 中分配；队列只传递指针，避免 8 KiB 管理报文
 * 或大分片被复制到 MQTT/业务任务栈。所有权在入队后归 worker，丢队时由回调释放。
 */
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
    bool retained;
    char *topic;
    uint8_t *data;
} business_mqtt_event_t;

/** @brief 上行 FIFO 条目；Payload 由 PSRAM 持有，发布和移除由 outbound_mutex 保护。 */
typedef struct {
    size_t payload_len;
    uint8_t *payload;
    uint8_t topic_kind;
} business_mqtt_outbound_t;

/** @brief 新网关协议独立 FIFO 条目；完成回调只由独立发送任务调用。 */
typedef struct {
    uint8_t *payload;
    size_t payload_len;
    uint32_t generation;
    gw_business_mqtt_gateway_done_fn done;
    void *context;
} business_mqtt_gateway_outbound_t;

typedef enum {
    BUSINESS_MQTT_TOPIC_UP_RAW = 0,
    BUSINESS_MQTT_TOPIC_UP_CONFIG = 1,
} business_mqtt_topic_kind_t;

typedef enum {
    BUSINESS_MQTT_TOPIC_DOWN_UNKNOWN = 0,
    BUSINESS_MQTT_TOPIC_DOWN_RAW = 1,
    BUSINESS_MQTT_TOPIC_DOWN_CONFIG = 2,
    BUSINESS_MQTT_TOPIC_DOWN_GATEWAY = 3,
} business_mqtt_down_topic_kind_t;

/** @brief MQTT DATA 多片重组状态，只由业务 worker 访问；大缓冲在 PSRAM。 */
typedef struct {
    bool active;
    uint32_t generation;
    int msg_id;
    business_mqtt_down_topic_kind_t topic_kind;
    bool retained;
    size_t total_len;
    size_t received_len;
    char *topic;
    uint8_t *payload;
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
 * client、凭据、重组器和状态机只允许 worker 访问。回调按控制事件和 DATA 分队列投递；
 * 队列元素均为 PSRAM 对象指针。外部发送者只在 outbound_mutex 保护下写上行环形队列。
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
    bool offline_since_valid;
    TickType_t offline_since_tick;
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
    char up_config_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    char down_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    char down_config_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    char gateway_up_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    char gateway_down_topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
    bool gateway_online;
    int gateway_subscribe_msg_id;
    TickType_t gateway_subscribe_deadline_tick;
    business_mqtt_config_copy_t config;

    gw_mqtt_client_t *client;
    uint32_t client_generation;
    int subscribe_msg_id;
    uint8_t subscribe_index;
    business_mqtt_callback_binding_t callback_binding;
    gw_business_mqtt_credentials_t credentials;
    business_mqtt_fragment_t fragment;

    business_mqtt_outbound_t *outbound;
    size_t outbound_head;
    size_t outbound_count;
    SemaphoreHandle_t outbound_mutex;
    StaticSemaphore_t outbound_mutex_buffer;

    business_mqtt_gateway_outbound_t *gateway_outbound;
    size_t gateway_outbound_head;
    size_t gateway_outbound_count;
    SemaphoreHandle_t gateway_outbound_mutex;
    StaticSemaphore_t gateway_outbound_mutex_buffer;
    int64_t gateway_last_attempt_us;
    bool gateway_attempted;
    TaskHandle_t gateway_sender_task;
    StaticTask_t gateway_sender_task_buffer;
    StackType_t gateway_sender_task_stack[GW_BUSINESS_MQTT_GATEWAY_STACK_DEPTH];

    QueueHandle_t event_queue;
    StaticQueue_t event_queue_buffer;
    uint8_t *event_queue_storage;

    QueueHandle_t data_queue;
    StaticQueue_t data_queue_buffer;
    uint8_t *data_queue_storage;

    QueueHandle_t downlink_queue;
    StaticQueue_t downlink_queue_buffer;
    uint8_t *downlink_queue_storage;

    QueueHandle_t management_queue;
    StaticQueue_t management_queue_buffer;
    uint8_t *management_queue_storage;

    QueueHandle_t gateway_queue;
    StaticQueue_t gateway_queue_buffer;
    uint8_t *gateway_queue_storage;

    /* 保护 publish_wait 与 worker 销毁/切换底层 client 的竞态。 */
    SemaphoreHandle_t client_mutex;
    StaticSemaphore_t client_mutex_buffer;

    EventGroupHandle_t control_events;
    StaticEventGroup_t control_events_buffer;
    TaskHandle_t worker_task;
    StaticTask_t worker_task_buffer;
    StackType_t worker_task_stack[GW_BUSINESS_MQTT_WORKER_STACK_DEPTH];
} business_mqtt_context_t;

static business_mqtt_context_t s_business = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/* QoS0 只保留在线发送窗口；断线时丢弃尚未提交的业务 FIFO。 */
static void clear_outbound(void);

static void free_owned_message(gw_business_mqtt_owned_message_t *message);
static void free_event(business_mqtt_event_t *event);

/** @brief 严格从 PSRAM 申请可异步持有的字节缓冲，并保留一个 C 字符串终止字节。 */
static void *psram_calloc_bytes(size_t count, size_t size)
{
    if (count != 0U && size > SIZE_MAX / count) {
        return NULL;
    }
    size_t bytes = count * size;
    return heap_caps_calloc(bytes == 0U ? 1U : bytes,
                            1U,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/** @brief 复制任意 MQTT 字节片段到 PSRAM，并额外补零，避免协议层误用 strlen。 */
static void *psram_copy_bytes(const void *data, size_t length, bool add_nul)
{
    if (length > SIZE_MAX - (add_nul ? 1U : 0U) ||
        (data == NULL && length > 0U)) {
        return NULL;
    }
    size_t alloc_len = length + (add_nul ? 1U : 0U);
    uint8_t *copy = (uint8_t *)psram_calloc_bytes(alloc_len, 1U);
    if (copy == NULL) {
        return NULL;
    }
    if (length > 0U) {
        memcpy(copy, data, length);
    }
    return copy;
}

/** @brief 释放 outbound FIFO 中每个动态 Payload。 */
static void free_outbound_items(void)
{
    if (s_business.outbound == NULL) {
        return;
    }
    for (size_t i = 0U; i < GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY; ++i) {
        heap_caps_free(s_business.outbound[i].payload);
        s_business.outbound[i].payload = NULL;
        s_business.outbound[i].payload_len = 0U;
        s_business.outbound[i].topic_kind = BUSINESS_MQTT_TOPIC_UP_RAW;
    }
}

/** @brief 释放初始化阶段申请的 PSRAM；仅允许在 worker 创建前调用。 */
static void free_psram_storage(void)
{
    free_outbound_items();
    heap_caps_free(s_business.downlink_queue_storage);
    heap_caps_free(s_business.management_queue_storage);
    heap_caps_free(s_business.data_queue_storage);
    heap_caps_free(s_business.event_queue_storage);
    heap_caps_free(s_business.outbound);
    heap_caps_free(s_business.gateway_outbound);
    heap_caps_free(s_business.gateway_queue_storage);
    s_business.downlink_queue_storage = NULL;
    s_business.management_queue_storage = NULL;
    s_business.data_queue_storage = NULL;
    s_business.event_queue_storage = NULL;
    s_business.outbound = NULL;
    s_business.gateway_outbound = NULL;
    s_business.gateway_queue_storage = NULL;
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

/** @brief 将 eFuse MAC 复制为固定的大写 12 位设备号。 */
static esp_err_t copy_mac_upper(char *out, size_t out_size, const char *mac)
{
    if (out == NULL || out_size < 13U || !valid_mac_text(mac)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < 12U; ++i) {
        out[i] = (char)toupper((unsigned char)mac[i]);
    }
    out[12] = '\0';
    return ESP_OK;
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
    if (online) {
        s_business.offline_since_valid = false;
        s_business.offline_since_tick = 0;
    } else if (!s_business.offline_since_valid) {
        s_business.offline_since_valid = true;
        s_business.offline_since_tick = xTaskGetTickCount();
    }
    portEXIT_CRITICAL(&s_business.lock);

    /*
     * 当前量产运行时只启用这一条统一 dev_mqtt，因此公共状态应反映真实链路。
     * 登录/属性等管理报文由兼容 facade 在 ONLINE 边沿触发。
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

/** @brief 启动连续离线计时；启动早期尚未创建 client 时也要计时。 */
static void mark_offline_if_needed(void)
{
    portENTER_CRITICAL(&s_business.lock);
    if (!s_business.offline_since_valid) {
        s_business.offline_since_valid = true;
        s_business.offline_since_tick = xTaskGetTickCount();
    }
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 检查 Lua 基线要求的 30 分钟连续离线重启条件。 */
static void check_offline_watchdog(bool run_requested, bool stop_requested)
{
    if (!run_requested || stop_requested) {
        return;
    }

    TickType_t offline_since;
    bool offline_valid;
    portENTER_CRITICAL(&s_business.lock);
    offline_valid = s_business.offline_since_valid;
    offline_since = s_business.offline_since_tick;
    portEXIT_CRITICAL(&s_business.lock);
    if (!offline_valid ||
        !tick_reached(xTaskGetTickCount(),
                      offline_since + timeout_ticks(GW_BUSINESS_MQTT_OFFLINE_REBOOT_AFTER_MS))) {
        return;
    }

    ESP_LOGE(TAG,
             "MQTT offline for %u minutes; restarting device",
             GW_BUSINESS_MQTT_OFFLINE_REBOOT_AFTER_MS / 60000U);
    esp_restart();
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

/** @brief 释放一个动态消息的两个缓冲，但不释放调用方提供的结构体本身。 */
static void free_owned_message_buffers(gw_business_mqtt_owned_message_t *message)
{
    if (message == NULL) {
        return;
    }
    heap_caps_free(message->topic);
    heap_caps_free(message->payload);
    message->topic = NULL;
    message->payload = NULL;
    message->payload_len = 0U;
    message->retained = false;
}

/** @brief 释放队列内部动态消息对象（结构体和其 PSRAM 缓冲）。 */
static void free_owned_message(gw_business_mqtt_owned_message_t *message)
{
    if (message == NULL) {
        return;
    }
    free_owned_message_buffers(message);
    heap_caps_free(message);
}

/** @brief 释放一个由回调分配、尚未交给 worker 的事件。 */
static void free_event(business_mqtt_event_t *event)
{
    if (event == NULL) {
        return;
    }
    heap_caps_free(event->topic);
    heap_caps_free(event->data);
    heap_caps_free(event);
}

/** @brief 清除当前未完成的 MQTT DATA 重组，并释放 PSRAM 缓冲。 */
static void clear_fragment(void)
{
    heap_caps_free(s_business.fragment.topic);
    heap_caps_free(s_business.fragment.payload);
    memset(&s_business.fragment, 0, sizeof(s_business.fragment));
}

/** @brief 复制回调事件到 PSRAM；队列只保存返回的指针。 */
static business_mqtt_event_t *copy_callback_event(gw_mqtt_client_t *client,
                                                  const gw_mqtt_client_event_t *event,
                                                  uint32_t generation)
{
    business_mqtt_event_t *copy = (business_mqtt_event_t *)psram_calloc_bytes(
        1U, sizeof(*copy));
    if (copy == NULL) {
        return NULL;
    }

    copy->source_client = client;
    copy->generation = generation;
    copy->event_id = event->event_id;
    copy->msg_id = event->msg_id;
    copy->esp_err = event->esp_err;
    copy->connect_return_code = event->connect_return_code;
    copy->topic_len = event->topic_len;
    copy->data_len = event->data_len;
    copy->total_data_len = event->total_data_len;
    copy->current_data_offset = event->current_data_offset;
    copy->subscribe_failed = event->subscribe_failed;
    copy->retained = event->retained;

    if (event->topic_len > 0) {
        copy->topic = (char *)psram_copy_bytes(event->topic,
                                               (size_t)event->topic_len,
                                               true);
        if (copy->topic == NULL) {
            free_event(copy);
            return NULL;
        }
    }
    if (event->data_len > 0) {
        copy->data = (uint8_t *)psram_copy_bytes(event->data,
                                                 (size_t)event->data_len,
                                                 true);
        if (copy->data == NULL) {
            free_event(copy);
            return NULL;
        }
    } else {
        /* 保持空 Payload 可安全传给分片处理器；不额外占用大缓冲。 */
        copy->data = NULL;
    }
    return copy;
}

/**
 * @brief 底层 MQTT 回调。
 *
 * 回调只做边界检查、PSRAM 复制和指针入队，不在 MQTT task 栈上保留 Payload。来源
 * client/generation 随事件保存，worker 会丢弃旧实例迟到的事件。
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

    if (event->topic_len < 0 || event->data_len < 0 || event->current_data_offset < 0 ||
        (size_t)event->topic_len >= GW_BUSINESS_MQTT_TOPIC_MAX_LEN ||
        (event->topic_len > 0 && event->topic == NULL) ||
        (event->data_len > 0 && event->data == NULL)) {
        count_event_drop("callback_event_invalid");
        return;
    }
    if (event->event_id == GW_MQTT_CLIENT_EVENT_DATA &&
        (event->total_data_len < 0 ||
         (size_t)event->current_data_offset > (size_t)event->total_data_len ||
         (size_t)event->data_len >
             (size_t)event->total_data_len - (size_t)event->current_data_offset)) {
        count_event_drop("callback_payload_oversize");
        return;
    }

    business_mqtt_event_t *copied = copy_callback_event(client, event, binding->generation);
    if (copied == NULL) {
        count_event_drop("callback_event_no_mem");
        return;
    }

    /* DATA 与控制事件分离，避免高流量下行挤掉 SUBACK/断线事件。 */
    QueueHandle_t target_queue = event->event_id == GW_MQTT_CLIENT_EVENT_DATA ?
                                 s_business.data_queue : s_business.event_queue;
    TickType_t queue_wait = event->event_id == GW_MQTT_CLIENT_EVENT_DATA ?
                            timeout_ticks(20U) : 0;
    if (xQueueSend(target_queue, &copied, queue_wait) != pdTRUE) {
        free_event(copied);
        count_event_drop(event->event_id == GW_MQTT_CLIENT_EVENT_DATA ?
                         "callback_data_queue_full" : "callback_control_queue_full");
        return;
    }
    if (s_business.worker_task != NULL) {
        xTaskNotifyGive(s_business.worker_task);
    }
}

/** @brief 判断一段 topic 是否精确等于某个固定业务 Topic。 */
static bool topic_slice_equals(const char *topic, int topic_len, const char *expected)
{
    if (topic == NULL || expected == NULL || topic_len < 0) {
        return false;
    }
    size_t expected_len = strlen(expected);
    return (size_t)topic_len == expected_len &&
           memcmp(topic, expected, expected_len) == 0;
}

/** @brief 将下行 Topic 映射为原始控制路由或配置控制路由。 */
static business_mqtt_down_topic_kind_t down_topic_kind(const char *topic, int topic_len)
{
    if (topic_slice_equals(topic, topic_len, s_business.down_topic)) {
        return BUSINESS_MQTT_TOPIC_DOWN_RAW;
    }
    if (topic_slice_equals(topic, topic_len, s_business.down_config_topic)) {
        return BUSINESS_MQTT_TOPIC_DOWN_CONFIG;
    }
    if (topic_slice_equals(topic, topic_len, s_business.gateway_down_topic)) {
        return BUSINESS_MQTT_TOPIC_DOWN_GATEWAY;
    }
    return BUSINESS_MQTT_TOPIC_DOWN_UNKNOWN;
}

/** @brief Lua 原始通道使用二进制；JSON 对象/数组归入管理协议队列。 */
static bool payload_is_management(const uint8_t *payload, size_t payload_len)
{
    size_t index = 0U;
    while (index < payload_len) {
        uint8_t ch = payload[index];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        ++index;
    }
    return index < payload_len && (payload[index] == '{' || payload[index] == '[');
}

/** @brief 将完整下行按 JSON/二进制分流到对应指针队列；满时丢弃最旧一条。 */
static void enqueue_downlink_owned(char *topic,
                                   uint8_t *payload,
                                   size_t payload_len,
                                   business_mqtt_down_topic_kind_t topic_kind,
                                   bool retained)
{
    if (topic == NULL || (payload == NULL && payload_len > 0U)) {
        heap_caps_free(topic);
        heap_caps_free(payload);
        count_downlink_drop("downlink_owned_invalid");
        return;
    }

    gw_business_mqtt_owned_message_t *message =
        (gw_business_mqtt_owned_message_t *)psram_calloc_bytes(1U, sizeof(*message));
    if (message == NULL) {
        heap_caps_free(topic);
        heap_caps_free(payload);
        count_downlink_drop("downlink_message_no_mem");
        return;
    }
    message->topic = topic;
    message->payload = payload;
    message->payload_len = payload_len;
    message->retained = retained;

    QueueHandle_t queue = topic_kind == BUSINESS_MQTT_TOPIC_DOWN_GATEWAY ?
                          s_business.gateway_queue :
                          (topic_kind == BUSINESS_MQTT_TOPIC_DOWN_CONFIG ||
                           payload_is_management(payload, payload_len) ?
                           s_business.management_queue : s_business.downlink_queue);
    if (queue == NULL || xQueueSend(queue, &message, 0) != pdTRUE) {
        gw_business_mqtt_owned_message_t *discarded = NULL;
        if (queue != NULL && xQueueReceive(queue, &discarded, 0) == pdTRUE) {
            free_owned_message(discarded);
            count_downlink_drop("downlink_queue_drop_oldest");
        }
        if (queue == NULL || xQueueSend(queue, &message, 0) != pdTRUE) {
            free_owned_message(message);
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
    if (event == NULL || event->total_data_len < 0 || event->data_len < 0 ||
        event->current_data_offset < 0) {
        clear_fragment();
        count_downlink_drop("downlink_fragment_invalid");
        return;
    }

    size_t total_len = (size_t)event->total_data_len;
    size_t data_len = (size_t)event->data_len;
    size_t offset = (size_t)event->current_data_offset;
    if (offset > total_len ||
        data_len > total_len - offset || (data_len > 0U && event->data == NULL)) {
        clear_fragment();
        count_downlink_drop("downlink_fragment_size_invalid");
        return;
    }

    if (offset == 0U) {
        clear_fragment();
        business_mqtt_down_topic_kind_t topic_kind =
            down_topic_kind(event->topic, event->topic_len);
        if (topic_kind == BUSINESS_MQTT_TOPIC_DOWN_UNKNOWN) {
            count_downlink_drop("downlink_topic_mismatch");
            return;
        }

        if (topic_kind != BUSINESS_MQTT_TOPIC_DOWN_GATEWAY &&
            total_len > GW_BUSINESS_MQTT_MANAGEMENT_PAYLOAD_MAX_LEN) {
            count_downlink_drop("downlink_fragment_size_invalid");
            return;
        }

        if (total_len == data_len) {
            char *topic = (char *)psram_copy_bytes(event->topic,
                                                   (size_t)event->topic_len,
                                                   true);
            uint8_t *payload = (uint8_t *)psram_copy_bytes(event->data, data_len, true);
            enqueue_downlink_owned(topic, payload, data_len, topic_kind, event->retained);
            return;
        }

        s_business.fragment.topic = (char *)psram_copy_bytes(event->topic,
                                                             (size_t)event->topic_len,
                                                             true);
        s_business.fragment.payload = (uint8_t *)psram_calloc_bytes(total_len + 1U, 1U);
        if (s_business.fragment.topic == NULL || s_business.fragment.payload == NULL) {
            clear_fragment();
            count_downlink_drop("downlink_fragment_no_mem");
            return;
        }
        s_business.fragment.active = true;
        s_business.fragment.generation = event->generation;
        s_business.fragment.msg_id = event->msg_id;
        s_business.fragment.topic_kind = topic_kind;
        s_business.fragment.retained = event->retained;
        s_business.fragment.total_len = total_len;
        s_business.fragment.received_len = data_len;
        if (data_len > 0U) {
            memcpy(s_business.fragment.payload, event->data, data_len);
        }
        return;
    }

    business_mqtt_fragment_t *fragment = &s_business.fragment;
    business_mqtt_down_topic_kind_t continuation_kind =
        event->topic_len == 0 ? BUSINESS_MQTT_TOPIC_DOWN_UNKNOWN :
        down_topic_kind(event->topic, event->topic_len);
    bool topic_matches = event->topic_len == 0 ||
                         continuation_kind == s_business.fragment.topic_kind;
    if (!fragment->active || fragment->generation != event->generation ||
        fragment->msg_id != event->msg_id || fragment->total_len != total_len ||
        fragment->received_len != offset || !topic_matches || fragment->payload == NULL) {
        clear_fragment();
        count_downlink_drop("downlink_fragment_sequence_invalid");
        return;
    }

    if (data_len > 0U) {
        memcpy(fragment->payload + offset, event->data, data_len);
    }
    fragment->received_len += data_len;
    if (fragment->received_len == fragment->total_len) {
        char *topic = fragment->topic;
        uint8_t *payload = fragment->payload;
        business_mqtt_down_topic_kind_t topic_kind = fragment->topic_kind;
        bool retained = fragment->retained;
        fragment->topic = NULL;
        fragment->payload = NULL;
        memset(fragment, 0, sizeof(*fragment));
        enqueue_downlink_owned(topic, payload, total_len, topic_kind, retained);
    }
}

/** @brief 销毁当前底层 client；必须由 worker 调用。 */
static void destroy_client(void)
{
    portENTER_CRITICAL(&s_business.lock);
    s_business.gateway_online = false;
    portEXIT_CRITICAL(&s_business.lock);
    if (s_business.client_mutex != NULL &&
        xSemaphoreTake(s_business.client_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    gw_mqtt_client_t *client = s_business.client;
    s_business.client = NULL;
    s_business.subscribe_msg_id = -1;
    s_business.subscribe_index = 0U;
    s_business.connect_deadline_tick = 0;
    s_business.subscribe_deadline_tick = 0;
    s_business.gateway_subscribe_msg_id = -1;
    s_business.gateway_subscribe_deadline_tick = 0;
    set_online(false);
    clear_fragment();
    clear_outbound();
    if (client != NULL) {
        (void)gw_mqtt_client_destroy(client);
    }
    if (s_business.client_mutex != NULL) {
        xSemaphoreGive(s_business.client_mutex);
    }
    if (s_business.gateway_sender_task != NULL) {
        xTaskNotifyGive(s_business.gateway_sender_task);
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

    /* Lua waits 10s while fetching credentials and rebuilds an existing session every 5s. */
    uint32_t delay_ms = reason != NULL && strcmp(reason, "register_retry_wait") == 0 ?
                        GW_BUSINESS_MQTT_REGISTER_RETRY_MS :
                        (authentication_failed ?
                         s_business.config.auth_retry_ms :
                         s_business.config.retry_ms);
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
             authentication_failed ? "dev_mqtt authentication failed" :
                                     "dev_mqtt session failed",
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

/** @brief 查询当前编译选择的网络是否已经获得可用 IP。 */
static bool network_ready(void)
{
    gw_net_status_snapshot_t status = {0};
    return gw_net_get_status(&status) == ESP_OK &&
           (status.public_status == GW_NETWORK_STATUS_CONNECTED ||
            status.public_status == GW_NETWORK_STATUS_DEGRADED);
}

/** @brief 提交下一个业务下行 Topic 订阅并开始等待 SUBACK。 */
static esp_err_t begin_subscribe(void)
{
    if (s_business.client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *topic = NULL;
    switch (s_business.subscribe_index) {
    case 0U:
        topic = s_business.down_topic;
        break;
    case 1U:
        topic = s_business.down_config_topic;
        break;
    default:
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = -1;
    esp_err_t err = gw_mqtt_client_subscribe_with_msg_id(s_business.client,
                                                         topic,
                                                         GW_BUSINESS_MQTT_SUBSCRIBE_QOS,
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

/** @brief 独立订阅网关入口；其 SUBACK 不改变旧协议 ONLINE 门禁。 */
static void begin_gateway_subscribe(void)
{
    int msg_id = -1;
    esp_err_t err = gw_mqtt_client_subscribe_with_msg_id(s_business.client,
                                                         s_business.gateway_down_topic,
                                                         0,
                                                         &msg_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gateway subscribe failed: %s", esp_err_to_name(err));
        return;
    }
    s_business.gateway_subscribe_msg_id = msg_id;
    s_business.gateway_subscribe_deadline_tick =
        xTaskGetTickCount() + timeout_ticks(GW_BUSINESS_MQTT_SUBSCRIBE_TIMEOUT_MS);
    portENTER_CRITICAL(&s_business.lock);
    ++s_business.subscribe_count;
    portEXIT_CRITICAL(&s_business.lock);
}

/** @brief 使用当前注册凭据创建并启动唯一 dev_mqtt client。 */
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
        .role = GW_MQTT_CLIENT_ROLE_SYSTEM_DEV,
        .name = GW_BUSINESS_MQTT_NAME,
        .broker_uri = s_business.credentials.broker_uri,
        .client_id = s_business.credentials.client_id,
        .username = s_business.credentials.username[0] != '\0' ?
                    s_business.credentials.username : NULL,
        .password = s_business.credentials.password[0] != '\0' ?
                    s_business.credentials.password : NULL,
        .keepalive_sec = 60,
        .qos_default = GW_BUSINESS_MQTT_PUBLISH_QOS,
        .clean_session = true,
        .retain_default = false,
        .last_will_enabled = false,
        .reconnect_enabled = false,
        .reconnect_min_sec = 0,
        .reconnect_max_sec = 0,
        /* ESP-MQTT 缓冲还要容纳 Topic 和协议头；统一 transport 支持 8 KiB 管理报文。 */
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
             "dev_mqtt registration succeeded: device_id=%s client_id_len=%u",
             s_business.business_device_id,
             (unsigned int)strlen(s_business.credentials.client_id));
    return ESP_OK;
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
        s_business.subscribe_index = 0U;
        portENTER_CRITICAL(&s_business.lock);
        ++s_business.connected_count;
        portEXIT_CRITICAL(&s_business.lock);
        esp_err_t err = begin_subscribe();
        if (err != ESP_OK) {
            schedule_retry(false, err, 0, "subscribe_submit_failed");
            return false;
        }
        begin_gateway_subscribe();
        return true;
    }
    case GW_MQTT_CLIENT_EVENT_SUBSCRIBED:
        if (event->msg_id == s_business.gateway_subscribe_msg_id) {
            s_business.gateway_subscribe_msg_id = -1;
            s_business.gateway_subscribe_deadline_tick = 0;
            portENTER_CRITICAL(&s_business.lock);
            s_business.gateway_online = !event->subscribe_failed;
            portEXIT_CRITICAL(&s_business.lock);
            if (event->subscribe_failed) {
                ESP_LOGW(TAG, "gateway subscribe rejected");
            } else {
                ESP_LOGI(TAG, "gateway down subscribed: %s", s_business.gateway_down_topic);
            }
            xTaskNotifyGive(s_business.gateway_sender_task);
            return true;
        }
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
            if (s_business.subscribe_index + 1U < 2U) {
                ++s_business.subscribe_index;
                esp_err_t next_err = begin_subscribe();
                if (next_err != ESP_OK) {
                    schedule_retry(false, next_err, 0, "subscribe_submit_failed");
                    return false;
                }
            } else {
                set_online(true);
                set_state(GW_BUSINESS_MQTT_STATE_ONLINE, "online");
                ESP_LOGI(TAG,
                         "dev_mqtt online: down_raw=%s down_config=%s",
                         s_business.down_topic,
                         s_business.down_config_topic);
            }
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
        /* 业务上行固定 QoS0，正常情况下不会产生 PUBACK/PUBLISHED 事件。 */
        return true;
    case GW_MQTT_CLIENT_EVENT_UNSUBSCRIBED:
    default:
        return true;
    }
}

/** @brief 排空回调事件队列；遇到 client 切换时停止本轮业务处理。 */
static void process_event_queue(void)
{
    business_mqtt_event_t *event = NULL;
    while (xQueueReceive(s_business.event_queue, &event, 0) == pdTRUE) {
        if (!process_mqtt_event(event)) {
            free_event(event);
            break;
        }
        free_event(event);
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
    business_mqtt_event_t *event = NULL;
    while (xQueueReceive(s_business.data_queue, &event, 0) == pdTRUE) {
        (void)process_mqtt_event(event);
        free_event(event);
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
        s_business.subscribe_index = 0U;
        portENTER_CRITICAL(&s_business.lock);
        ++s_business.connected_count;
        portEXIT_CRITICAL(&s_business.lock);
        err = begin_subscribe();
        if (err != ESP_OK) {
            schedule_retry(false, err, 0, "subscribe_submit_failed");
        } else {
            begin_gateway_subscribe();
        }
    }
}

/**
 * @brief 提交当前 FIFO 队首并完成 QoS0 业务队列转移。
 *
 * `gw_mqtt_client_publish()` 成功时只表示数据已复制到 ESP-MQTT 的发送队列；该复制完成
 * 后即可释放业务 FIFO 槽位。QoS0 没有 PUBACK，不能等待 PUBLISHED 事件，否则队列会永久
 * 卡在队首。
 */
static void flush_outbound(void)
{
    if (s_business.client == NULL ||
        s_business.state != GW_BUSINESS_MQTT_STATE_ONLINE) {
        return;
    }
    if (s_business.client_mutex != NULL &&
        xSemaphoreTake(s_business.client_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) != pdTRUE) {
        if (s_business.client_mutex != NULL) {
            xSemaphoreGive(s_business.client_mutex);
        }
        return;
    }
    if (s_business.outbound_count == 0U) {
        xSemaphoreGive(s_business.outbound_mutex);
        if (s_business.client_mutex != NULL) {
            xSemaphoreGive(s_business.client_mutex);
        }
        return;
    }
    business_mqtt_outbound_t *item =
        &s_business.outbound[s_business.outbound_head];
    esp_err_t err = gw_mqtt_client_publish(s_business.client,
                                           item->topic_kind == BUSINESS_MQTT_TOPIC_UP_CONFIG ?
                                           s_business.up_config_topic : s_business.up_topic,
                                           item->payload,
                                           item->payload_len,
                                           GW_BUSINESS_MQTT_PUBLISH_QOS,
                                           false);
    if (err != ESP_OK) {
        xSemaphoreGive(s_business.outbound_mutex);
        if (s_business.client_mutex != NULL) {
            xSemaphoreGive(s_business.client_mutex);
        }
        schedule_retry(false, err, 0, "publish_failed");
        return;
    }

    heap_caps_free(item->payload);
    item->payload = NULL;
    item->payload_len = 0U;
    item->topic_kind = BUSINESS_MQTT_TOPIC_UP_RAW;
    s_business.outbound_head =
        (s_business.outbound_head + 1U) % GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY;
    --s_business.outbound_count;
    xSemaphoreGive(s_business.outbound_mutex);
    if (s_business.client_mutex != NULL) {
        xSemaphoreGive(s_business.client_mutex);
    }

    portENTER_CRITICAL(&s_business.lock);
    ++s_business.published_count;
    portEXIT_CRITICAL(&s_business.lock);
    xTaskNotifyGive(s_business.worker_task);
}

/** @brief 清空尚未提交的 QoS0 上行 FIFO。 */
static void clear_outbound(void)
{
    if (s_business.outbound == NULL || s_business.outbound_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) == pdTRUE) {
        free_outbound_items();
        memset(s_business.outbound, 0,
               GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY * sizeof(*s_business.outbound));
        s_business.outbound_head = 0U;
        s_business.outbound_count = 0U;
        xSemaphoreGive(s_business.outbound_mutex);
    }
}

/** @brief 清理指针事件队列，避免显式 stop/重建时泄漏 PSRAM。 */
static void drain_event_queue(QueueHandle_t queue)
{
    if (queue == NULL) {
        return;
    }
    business_mqtt_event_t *event = NULL;
    while (xQueueReceive(queue, &event, 0) == pdTRUE) {
        free_event(event);
        event = NULL;
    }
}

/** @brief 清理 raw/management 动态消息队列。 */
static void drain_message_queue(QueueHandle_t queue)
{
    if (queue == NULL) {
        return;
    }
    gw_business_mqtt_owned_message_t *message = NULL;
    while (xQueueReceive(queue, &message, 0) == pdTRUE) {
        free_owned_message(message);
        message = NULL;
    }
}

/** @brief 完成显式 stop 的资源和敏感信息清理。 */
static void handle_explicit_stop(void)
{
    destroy_client();
    clear_credentials();
    clear_retry();
    drain_event_queue(s_business.event_queue);
    drain_event_queue(s_business.data_queue);
    drain_message_queue(s_business.downlink_queue);
    drain_message_queue(s_business.management_queue);
    drain_message_queue(s_business.gateway_queue);
    xQueueReset(s_business.event_queue);
    xQueueReset(s_business.data_queue);
    xQueueReset(s_business.downlink_queue);
    xQueueReset(s_business.management_queue);
    xQueueReset(s_business.gateway_queue);

    portENTER_CRITICAL(&s_business.lock);
    s_business.run_requested = false;
    s_business.stop_requested = false;
    s_business.network_ready_seen = false;
    s_business.offline_since_valid = false;
    s_business.offline_since_tick = 0;
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

    if (!s_business.online) {
        mark_offline_if_needed();
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
        set_state(GW_BUSINESS_MQTT_STATE_WAIT_NETWORK, "wait_network");
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (!s_business.network_ready_seen) {
        /*
         * 网络就绪后留出一个短窗口，让底层网络资源稳定，再启动 HTTPS/MQTT dev_mqtt。
         * 生产启动只保留这一条活动 MQTT 连接。
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
    if (s_business.gateway_subscribe_msg_id >= 0 &&
        tick_reached(xTaskGetTickCount(), s_business.gateway_subscribe_deadline_tick)) {
        s_business.gateway_subscribe_msg_id = -1;
        s_business.gateway_subscribe_deadline_tick = 0;
        ESP_LOGW(TAG, "gateway subscribe timeout");
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
        check_offline_watchdog(run_requested, stop_requested);
        uint32_t wait_ms = (!run_requested && !stop_requested) ?
                           1000U : GW_BUSINESS_MQTT_WORKER_POLL_MS;
        (void)ulTaskNotifyTake(pdTRUE, timeout_ticks(wait_ms));
    }
}

/** @brief 独立发送任务；只在真实发布尝试时消耗网关发送间隔。 */
static void gateway_mqtt_sender(void *arg)
{
    (void)arg;
    for (;;) {
        TickType_t wait_ticks = portMAX_DELAY;
        business_mqtt_gateway_outbound_t item = {0};
        bool have_item = false;
        bool can_send = false;
        esp_err_t result = ESP_ERR_INVALID_STATE;

        if (xSemaphoreTake(s_business.client_mutex, portMAX_DELAY) == pdTRUE) {
            if (xSemaphoreTake(s_business.gateway_outbound_mutex, portMAX_DELAY) == pdTRUE) {
                if (s_business.gateway_outbound_count > 0U) {
                    business_mqtt_gateway_outbound_t *head =
                        &s_business.gateway_outbound[s_business.gateway_outbound_head];
                    portENTER_CRITICAL(&s_business.lock);
                    can_send = s_business.initialized && s_business.run_requested &&
                               !s_business.stop_requested && s_business.gateway_online &&
                               head->generation == s_business.client_generation &&
                               s_business.client != NULL;
                    portEXIT_CRITICAL(&s_business.lock);
                    int64_t now_us = esp_timer_get_time();
                    int64_t remaining_us = s_business.gateway_attempted ?
                        GW_BUSINESS_MQTT_GATEWAY_INTERVAL_US -
                        (now_us - s_business.gateway_last_attempt_us) : 0;
                    if (can_send && remaining_us > 0) {
                        wait_ticks = timeout_ticks((uint32_t)((remaining_us + 999LL) / 1000LL));
                    } else {
                        item = *head;
                        memset(head, 0, sizeof(*head));
                        s_business.gateway_outbound_head =
                            (s_business.gateway_outbound_head + 1U) %
                            GW_BUSINESS_MQTT_GATEWAY_QUEUE_CAPACITY;
                        --s_business.gateway_outbound_count;
                        have_item = true;
                    }
                }
                xSemaphoreGive(s_business.gateway_outbound_mutex);
            }
            if (have_item && can_send) {
                s_business.gateway_attempted = true;
                s_business.gateway_last_attempt_us = esp_timer_get_time();
                result = gw_mqtt_client_publish_immediate(s_business.client,
                                                          s_business.gateway_up_topic,
                                                          item.payload,
                                                          item.payload_len);
            }
            xSemaphoreGive(s_business.client_mutex);
        }

        if (have_item) {
            heap_caps_free(item.payload);
            portENTER_CRITICAL(&s_business.lock);
            if (result == ESP_OK) {
                ++s_business.published_count;
            } else {
                ++s_business.outbound_dropped_count;
            }
            portEXIT_CRITICAL(&s_business.lock);
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "gateway message dropped: %s", esp_err_to_name(result));
            }
            if (item.done != NULL) {
                item.done(item.context, result);
            }
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
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
        /* The service exposes one business group for both network targets. */
        err = copy_required_string(s_business.config.register_group_id,
                                   sizeof(s_business.config.register_group_id),
                                   GW_BUSINESS_MQTT_REGISTER_DEFAULT_GROUP_ID);
    }
    if (err == ESP_OK) {
        err = copy_required_string(s_business.config.topic_group_id,
                                   sizeof(s_business.config.topic_group_id),
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

    /* Always derive the business ID from the eFuse-backed storage identity. */
    gw_storage_identity_t identity = {0};
    err = gw_storage_get_identity(&identity);
    if (err != ESP_OK) {
        return err;
    }
    if (!valid_mac_text(identity.mac)) {
        return ESP_ERR_INVALID_STATE;
    }
    err = copy_mac_upper(s_business.business_device_id,
                         sizeof(s_business.business_device_id),
                         identity.mac);
    if (err != ESP_OK) {
        return err;
    }

    int written = snprintf(s_business.up_topic,
                           sizeof(s_business.up_topic),
                           "%s/YxzhNode%s/thing/model/up_raw",
                           s_business.config.topic_group_id,
                           s_business.business_device_id);
    if (written <= 0 || (size_t)written >= sizeof(s_business.up_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(s_business.up_config_topic,
                       sizeof(s_business.up_config_topic),
                       "%s/YxzhNode%s/thing/model/up_config",
                       s_business.config.topic_group_id,
                       s_business.business_device_id);
    if (written <= 0 || (size_t)written >= sizeof(s_business.up_config_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(s_business.down_topic,
                       sizeof(s_business.down_topic),
                       "%s/YxzhNode%s/thing/model/down_raw",
                       s_business.config.topic_group_id,
                       s_business.business_device_id);
    if (written <= 0 || (size_t)written >= sizeof(s_business.down_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(s_business.down_config_topic,
                       sizeof(s_business.down_config_topic),
                       "%s/YxzhNode%s/thing/model/down_config",
                       s_business.config.topic_group_id,
                       s_business.business_device_id);
    if (written <= 0 || (size_t)written >= sizeof(s_business.down_config_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(s_business.gateway_up_topic,
                       sizeof(s_business.gateway_up_topic),
                       "%s/YxzhNode%s/thing/model/config_up",
                       s_business.config.topic_group_id,
                       s_business.business_device_id);
    if (written <= 0 || (size_t)written >= sizeof(s_business.gateway_up_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(s_business.gateway_down_topic,
                       sizeof(s_business.gateway_down_topic),
                       "%s/YxzhNode%s/thing/model/config_down",
                       s_business.config.topic_group_id,
                       s_business.business_device_id);
    return written > 0 && (size_t)written < sizeof(s_business.gateway_down_topic) ?
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
     * 离线 FIFO 及四个 FreeRTOS 指针队列都只在普通任务上下文访问，统一放入 PSRAM，
     * 为业务 MQTT 的内部任务栈和 TLS 资源保留足够大的连续内存块。
     */
    s_business.outbound = (business_mqtt_outbound_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY,
        sizeof(*s_business.outbound),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.event_queue_storage = (uint8_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_EVENT_QUEUE_CAPACITY,
        sizeof(business_mqtt_event_t *),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.data_queue_storage = (uint8_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_DATA_QUEUE_CAPACITY,
        sizeof(business_mqtt_event_t *),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.downlink_queue_storage = (uint8_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY,
        sizeof(gw_business_mqtt_owned_message_t *),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.management_queue_storage = (uint8_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY,
        sizeof(gw_business_mqtt_owned_message_t *),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.gateway_outbound = (business_mqtt_gateway_outbound_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_GATEWAY_QUEUE_CAPACITY,
        sizeof(*s_business.gateway_outbound),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_business.gateway_queue_storage = (uint8_t *)heap_caps_calloc(
        GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY,
        sizeof(gw_business_mqtt_owned_message_t *),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_business.gateway_outbound == NULL || s_business.gateway_queue_storage == NULL ||
        s_business.outbound == NULL || s_business.event_queue_storage == NULL ||
        s_business.data_queue_storage == NULL ||
        s_business.downlink_queue_storage == NULL ||
        s_business.management_queue_storage == NULL) {
        free_psram_storage();
        return ESP_ERR_NO_MEM;
    }

    s_business.outbound_mutex =
        xSemaphoreCreateMutexStatic(&s_business.outbound_mutex_buffer);
    s_business.client_mutex =
        xSemaphoreCreateMutexStatic(&s_business.client_mutex_buffer);
    s_business.gateway_outbound_mutex =
        xSemaphoreCreateMutexStatic(&s_business.gateway_outbound_mutex_buffer);
    s_business.event_queue =
        xQueueCreateStatic(GW_BUSINESS_MQTT_EVENT_QUEUE_CAPACITY,
                           sizeof(business_mqtt_event_t *),
                           s_business.event_queue_storage,
                           &s_business.event_queue_buffer);
    s_business.data_queue =
        xQueueCreateStatic(GW_BUSINESS_MQTT_DATA_QUEUE_CAPACITY,
                           sizeof(business_mqtt_event_t *),
                           s_business.data_queue_storage,
                           &s_business.data_queue_buffer);
    s_business.downlink_queue =
        xQueueCreateStatic(GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY,
                           sizeof(gw_business_mqtt_owned_message_t *),
                           s_business.downlink_queue_storage,
                           &s_business.downlink_queue_buffer);
    s_business.management_queue =
        xQueueCreateStatic(GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY,
                           sizeof(gw_business_mqtt_owned_message_t *),
                           s_business.management_queue_storage,
                           &s_business.management_queue_buffer);
    s_business.gateway_queue =
        xQueueCreateStatic(GW_BUSINESS_MQTT_DOWNLINK_QUEUE_CAPACITY,
                           sizeof(gw_business_mqtt_owned_message_t *),
                           s_business.gateway_queue_storage,
                           &s_business.gateway_queue_buffer);
    s_business.control_events =
        xEventGroupCreateStatic(&s_business.control_events_buffer);
    if (s_business.gateway_outbound_mutex == NULL || s_business.gateway_queue == NULL ||
        s_business.outbound_mutex == NULL || s_business.client_mutex == NULL ||
        s_business.event_queue == NULL ||
        s_business.data_queue == NULL ||
        s_business.downlink_queue == NULL || s_business.management_queue == NULL ||
        s_business.control_events == NULL) {
        free_psram_storage();
        return ESP_ERR_NO_MEM;
    }

    s_business.state = GW_BUSINESS_MQTT_STATE_STOPPED;
    s_business.subscribe_msg_id = -1;
    s_business.subscribe_index = 0U;
    s_business.gateway_subscribe_msg_id = -1;
    strlcpy(s_business.last_reason, "initialized", sizeof(s_business.last_reason));
    s_business.worker_task =
        xTaskCreateStatic(business_mqtt_worker,
                          "dev_mqtt",
                          GW_BUSINESS_MQTT_WORKER_STACK_DEPTH,
                          NULL,
                          GW_BUSINESS_MQTT_WORKER_PRIORITY,
                          s_business.worker_task_stack,
                          &s_business.worker_task_buffer);
    if (s_business.worker_task == NULL) {
        free_psram_storage();
        return ESP_ERR_NO_MEM;
    }

    s_business.gateway_sender_task =
        xTaskCreateStatic(gateway_mqtt_sender,
                          "gateway_mqtt_tx",
                          GW_BUSINESS_MQTT_GATEWAY_STACK_DEPTH,
                          NULL,
                          GW_BUSINESS_MQTT_WORKER_PRIORITY,
                          s_business.gateway_sender_task_stack,
                          &s_business.gateway_sender_task_buffer);
    if (s_business.gateway_sender_task == NULL) {
        vTaskDelete(s_business.worker_task);
        s_business.worker_task = NULL;
        free_psram_storage();
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_business.lock);
    s_business.initialized = true;
    portEXIT_CRITICAL(&s_business.lock);
    xEventGroupSetBits(s_business.control_events, GW_BUSINESS_MQTT_STOPPED_BIT);
    ESP_LOGI(TAG,
             "dev_mqtt initialized: device_id=%s up_raw=%s up_config=%s down_raw=%s down_config=%s",
             s_business.business_device_id,
             s_business.up_topic,
             s_business.up_config_topic,
             s_business.down_topic,
             s_business.down_config_topic);
    ESP_LOGI(TAG, "gateway topics: up=%s down=%s",
             s_business.gateway_up_topic, s_business.gateway_down_topic);
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
    xTaskNotifyGive(s_business.gateway_sender_task);
    EventBits_t bits = xEventGroupWaitBits(s_business.control_events,
                                           GW_BUSINESS_MQTT_STOPPED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           timeout_ticks(timeout_ms));
    return (bits & GW_BUSINESS_MQTT_STOPPED_BIT) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

/** @brief 校验统一上行 topic 并返回其路由。 */
static bool is_up_topic(const char *topic, business_mqtt_topic_kind_t *out_kind)
{
    if (topic == NULL || topic[0] == '\0') {
        return false;
    }
    if (strcmp(topic, s_business.up_topic) == 0) {
        if (out_kind != NULL) {
            *out_kind = BUSINESS_MQTT_TOPIC_UP_RAW;
        }
        return true;
    }
    if (strcmp(topic, s_business.up_config_topic) == 0) {
        if (out_kind != NULL) {
            *out_kind = BUSINESS_MQTT_TOPIC_UP_CONFIG;
        }
        return true;
    }
    return false;
}

/** @brief 将任意长度上行复制到 PSRAM FIFO；调用方已完成长度/Topic 校验。 */
static esp_err_t enqueue_outbound(const void *payload,
                                  size_t payload_len,
                                  business_mqtt_topic_kind_t topic_kind)
{
    if ((payload == NULL && payload_len > 0U) ||
        payload_len > GW_BUSINESS_MQTT_MANAGEMENT_PAYLOAD_MAX_LEN) {
        return payload_len > GW_BUSINESS_MQTT_MANAGEMENT_PAYLOAD_MAX_LEN ?
               ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }

    uint8_t *payload_copy = payload_len > 0U ?
                            (uint8_t *)psram_copy_bytes(payload, payload_len, false) : NULL;
    if (payload_len > 0U && payload_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (s_business.outbound == NULL || s_business.outbound_mutex == NULL ||
        xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) != pdTRUE) {
        heap_caps_free(payload_copy);
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_business.lock);
    bool can_send = s_business.initialized && s_business.run_requested &&
                    !s_business.stop_requested && s_business.online &&
                    s_business.state == GW_BUSINESS_MQTT_STATE_ONLINE;
    bool initialized = s_business.initialized;
    portEXIT_CRITICAL(&s_business.lock);
    if (!can_send) {
        if (initialized) {
            portENTER_CRITICAL(&s_business.lock);
            ++s_business.outbound_dropped_count;
            strlcpy(s_business.last_reason,
                    "outbound_not_online",
                    sizeof(s_business.last_reason));
            portEXIT_CRITICAL(&s_business.lock);
        }
        xSemaphoreGive(s_business.outbound_mutex);
        heap_caps_free(payload_copy);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_business.outbound_count == GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY) {
        const char *drop_reason = "outbound_queue_drop_oldest";
        business_mqtt_outbound_t *oldest = &s_business.outbound[s_business.outbound_head];
        heap_caps_free(oldest->payload);
        oldest->payload = NULL;
        oldest->payload_len = 0U;
        oldest->topic_kind = BUSINESS_MQTT_TOPIC_UP_RAW;
        s_business.outbound_head =
            (s_business.outbound_head + 1U) % GW_BUSINESS_MQTT_OFFLINE_QUEUE_CAPACITY;
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
    heap_caps_free(item->payload);
    item->payload = payload_copy;
    item->payload_len = payload_len;
    item->topic_kind = (uint8_t)topic_kind;
    ++s_business.outbound_count;
    xSemaphoreGive(s_business.outbound_mutex);

    xTaskNotifyGive(s_business.worker_task);
    return ESP_OK;
}

esp_err_t gw_business_mqtt_publish_gateway(const void *payload,
                                          size_t payload_len,
                                          gw_business_mqtt_gateway_done_fn done,
                                          void *context)
{
    if (payload == NULL && payload_len > 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_business.gateway_outbound == NULL || s_business.gateway_outbound_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *payload_copy = payload_len > 0U ?
                            (uint8_t *)psram_copy_bytes(payload, payload_len, false) : NULL;
    if (payload_len > 0U && payload_copy == NULL) {
        ESP_LOGW(TAG, "gateway enqueue dropped: no memory");
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_business.gateway_outbound_mutex, portMAX_DELAY) != pdTRUE) {
        heap_caps_free(payload_copy);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_business.lock);
    bool can_send = s_business.initialized && s_business.run_requested &&
                    !s_business.stop_requested && s_business.gateway_online;
    uint32_t generation = s_business.client_generation;
    portEXIT_CRITICAL(&s_business.lock);
    esp_err_t err = !can_send ? ESP_ERR_INVALID_STATE :
                    (s_business.gateway_outbound_count == GW_BUSINESS_MQTT_GATEWAY_QUEUE_CAPACITY ?
                     ESP_ERR_NO_MEM : ESP_OK);
    if (err == ESP_OK) {
        size_t tail = (s_business.gateway_outbound_head + s_business.gateway_outbound_count) %
                      GW_BUSINESS_MQTT_GATEWAY_QUEUE_CAPACITY;
        s_business.gateway_outbound[tail] = (business_mqtt_gateway_outbound_t) {
            .payload = payload_copy,
            .payload_len = payload_len,
            .generation = generation,
            .done = done,
            .context = context,
        };
        ++s_business.gateway_outbound_count;
    }
    xSemaphoreGive(s_business.gateway_outbound_mutex);
    if (err != ESP_OK) {
        heap_caps_free(payload_copy);
        portENTER_CRITICAL(&s_business.lock);
        ++s_business.outbound_dropped_count;
        portEXIT_CRITICAL(&s_business.lock);
        ESP_LOGW(TAG, "gateway enqueue dropped: %s", esp_err_to_name(err));
        return err;
    }
    xTaskNotifyGive(s_business.gateway_sender_task);
    return ESP_OK;
}

esp_err_t gw_business_mqtt_send(const void *payload, size_t payload_len)
{
    if (payload_len > GW_BUSINESS_MQTT_PAYLOAD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    return enqueue_outbound(payload,
                            payload_len,
                            BUSINESS_MQTT_TOPIC_UP_RAW);
}

esp_err_t gw_business_mqtt_publish(const char *topic,
                                   const void *payload,
                                   size_t payload_len)
{
    business_mqtt_topic_kind_t topic_kind;
    if (!is_up_topic(topic, &topic_kind)) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_outbound(payload, payload_len, topic_kind);
}

esp_err_t gw_business_mqtt_publish_wait(const char *topic,
                                        const void *payload,
                                        size_t payload_len,
                                        uint32_t timeout_ms)
{
    business_mqtt_topic_kind_t topic_kind;
    if (!is_up_topic(topic, &topic_kind)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((payload == NULL && payload_len > 0U) ||
        payload_len > GW_BUSINESS_MQTT_MANAGEMENT_PAYLOAD_MAX_LEN) {
        return payload_len > GW_BUSINESS_MQTT_MANAGEMENT_PAYLOAD_MAX_LEN ?
               ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }
    if (s_business.client_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait_ticks = timeout_ticks(timeout_ms);
    if (xSemaphoreTake(s_business.client_mutex, wait_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_business.lock);
    bool can_send = s_business.initialized && s_business.run_requested &&
                    !s_business.stop_requested && s_business.online &&
                    s_business.state == GW_BUSINESS_MQTT_STATE_ONLINE;
    gw_mqtt_client_t *client = s_business.client;
    portEXIT_CRITICAL(&s_business.lock);

    esp_err_t err = can_send && client != NULL ?
                    gw_mqtt_client_publish(client,
                                           topic_kind == BUSINESS_MQTT_TOPIC_UP_CONFIG ?
                                           s_business.up_config_topic : s_business.up_topic,
                                           payload,
                                           payload_len,
                                           GW_BUSINESS_MQTT_PUBLISH_QOS,
                                           false) :
                    ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_business.client_mutex);

    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_business.lock);
        ++s_business.published_count;
        portEXIT_CRITICAL(&s_business.lock);
    }
    return err;
}

/** @brief 从指定动态队列领取消息并转移缓冲所有权给调用方。 */
static esp_err_t recv_owned_message(QueueHandle_t queue,
                                    gw_business_mqtt_owned_message_t *out_message,
                                    uint32_t timeout_ms)
{
    if (out_message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_message, 0, sizeof(*out_message));

    portENTER_CRITICAL(&s_business.lock);
    bool initialized = s_business.initialized;
    portEXIT_CRITICAL(&s_business.lock);
    if (!initialized || queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_business_mqtt_owned_message_t *message = NULL;
    if (xQueueReceive(queue, &message, timeout_ticks(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (message == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_message = *message;
    heap_caps_free(message);
    return ESP_OK;
}

esp_err_t gw_business_mqtt_recv_raw(gw_business_mqtt_owned_message_t *out_message,
                                     uint32_t timeout_ms)
{
    return recv_owned_message(s_business.downlink_queue, out_message, timeout_ms);
}

esp_err_t gw_business_mqtt_recv_management(gw_business_mqtt_management_message_t *out_message,
                                            uint32_t timeout_ms)
{
    return recv_owned_message(s_business.management_queue, out_message, timeout_ms);
}

esp_err_t gw_business_mqtt_recv_gateway(gw_business_mqtt_owned_message_t *out_message,
                                       uint32_t timeout_ms)
{
    return recv_owned_message(s_business.gateway_queue, out_message, timeout_ms);
}

void gw_business_mqtt_message_release(gw_business_mqtt_owned_message_t *message)
{
    free_owned_message_buffers(message);
}

esp_err_t gw_business_mqtt_recv(gw_business_mqtt_message_t *out_message,
                                uint32_t timeout_ms)
{
    if (out_message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_message, 0, sizeof(*out_message));

    gw_business_mqtt_owned_message_t owned = {0};
    esp_err_t err = gw_business_mqtt_recv_raw(&owned, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    size_t topic_len = owned.topic != NULL ? strlen(owned.topic) : 0U;
    if (topic_len >= sizeof(out_message->topic) ||
        owned.payload_len > sizeof(out_message->payload)) {
        gw_business_mqtt_message_release(&owned);
        count_downlink_drop("legacy_raw_message_oversize");
        return ESP_ERR_INVALID_SIZE;
    }
    if (topic_len > 0U) {
        memcpy(out_message->topic, owned.topic, topic_len + 1U);
    }
    if (owned.payload_len > 0U) {
        memcpy(out_message->payload, owned.payload, owned.payload_len);
    }
    out_message->payload_len = owned.payload_len;
    gw_business_mqtt_message_release(&owned);
    return ESP_OK;
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

    if (s_business.outbound_mutex != NULL &&
        xSemaphoreTake(s_business.outbound_mutex, portMAX_DELAY) == pdTRUE) {
        out_status->outbound_pending = s_business.outbound_count;
        xSemaphoreGive(s_business.outbound_mutex);
    }
    if (s_business.downlink_queue != NULL) {
        out_status->raw_downlink_pending =
            (size_t)uxQueueMessagesWaiting(s_business.downlink_queue);
    }
    if (s_business.management_queue != NULL) {
        out_status->management_downlink_pending =
            (size_t)uxQueueMessagesWaiting(s_business.management_queue);
    }
    out_status->downlink_pending = out_status->raw_downlink_pending +
                                   out_status->management_downlink_pending;

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
