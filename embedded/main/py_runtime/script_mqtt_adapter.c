/**
 * @file script_mqtt_adapter.c
 * @brief 脚本 MQTT 多客户端适配层实现。
 */

#include "script_mqtt_adapter.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_log.h"
#include "gw_py_bridge.h"
#include "gw_result.h"
#include "gw_time.h"

/**
 * @brief esp-mqtt DATA 分片组包状态。
 *
 * esp-mqtt 在 payload 较大时会多次回调 DATA。脚本侧要求 Python 只收到完整包，
 * 因此每个脚本 client 都独立维护一个分片组包状态。payload 必须放在 PSRAM。
 */
typedef struct {
    bool active;
    int msg_id;
    size_t total_len;
    size_t received_len;
    char topic[GW_SCRIPT_MQTT_TOPIC_MAX_LEN];
    uint8_t *payload;
} script_mqtt_fragment_t;

typedef struct {
    bool in_use;
    bool configured;
    bool closing;
    script_mqtt_handle_t handle;
    QueueHandle_t event_queue;
    gw_mqtt_client_t *client;
    script_mqtt_fragment_t fragment;
    uint32_t queued_count;
    uint32_t dropped_count;
    uint32_t polled_count;
    uint32_t publish_count;
    uint32_t subscribe_count;
    uint32_t unsubscribe_count;
    uint32_t cleanup_count;
    char name[32];
    char broker_uri[192];
    char client_id[128];
    char username[128];
    char password[128];
    uint16_t keepalive_sec;
    uint8_t qos_default;
    bool clean_session;
    bool reconnect_enabled;
    char last_reason[GW_SCRIPT_MQTT_REASON_MAX_LEN];
} script_mqtt_slot_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static script_mqtt_handle_t s_next_handle = 1;
static script_mqtt_slot_t s_slots[GW_SCRIPT_MQTT_MAX_CLIENTS];

/** @brief 判断字符串是否非空。 */
static bool is_nonempty_string(const char *value)
{
    return value != NULL && value[0] != '\0';
}

/** @brief 判断脚本传入的 Broker URI 是否允许。 */
static bool mqtt_uri_allowed(const char *uri)
{
    return is_nonempty_string(uri) &&
           (strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "mqtts://", 8) == 0);
}

/** @brief 校验 QoS 取值。 */
static bool qos_valid(int qos)
{
    return qos >= 0 && qos <= 2;
}

/**
 * @brief 校验脚本 MQTT Topic。
 *
 * 脚本 MQTT 不复用系统 dev_mqtt 凭据，因此不在这里按平台系统 Topic 做额外封禁；
 * 安全边界由独立 Broker 凭据和 topic ACL 负责。
 */
static bool topic_allowed(const char *topic)
{
    return is_nonempty_string(topic) && strlen(topic) < GW_SCRIPT_MQTT_TOPIC_MAX_LEN;
}

/** @brief 获取可信 UTC 毫秒；RTC/NTP 不可信时返回 0。 */
static uint64_t trusted_utc_or_zero(void)
{
    gw_time_point_t now = gw_time_now();
    return now.utc_trusted ? now.utc_ms : 0ULL;
}

/** @brief 根据句柄查找槽位。 */
static script_mqtt_slot_t *slot_from_handle(script_mqtt_handle_t handle)
{
    if (handle == SCRIPT_MQTT_INVALID_HANDLE) {
        return NULL;
    }
    for (size_t i = 0U; i < GW_SCRIPT_MQTT_MAX_CLIENTS; ++i) {
        if (s_slots[i].in_use && s_slots[i].handle == handle) {
            return &s_slots[i];
        }
    }
    return NULL;
}

/** @brief 释放事件持有的 PSRAM payload。 */
static void release_payload(script_mqtt_event_t *event)
{
    if (event != NULL && event->payload != NULL) {
        heap_caps_free(event->payload);
        event->payload = NULL;
        event->payload_len = 0;
    }
}

/** @brief 清理未完成的 DATA 分片。 */
static void clear_fragment(script_mqtt_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    if (slot->fragment.payload != NULL) {
        heap_caps_free(slot->fragment.payload);
    }
    memset(&slot->fragment, 0, sizeof(slot->fragment));
}

/** @brief 更新最近诊断原因。 */
static void set_slot_reason(script_mqtt_slot_t *slot, const char *reason)
{
    if (slot == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(slot->last_reason,
            reason != NULL && reason[0] != '\0' ? reason : "",
            sizeof(slot->last_reason));
    portEXIT_CRITICAL(&s_lock);
}

/** @brief 记录丢弃事件。 */
static void inc_dropped(script_mqtt_slot_t *slot, const char *reason)
{
    if (slot == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    ++slot->dropped_count;
    strlcpy(slot->last_reason,
            reason != NULL && reason[0] != '\0' ? reason : "event_dropped",
            sizeof(slot->last_reason));
    portEXIT_CRITICAL(&s_lock);
}

/** @brief 映射通用 MQTT 事件。 */
static script_mqtt_event_id_t map_event(gw_mqtt_client_event_id_t event_id)
{
    switch (event_id) {
    case GW_MQTT_CLIENT_EVENT_CONNECTED:
        return SCRIPT_MQTT_EVENT_CONNECTED;
    case GW_MQTT_CLIENT_EVENT_DISCONNECTED:
        return SCRIPT_MQTT_EVENT_DISCONNECTED;
    case GW_MQTT_CLIENT_EVENT_AUTH_FAILED:
        return SCRIPT_MQTT_EVENT_AUTH_FAILED;
    case GW_MQTT_CLIENT_EVENT_SUBSCRIBED:
        return SCRIPT_MQTT_EVENT_SUBSCRIBED;
    case GW_MQTT_CLIENT_EVENT_UNSUBSCRIBED:
        return SCRIPT_MQTT_EVENT_UNSUBSCRIBED;
    case GW_MQTT_CLIENT_EVENT_PUBLISHED:
        return SCRIPT_MQTT_EVENT_PUBLISHED;
    case GW_MQTT_CLIENT_EVENT_DATA:
        return SCRIPT_MQTT_EVENT_DATA;
    case GW_MQTT_CLIENT_EVENT_ERROR:
    default:
        return SCRIPT_MQTT_EVENT_ERROR;
    }
}

/** @brief 将底层 topic 复制到脚本事件。 */
static bool copy_topic_to_event(script_mqtt_slot_t *slot,
                                const gw_mqtt_client_event_t *event,
                                script_mqtt_event_t *script_event)
{
    if (event->topic == NULL || event->topic_len <= 0) {
        return true;
    }

    size_t topic_len = (size_t)event->topic_len;
    if (topic_len >= sizeof(script_event->topic)) {
        inc_dropped(slot, "topic_too_long");
        return false;
    }
    memcpy(script_event->topic, event->topic, topic_len);
    script_event->topic[topic_len] = '\0';
    return true;
}

/** @brief 判断 DATA 分片是否属于同一条正在组包的 MQTT 消息。 */
static bool same_fragment_stream(const script_mqtt_slot_t *slot,
                                 const gw_mqtt_client_event_t *event,
                                 const script_mqtt_event_t *script_event,
                                 size_t total_len)
{
    const script_mqtt_fragment_t *fragment = &slot->fragment;
    if (!fragment->active || fragment->total_len != total_len ||
        fragment->received_len != (size_t)event->current_data_offset) {
        return false;
    }
    if (fragment->msg_id != event->msg_id) {
        return false;
    }
    return strcmp(fragment->topic, script_event->topic) == 0;
}

/** @brief 追加 DATA 分片，并在完整时把 payload 转移给脚本事件。 */
static bool append_fragment_payload(script_mqtt_slot_t *slot,
                                    const gw_mqtt_client_event_t *event,
                                    script_mqtt_event_t *script_event)
{
    if (event->total_data_len < 0 || event->data_len < 0 || event->current_data_offset < 0) {
        inc_dropped(slot, "fragment_invalid_len");
        clear_fragment(slot);
        return false;
    }

    size_t total_len = (size_t)event->total_data_len;
    size_t data_len = (size_t)event->data_len;
    size_t offset = (size_t)event->current_data_offset;
    if (total_len > GW_MQTT_MAX_PAYLOAD_SIZE || offset > total_len || data_len > total_len - offset) {
        inc_dropped(slot, "fragment_too_large");
        clear_fragment(slot);
        return false;
    }

    if (total_len == data_len && offset == 0) {
        if (data_len == 0U || event->data == NULL) {
            return true;
        }
        script_event->payload = heap_caps_malloc(data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (script_event->payload == NULL) {
            inc_dropped(slot, "payload_no_psram");
            return false;
        }
        memcpy(script_event->payload, event->data, data_len);
        script_event->payload_len = data_len;
        return true;
    }

    if (offset == 0U) {
        clear_fragment(slot);
        slot->fragment.payload = heap_caps_malloc(total_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (slot->fragment.payload == NULL) {
            inc_dropped(slot, "fragment_no_psram");
            return false;
        }
        slot->fragment.active = true;
        slot->fragment.msg_id = event->msg_id;
        slot->fragment.total_len = total_len;
        slot->fragment.received_len = 0U;
        strlcpy(slot->fragment.topic, script_event->topic, sizeof(slot->fragment.topic));
    } else if (!same_fragment_stream(slot, event, script_event, total_len)) {
        inc_dropped(slot, "fragment_sequence_invalid");
        clear_fragment(slot);
        return false;
    }

    if (data_len > 0U) {
        if (event->data == NULL) {
            inc_dropped(slot, "fragment_data_null");
            clear_fragment(slot);
            return false;
        }
        memcpy(slot->fragment.payload + offset, event->data, data_len);
    }
    slot->fragment.received_len = offset + data_len;

    if (slot->fragment.received_len < slot->fragment.total_len) {
        return false;
    }

    script_event->payload = slot->fragment.payload;
    script_event->payload_len = slot->fragment.total_len;
    slot->fragment.payload = NULL;
    clear_fragment(slot);
    return true;
}

/** @brief 投递脚本事件队列并唤醒 Python。 */
static void enqueue_event(script_mqtt_slot_t *slot, script_mqtt_event_t *event)
{
    if (slot == NULL || event == NULL || slot->event_queue == NULL || slot->closing) {
        release_payload(event);
        return;
    }

    if (xQueueSend(slot->event_queue, event, 0) != pdTRUE) {
        release_payload(event);
        inc_dropped(slot, "event_queue_full");
        return;
    }

    portENTER_CRITICAL(&s_lock);
    ++slot->queued_count;
    portEXIT_CRITICAL(&s_lock);
    (void)gw_py_bridge_wake(GW_PY_BRIDGE_SOURCE_MQTT);
}

/** @brief 底层 MQTT 事件回调。 */
static void script_mqtt_event_cb(gw_mqtt_client_t *client,
                                 const gw_mqtt_client_event_t *event,
                                 void *user_ctx)
{
    (void)client;
    script_mqtt_slot_t *slot = (script_mqtt_slot_t *)user_ctx;
    if (slot == NULL || event == NULL || !slot->in_use) {
        return;
    }

    script_mqtt_event_t script_event = {
        .handle = slot->handle,
        .event_id = map_event(event->event_id),
        .msg_id = event->msg_id,
        .esp_err = event->esp_err,
        .connect_return_code = event->connect_return_code,
        .timestamp_ms = trusted_utc_or_zero(),
    };
    strlcpy(script_event.name, slot->name, sizeof(script_event.name));

    if (!copy_topic_to_event(slot, event, &script_event)) {
        return;
    }
    if (event->event_id == GW_MQTT_CLIENT_EVENT_DATA &&
        !append_fragment_payload(slot, event, &script_event)) {
        return;
    }
    if (script_event.reason[0] == '\0') {
        strlcpy(script_event.reason,
                script_mqtt_event_to_string(script_event.event_id),
                sizeof(script_event.reason));
    }
    enqueue_event(slot, &script_event);
}

/** @brief 清空指定 slot 的脚本事件队列。 */
static void drain_event_queue(script_mqtt_slot_t *slot)
{
    if (slot == NULL || slot->event_queue == NULL) {
        return;
    }

    script_mqtt_event_t event = {0};
    while (xQueueReceive(slot->event_queue, &event, 0) == pdTRUE) {
        release_payload(&event);
    }
}

/** @brief 为 slot 创建事件队列。 */
static esp_err_t ensure_slot_queue(script_mqtt_slot_t *slot)
{
    if (slot->event_queue != NULL) {
        return ESP_OK;
    }
    slot->event_queue = xQueueCreate(GW_SCRIPT_MQTT_EVENT_QUEUE_LEN, sizeof(script_mqtt_event_t));
    return slot->event_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

/** @brief 释放 slot 的底层 client、队列事件和分片。 */
static void destroy_slot_resources(script_mqtt_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    slot->closing = true;
    if (slot->client != NULL) {
        (void)gw_mqtt_client_disconnect(slot->client);
        (void)gw_mqtt_client_destroy(slot->client);
        slot->client = NULL;
    }
    clear_fragment(slot);
    drain_event_queue(slot);
    slot->closing = false;
}

esp_err_t script_mqtt_adapter_init(void)
{
    portENTER_CRITICAL(&s_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t script_mqtt_adapter_create(const char *name, script_mqtt_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = SCRIPT_MQTT_INVALID_HANDLE;
    esp_err_t init_err = script_mqtt_adapter_init();
    if (init_err != ESP_OK) {
        return init_err;
    }

    for (size_t i = 0U; i < GW_SCRIPT_MQTT_MAX_CLIENTS; ++i) {
        script_mqtt_slot_t *slot = &s_slots[i];
        if (slot->in_use) {
            continue;
        }
        esp_err_t err = ensure_slot_queue(slot);
        if (err != ESP_OK) {
            return err;
        }

        QueueHandle_t queue = slot->event_queue;
        portENTER_CRITICAL(&s_lock);
        memset(slot, 0, sizeof(*slot));
        slot->event_queue = queue;
        slot->in_use = true;
        slot->handle = s_next_handle++;
        if (s_next_handle == SCRIPT_MQTT_INVALID_HANDLE) {
            s_next_handle = 1;
        }
        strlcpy(slot->name, is_nonempty_string(name) ? name : "script_mqtt", sizeof(slot->name));
        strlcpy(slot->last_reason, "created", sizeof(slot->last_reason));
        *out_handle = slot->handle;
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t script_mqtt_adapter_configure(script_mqtt_handle_t handle, const script_mqtt_config_t *config)
{
    if (config == NULL || !mqtt_uri_allowed(config->broker_uri) ||
        !is_nonempty_string(config->client_id) || !qos_valid(config->qos_default)) {
        return ESP_ERR_INVALID_ARG;
    }

    script_mqtt_slot_t *slot = slot_from_handle(handle);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (slot->client != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (config->name != NULL && config->name[0] != '\0') {
        strlcpy(slot->name, config->name, sizeof(slot->name));
    }
    if (strlcpy(slot->broker_uri, config->broker_uri, sizeof(slot->broker_uri)) >= sizeof(slot->broker_uri) ||
        strlcpy(slot->client_id, config->client_id, sizeof(slot->client_id)) >= sizeof(slot->client_id)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (config->username != NULL &&
        strlcpy(slot->username, config->username, sizeof(slot->username)) >= sizeof(slot->username)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (config->password != NULL &&
        strlcpy(slot->password, config->password, sizeof(slot->password)) >= sizeof(slot->password)) {
        return ESP_ERR_INVALID_SIZE;
    }
    slot->keepalive_sec = config->keepalive_sec;
    slot->qos_default = config->qos_default;
    slot->clean_session = config->clean_session;
    slot->reconnect_enabled = config->reconnect_enabled;
    slot->configured = true;
    set_slot_reason(slot, "configured");
    return ESP_OK;
}

esp_err_t script_mqtt_adapter_start(script_mqtt_handle_t handle)
{
    script_mqtt_slot_t *slot = slot_from_handle(handle);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!slot->configured) {
        return ESP_ERR_INVALID_STATE;
    }

    if (slot->client == NULL) {
        gw_mqtt_client_config_t mqtt_config = {
            .role = GW_MQTT_CLIENT_ROLE_SCRIPT,
            .name = slot->name,
            .broker_uri = slot->broker_uri,
            .client_id = slot->client_id,
            .username = slot->username[0] != '\0' ? slot->username : NULL,
            .password = slot->password[0] != '\0' ? slot->password : NULL,
            .keepalive_sec = slot->keepalive_sec,
            .qos_default = slot->qos_default,
            .clean_session = slot->clean_session,
            .retain_default = false,
            .last_will_enabled = false,
            .reconnect_enabled = slot->reconnect_enabled,
            .reconnect_min_sec = GW_MQTT_DEFAULT_RECONNECT_SEC,
            .reconnect_max_sec = GW_MQTT_DEFAULT_RECONNECT_MAX_SEC,
            .max_payload_size = GW_MQTT_MAX_PAYLOAD_SIZE,
        };

        esp_err_t err = gw_mqtt_client_create(&mqtt_config, script_mqtt_event_cb, slot, &slot->client);
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = gw_mqtt_client_connect(slot->client);
    if (err == ESP_OK) {
        set_slot_reason(slot, "connecting");
        gw_log_write(GW_LOG_LEVEL_INFO, "script_mqtt", "start", "script MQTT connect requested", NULL);
    }
    return err;
}

esp_err_t script_mqtt_adapter_stop(script_mqtt_handle_t handle)
{
    script_mqtt_slot_t *slot = slot_from_handle(handle);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (slot->client != NULL) {
        esp_err_t err = gw_mqtt_client_disconnect(slot->client);
        if (err != ESP_OK) {
            return err;
        }
    }
    clear_fragment(slot);
    drain_event_queue(slot);
    set_slot_reason(slot, "stopped");
    return ESP_OK;
}

esp_err_t script_mqtt_adapter_destroy(script_mqtt_handle_t handle)
{
    script_mqtt_slot_t *slot = slot_from_handle(handle);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    destroy_slot_resources(slot);
    portENTER_CRITICAL(&s_lock);
    ++slot->cleanup_count;
    slot->in_use = false;
    slot->configured = false;
    strlcpy(slot->last_reason, "destroyed", sizeof(slot->last_reason));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t script_mqtt_adapter_connect(const script_mqtt_config_t *config, script_mqtt_handle_t *out_handle)
{
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    esp_err_t err = script_mqtt_adapter_create(config != NULL ? config->name : NULL, &handle);
    if (err == ESP_OK) {
        err = script_mqtt_adapter_configure(handle, config);
    }
    if (err == ESP_OK) {
        err = script_mqtt_adapter_start(handle);
    }
    if (err != ESP_OK && handle != SCRIPT_MQTT_INVALID_HANDLE) {
        (void)script_mqtt_adapter_destroy(handle);
    }
    if (err == ESP_OK && out_handle != NULL) {
        *out_handle = handle;
    }
    return err;
}

esp_err_t script_mqtt_adapter_disconnect_all(void)
{
    for (size_t i = 0U; i < GW_SCRIPT_MQTT_MAX_CLIENTS; ++i) {
        if (s_slots[i].in_use) {
            (void)script_mqtt_adapter_destroy(s_slots[i].handle);
        }
    }
    gw_py_bridge_invalidate(GW_PY_BRIDGE_SOURCE_MQTT);
    return ESP_OK;
}

esp_err_t script_mqtt_adapter_publish(script_mqtt_handle_t handle,
                                      const char *topic,
                                      const void *payload,
                                      size_t payload_len,
                                      int qos,
                                      bool retain)
{
    if (!topic_allowed(topic) || (!payload && payload_len > 0U) ||
        payload_len > GW_MQTT_MAX_PAYLOAD_SIZE || !qos_valid(qos)) {
        return ESP_ERR_INVALID_ARG;
    }

    script_mqtt_slot_t *slot = slot_from_handle(handle);
    if (slot == NULL || slot->client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gw_mqtt_client_publish(slot->client, topic, payload, payload_len, qos, retain);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        ++slot->publish_count;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

esp_err_t script_mqtt_adapter_subscribe(script_mqtt_handle_t handle, const char *topic, int qos)
{
    if (!topic_allowed(topic) || !qos_valid(qos)) {
        return ESP_ERR_INVALID_ARG;
    }

    script_mqtt_slot_t *slot = slot_from_handle(handle);
    if (slot == NULL || slot->client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gw_mqtt_client_subscribe(slot->client, topic, qos);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        ++slot->subscribe_count;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

esp_err_t script_mqtt_adapter_unsubscribe(script_mqtt_handle_t handle, const char *topic)
{
    if (!topic_allowed(topic)) {
        return ESP_ERR_INVALID_ARG;
    }

    script_mqtt_slot_t *slot = slot_from_handle(handle);
    if (slot == NULL || slot->client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gw_mqtt_client_unsubscribe(slot->client, topic);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        ++slot->unsubscribe_count;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

static esp_err_t recv_from_slot(script_mqtt_slot_t *slot, uint32_t timeout_ms, script_mqtt_event_t *out_event)
{
    if (slot == NULL || slot->event_queue == NULL || out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(slot->event_queue, out_event, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_lock);
    ++slot->polled_count;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t script_mqtt_adapter_recv(script_mqtt_handle_t handle,
                                   uint32_t timeout_ms,
                                   script_mqtt_event_t *out_event)
{
    if (out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_event, 0, sizeof(*out_event));

    if (handle != SCRIPT_MQTT_INVALID_HANDLE) {
        script_mqtt_slot_t *slot = slot_from_handle(handle);
        return recv_from_slot(slot, timeout_ms, out_event);
    }

    int64_t start_us = esp_timer_get_time();
    for (;;) {
        for (size_t i = 0U; i < GW_SCRIPT_MQTT_MAX_CLIENTS; ++i) {
            if (!s_slots[i].in_use || s_slots[i].event_queue == NULL) {
                continue;
            }
            if (recv_from_slot(&s_slots[i], 0, out_event) == ESP_OK) {
                return ESP_OK;
            }
        }
        if (timeout_ms == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000LL;
        if (elapsed_ms >= (int64_t)timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void script_mqtt_adapter_release_event(script_mqtt_event_t *event)
{
    release_payload(event);
}

esp_err_t script_mqtt_adapter_get_status(script_mqtt_handle_t handle, script_mqtt_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;

    if (handle == SCRIPT_MQTT_INVALID_HANDLE) {
        out->mqtt_status = GW_MQTT_CLIENT_STATUS_IDLE;
        for (size_t i = 0U; i < GW_SCRIPT_MQTT_MAX_CLIENTS; ++i) {
            script_mqtt_slot_t *slot = &s_slots[i];
            if (!slot->in_use) {
                continue;
            }
            out->in_use = true;
            out->queued_count += slot->queued_count;
            out->dropped_count += slot->dropped_count;
            out->polled_count += slot->polled_count;
            out->publish_count += slot->publish_count;
            out->subscribe_count += slot->subscribe_count;
            out->unsubscribe_count += slot->unsubscribe_count;
            out->cleanup_count += slot->cleanup_count;
            out->queue_pending += slot->event_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(slot->event_queue) : 0U;
        }
        strlcpy(out->name, "all", sizeof(out->name));
        return ESP_OK;
    }

    script_mqtt_slot_t *slot = slot_from_handle(handle);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    gw_mqtt_client_status_snapshot_t mqtt_status = {0};
    if (slot->client != NULL) {
        (void)gw_mqtt_client_get_status(slot->client, &mqtt_status);
    }

    out->in_use = slot->in_use;
    out->configured = slot->configured;
    out->client_created = slot->client != NULL;
    out->handle = slot->handle;
    out->mqtt_status = slot->client != NULL ? mqtt_status.status : GW_MQTT_CLIENT_STATUS_IDLE;
    out->queued_count = slot->queued_count;
    out->dropped_count = slot->dropped_count;
    out->polled_count = slot->polled_count;
    out->publish_count = slot->publish_count;
    out->subscribe_count = slot->subscribe_count;
    out->unsubscribe_count = slot->unsubscribe_count;
    out->cleanup_count = slot->cleanup_count;
    out->queue_pending = slot->event_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(slot->event_queue) : 0U;
    strlcpy(out->name, slot->name, sizeof(out->name));
    strlcpy(out->broker_uri, slot->broker_uri, sizeof(out->broker_uri));
    strlcpy(out->last_reason, slot->last_reason, sizeof(out->last_reason));
    return ESP_OK;
}

void script_mqtt_adapter_cleanup_on_script_exit(void)
{
    (void)script_mqtt_adapter_disconnect_all();
}

const char *script_mqtt_event_to_string(script_mqtt_event_id_t event_id)
{
    switch (event_id) {
    case SCRIPT_MQTT_EVENT_CONNECTED:
        return "connected";
    case SCRIPT_MQTT_EVENT_DISCONNECTED:
        return "disconnected";
    case SCRIPT_MQTT_EVENT_AUTH_FAILED:
        return "auth_failed";
    case SCRIPT_MQTT_EVENT_SUBSCRIBED:
        return "subscribed";
    case SCRIPT_MQTT_EVENT_UNSUBSCRIBED:
        return "unsubscribed";
    case SCRIPT_MQTT_EVENT_PUBLISHED:
        return "published";
    case SCRIPT_MQTT_EVENT_DATA:
        return "data";
    case SCRIPT_MQTT_EVENT_ERROR:
    default:
        return "error";
    }
}
