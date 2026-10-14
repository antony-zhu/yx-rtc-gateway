#include "gw_mqtt_client.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file gw_mqtt_client.c
 * @brief 网关 MQTT 客户端共用的 esp-mqtt 轻量封装。
 *
 * 该层只处理连接状态、事件映射、发布订阅和资源隔离，不理解设备管理协议字段，也不
 * 维护 request_id。上层如果需要 payload 异步处理，必须在回调中复制 topic/data。
 */

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "mbedtls/platform_util.h"
#include "mqtt_client.h"

#include "gw_defaults.h"

static const char *TAG = "gw_mqtt_client";

#define GW_MQTT_WILL_TOPIC_MAX_LEN   192U
#define GW_MQTT_WILL_PAYLOAD_MAX_LEN 512U

/**
 * @brief 通用 MQTT client 私有状态。
 *
 * 敏感字段保存在内存中用于重连和底层配置，不允许直接打印。状态计数只用于诊断，
 * 不参与上层业务决策。
 */
struct gw_mqtt_client {
    esp_mqtt_client_handle_t esp_client;       /*!< esp-mqtt 句柄 */
    gw_mqtt_client_event_cb_t event_cb;        /*!< 上层事件回调 */
    void *user_ctx;                            /*!< 回调上下文 */
    portMUX_TYPE lock;                         /*!< 状态保护锁 */
    gw_mqtt_client_role_t role;                /*!< client 角色 */
    gw_mqtt_client_status_t status;            /*!< 当前连接状态 */
    uint32_t connected_count;                  /*!< CONNECTED 次数 */
    uint32_t disconnected_count;               /*!< DISCONNECTED 次数 */
    uint32_t error_count;                      /*!< ERROR 次数 */
    int last_msg_id;                           /*!< 最近一次消息 ID */
    int last_esp_err;                          /*!< 最近一次 ESP 错误 */
    int last_connect_return_code;              /*!< 最近一次 Broker 连接返回码 */
    size_t max_payload_size;                   /*!< 上层允许的最大 payload */
    bool started;                              /*!< esp-mqtt 是否已 start */
    char name[32];                             /*!< 诊断名称 */
    char broker_uri[192];                      /*!< Broker URI，不包含密码 */
    char client_id[128];                       /*!< MQTT Client ID */
    char username[128];                        /*!< 用户名 */
    char password[128];                        /*!< 密码，禁止明文日志 */
    char last_will_topic[GW_MQTT_WILL_TOPIC_MAX_LEN];     /*!< LWT Topic */
    char last_will_payload[GW_MQTT_WILL_PAYLOAD_MAX_LEN]; /*!< LWT Payload */
    int last_will_payload_len;                 /*!< LWT Payload 长度 */
    int last_will_qos;                         /*!< LWT QoS */
    int last_will_retain;                      /*!< LWT retain */
    size_t task_stack_bytes;                   /*!< MQTT task 栈大小 */
};

/** @brief 擦除凭据等私有状态后释放 client 内存。 */
static void secure_free_client(gw_mqtt_client_t *client)
{
    if (client == NULL) {
        return;
    }
    mbedtls_platform_zeroize(client, sizeof(*client));
    free(client);
}

/** @brief 根据角色返回默认诊断名称。 */
static const char *default_client_name(gw_mqtt_client_role_t role)
{
    switch (role) {
    case GW_MQTT_CLIENT_ROLE_SYSTEM_DEV:
        return "dev_mqtt";
    case GW_MQTT_CLIENT_ROLE_BUSINESS:
        return "business_mqtt";
    default:
        return "mqtt";
    }
}

/** @brief 判断字符串是否非空。 */
static bool valid_string(const char *value)
{
    return value != NULL && value[0] != '\0';
}

/** @brief 复制必填字符串并校验目标缓冲区是否足够。 */
static esp_err_t copy_required(char *out, size_t out_size, const char *value)
{
    if (!valid_string(value)) {
        return ESP_ERR_INVALID_ARG;
    }
    return strlcpy(out, value, out_size) < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/** @brief 复制必填二进制/字符串片段，并额外补 '\0' 方便日志排查。 */
static esp_err_t copy_required_bytes(char *out,
                                     size_t out_size,
                                     const char *value,
                                     size_t value_len)
{
    if (out == NULL || out_size == 0U || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value_len >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, value, value_len);
    out[value_len] = '\0';
    return ESP_OK;
}

/** @brief 更新连接状态。 */
static void set_status(gw_mqtt_client_t *client, gw_mqtt_client_status_t status)
{
    portENTER_CRITICAL(&client->lock);
    client->status = status;
    portEXIT_CRITICAL(&client->lock);
}

/** @brief 读取连接状态快照。 */
static gw_mqtt_client_status_t get_status_locked(gw_mqtt_client_t *client)
{
    gw_mqtt_client_status_t status;
    portENTER_CRITICAL(&client->lock);
    status = client->status;
    portEXIT_CRITICAL(&client->lock);
    return status;
}

/** @brief 将 esp-mqtt 事件映射为项目通用 MQTT 事件。 */
static bool map_event_id(esp_mqtt_event_id_t event_id, gw_mqtt_client_event_id_t *out_event_id)
{
    if (out_event_id == NULL) {
        return false;
    }

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        *out_event_id = GW_MQTT_CLIENT_EVENT_CONNECTED;
        return true;
    case MQTT_EVENT_DISCONNECTED:
        *out_event_id = GW_MQTT_CLIENT_EVENT_DISCONNECTED;
        return true;
    case MQTT_EVENT_SUBSCRIBED:
        *out_event_id = GW_MQTT_CLIENT_EVENT_SUBSCRIBED;
        return true;
    case MQTT_EVENT_UNSUBSCRIBED:
        *out_event_id = GW_MQTT_CLIENT_EVENT_UNSUBSCRIBED;
        return true;
    case MQTT_EVENT_PUBLISHED:
        *out_event_id = GW_MQTT_CLIENT_EVENT_PUBLISHED;
        return true;
    case MQTT_EVENT_DATA:
        *out_event_id = GW_MQTT_CLIENT_EVENT_DATA;
        return true;
    case MQTT_EVENT_ERROR:
        *out_event_id = GW_MQTT_CLIENT_EVENT_ERROR;
        return true;
    default:
        return false;
    }
}

/** @brief 判断 ERROR 事件是否为明确的用户名/授权失败。 */
static bool event_is_auth_failed(esp_mqtt_event_handle_t event)
{
    if (event == NULL || event->event_id != MQTT_EVENT_ERROR || event->error_handle == NULL) {
        return false;
    }

    /*
     * CONNACK 非零并不都表示凭据失效。例如 SERVER_UNAVAILABLE 应走普通重试，
     * 否则业务层会无意义地清凭据并重复 HTTPS 注册。
     */
    esp_mqtt_connect_return_code_t code = event->error_handle->connect_return_code;
    return code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
           code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED;
}

/** @brief 根据 esp-mqtt 事件更新内部状态计数。 */
static void update_from_event(gw_mqtt_client_t *client, esp_mqtt_event_handle_t event)
{
    portENTER_CRITICAL(&client->lock);
    client->last_msg_id = event->msg_id;
    if (event->error_handle != NULL) {
        client->last_esp_err = event->error_handle->esp_tls_last_esp_err;
        client->last_connect_return_code = event->error_handle->connect_return_code;
    }

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        client->status = GW_MQTT_CLIENT_STATUS_CONNECTED;
        client->connected_count++;
        break;
    case MQTT_EVENT_DISCONNECTED:
        /*
         * Broker 拒绝后通常还会紧跟 DISCONNECTED。保留 AUTH_FAILED，避免上层事件队列
         * 丢失鉴权事件时，状态快照退化成普通断线而继续复用已经失效的凭据。
         */
        if (client->status != GW_MQTT_CLIENT_STATUS_AUTH_FAILED) {
            client->status = GW_MQTT_CLIENT_STATUS_DISCONNECTED;
        }
        client->disconnected_count++;
        break;
    case MQTT_EVENT_ERROR:
        client->error_count++;
        client->status = event_is_auth_failed(event) ?
                         GW_MQTT_CLIENT_STATUS_AUTH_FAILED :
                         GW_MQTT_CLIENT_STATUS_ERROR;
        break;
    default:
        break;
    }
    portEXIT_CRITICAL(&client->lock);
}

/**
 * @brief esp-mqtt 原始事件处理入口。
 *
 * 这里不做业务解析，只完成状态更新和事件结构转换。topic/data 指针仍指向 esp-mqtt
 * 临时缓冲区，因此上层回调如果需要异步处理必须立即复制。
 */
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)base;
    (void)event_id;

    gw_mqtt_client_t *client = (gw_mqtt_client_t *)handler_args;
    esp_mqtt_event_handle_t esp_event = (esp_mqtt_event_handle_t)event_data;
    if (client == NULL || esp_event == NULL) {
        return;
    }

    gw_mqtt_client_event_id_t mapped_event_id;
    if (!map_event_id(esp_event->event_id, &mapped_event_id)) {
        return;
    }

    update_from_event(client, esp_event);

    gw_mqtt_client_event_t event = {
        .event_id = event_is_auth_failed(esp_event) ?
                    GW_MQTT_CLIENT_EVENT_AUTH_FAILED :
                    mapped_event_id,
        .msg_id = esp_event->msg_id,
        .topic = esp_event->topic,
        .topic_len = esp_event->topic_len,
        .data = esp_event->data,
        .data_len = esp_event->data_len,
        .total_data_len = esp_event->total_data_len,
        .current_data_offset = esp_event->current_data_offset,
        .retained = esp_event->retain != 0,
        .esp_err = esp_event->error_handle != NULL ?
                   esp_event->error_handle->esp_tls_last_esp_err :
                   0,
        .connect_return_code = esp_event->error_handle != NULL ?
                               esp_event->error_handle->connect_return_code :
                               0,
        .subscribe_failed =
            esp_event->event_id == MQTT_EVENT_SUBSCRIBED &&
            esp_event->error_handle != NULL &&
            esp_event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED,
    };

    if (client->event_cb != NULL) {
        client->event_cb(client, &event, client->user_ctx);
    }
}

esp_err_t gw_mqtt_client_create(const gw_mqtt_client_config_t *config,
                          gw_mqtt_client_event_cb_t event_cb,
                          void *user_ctx,
                          gw_mqtt_client_t **out_client)
{
    if (config == NULL || out_client == NULL ||
        !valid_string(config->broker_uri) || !valid_string(config->client_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_mqtt_client_t *client = (gw_mqtt_client_t *)calloc(1, sizeof(*client));
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    client->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    client->event_cb = event_cb;
    client->user_ctx = user_ctx;
    client->role = config->role;
    client->status = GW_MQTT_CLIENT_STATUS_IDLE;
    client->max_payload_size = config->max_payload_size != 0 ?
                               config->max_payload_size :
                               GW_MQTT_MAX_PAYLOAD_SIZE;
    client->task_stack_bytes = config->task_stack_bytes != 0 ?
                               config->task_stack_bytes :
                               GW_MQTT_DEFAULT_TASK_STACK_BYTES;

    esp_err_t err = copy_required(client->broker_uri,
                                  sizeof(client->broker_uri),
                                  config->broker_uri);
    if (err == ESP_OK) {
        err = copy_required(client->client_id, sizeof(client->client_id), config->client_id);
    }
    if (err == ESP_OK && config->name != NULL) {
        err = strlcpy(client->name, config->name, sizeof(client->name)) < sizeof(client->name) ?
              ESP_OK :
              ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && config->username != NULL) {
        err = strlcpy(client->username, config->username, sizeof(client->username)) < sizeof(client->username) ?
              ESP_OK :
              ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && config->password != NULL) {
        err = strlcpy(client->password, config->password, sizeof(client->password)) < sizeof(client->password) ?
              ESP_OK :
              ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && config->last_will_enabled) {
        if (config->last_will_qos > 2U) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            size_t will_len = config->last_will_payload_len != 0U ?
                              config->last_will_payload_len :
                              (config->last_will_payload != NULL ? strlen(config->last_will_payload) : 0U);
            err = copy_required(client->last_will_topic,
                                sizeof(client->last_will_topic),
                                config->last_will_topic);
            if (err == ESP_OK) {
                err = copy_required_bytes(client->last_will_payload,
                                          sizeof(client->last_will_payload),
                                          config->last_will_payload,
                                          will_len);
            }
            if (err == ESP_OK) {
                client->last_will_payload_len = (int)will_len;
                client->last_will_qos = (int)config->last_will_qos;
                client->last_will_retain = config->last_will_retain ? 1 : 0;
            }
        }
    }
    if (err != ESP_OK) {
        secure_free_client(client);
        return err;
    }

    if (client->name[0] == '\0') {
        strlcpy(client->name, default_client_name(config->role), sizeof(client->name));
    }

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = client->broker_uri,
        .credentials.client_id = client->client_id,
        .credentials.username = client->username[0] != '\0' ? client->username : NULL,
        .credentials.authentication.password = client->password[0] != '\0' ? client->password : NULL,
        .session.last_will.topic = client->last_will_topic[0] != '\0' ? client->last_will_topic : NULL,
        .session.last_will.msg = client->last_will_topic[0] != '\0' ? client->last_will_payload : NULL,
        .session.last_will.msg_len = client->last_will_topic[0] != '\0' ? client->last_will_payload_len : 0,
        .session.last_will.qos = client->last_will_qos,
        .session.last_will.retain = client->last_will_retain,
        .session.disable_clean_session = !config->clean_session,
        .session.keepalive = config->keepalive_sec != 0 ?
                             config->keepalive_sec :
                             GW_MQTT_DEFAULT_KEEP_ALIVE_SEC,
        .network.timeout_ms = GW_MQTT_DEFAULT_CONNECT_TIMEOUT_SEC * 1000,
        .network.disable_auto_reconnect = !config->reconnect_enabled,
        .network.reconnect_timeout_ms = (config->reconnect_min_sec != 0 ?
                                         config->reconnect_min_sec :
                                         GW_MQTT_DEFAULT_RECONNECT_SEC) * 1000,
        .task.stack_size = (int)client->task_stack_bytes,
        .buffer.size = (int)client->max_payload_size,
        .buffer.out_size = (int)client->max_payload_size,
    };

    client->esp_client = esp_mqtt_client_init(&mqtt_config);
    if (client->esp_client == NULL) {
        secure_free_client(client);
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(client->esp_client,
                                         MQTT_EVENT_ANY,
                                         mqtt_event_handler,
                                         client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(client->esp_client);
        secure_free_client(client);
        return err;
    }

    *out_client = client;
    ESP_LOGI(TAG, "%s created: broker=%s client_id_len=%u payload=%u stack=%u",
             client->name,
             client->broker_uri,
             (unsigned int)strlen(client->client_id),
             (unsigned int)client->max_payload_size,
             (unsigned int)client->task_stack_bytes);
    return ESP_OK;
}

esp_err_t gw_mqtt_client_destroy(gw_mqtt_client_t *client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (client->started) {
        (void)esp_mqtt_client_stop(client->esp_client);
    }
    esp_err_t err = esp_mqtt_client_destroy(client->esp_client);
    secure_free_client(client);
    return err;
}

esp_err_t gw_mqtt_client_connect(gw_mqtt_client_t *client)
{
    if (client == NULL || client->esp_client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (client->started) {
        return ESP_OK;
    }

    set_status(client, GW_MQTT_CLIENT_STATUS_CONNECTING);
    esp_err_t err = esp_mqtt_client_start(client->esp_client);
    if (err != ESP_OK) {
        set_status(client, GW_MQTT_CLIENT_STATUS_ERROR);
        ESP_LOGW(TAG,
                 "%s start failed: err=%s heap8=%u internal=%u largest_internal=%u",
                 client->name,
                 esp_err_to_name(err),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return err;
    }

    client->started = true;
    return ESP_OK;
}

esp_err_t gw_mqtt_client_disconnect(gw_mqtt_client_t *client)
{
    if (client == NULL || client->esp_client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!client->started) {
        set_status(client, GW_MQTT_CLIENT_STATUS_DISCONNECTED);
        return ESP_OK;
    }

    esp_err_t err = esp_mqtt_client_stop(client->esp_client);
    if (err == ESP_OK) {
        client->started = false;
        set_status(client, GW_MQTT_CLIENT_STATUS_DISCONNECTED);
    }
    return err;
}

esp_err_t gw_mqtt_client_publish(gw_mqtt_client_t *client,
                           const char *topic,
                           const void *payload,
                           size_t payload_len,
                           int qos,
                           bool retain)
{
    return gw_mqtt_client_publish_with_msg_id(client, topic, payload, payload_len, qos, retain, NULL);
}

esp_err_t gw_mqtt_client_publish_immediate(gw_mqtt_client_t *client,
                                          const char *topic,
                                          const void *payload,
                                          size_t payload_len)
{
    if (client == NULL || client->esp_client == NULL || !valid_string(topic) ||
        (payload == NULL && payload_len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (get_status_locked(client) != GW_MQTT_CLIENT_STATUS_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(client->esp_client,
                                         topic,
                                         payload_len > 0U ? (const char *)payload : NULL,
                                         (int)payload_len,
                                         0,
                                         0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&client->lock);
    client->last_msg_id = msg_id;
    portEXIT_CRITICAL(&client->lock);
    return ESP_OK;
}

esp_err_t gw_mqtt_client_publish_with_msg_id(gw_mqtt_client_t *client,
                                       const char *topic,
                                       const void *payload,
                                       size_t payload_len,
                                       int qos,
                                       bool retain,
                                       int *out_msg_id)
{
    if (client == NULL || client->esp_client == NULL || !valid_string(topic) ||
        (payload == NULL && payload_len > 0U) || qos < 0 || qos > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (payload_len > client->max_payload_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (get_status_locked(client) != GW_MQTT_CLIENT_STATUS_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    /**
     * 使用 enqueue 并要求 store=true，让 QoS0/1/2 都走同一套异步 outbox 路径。
     * sdkconfig 已开启 MQTT outbox data external memory，payload 不应落到内部 RAM。
     * 上层在未连接时已经被拒绝，因此这里不会形成离线缓存。
     */
    int msg_id = esp_mqtt_client_enqueue(client->esp_client,
                                         topic,
                                         payload_len > 0U ? (const char *)payload : NULL,
                                         (int)payload_len,
                                         qos,
                                         retain ? 1 : 0,
                                         true);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&client->lock);
    client->last_msg_id = msg_id;
    portEXIT_CRITICAL(&client->lock);
    if (out_msg_id != NULL) {
        *out_msg_id = msg_id;
    }
    return ESP_OK;
}

esp_err_t gw_mqtt_client_subscribe_with_msg_id(gw_mqtt_client_t *client,
                                         const char *topic,
                                         int qos,
                                         int *out_msg_id)
{
    if (client == NULL || client->esp_client == NULL || !valid_string(topic) ||
        qos < 0 || qos > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (get_status_locked(client) != GW_MQTT_CLIENT_STATUS_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_subscribe(client->esp_client, topic, qos);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&client->lock);
    client->last_msg_id = msg_id;
    portEXIT_CRITICAL(&client->lock);
    if (out_msg_id != NULL) {
        *out_msg_id = msg_id;
    }
    return ESP_OK;
}

esp_err_t gw_mqtt_client_subscribe(gw_mqtt_client_t *client, const char *topic, int qos)
{
    return gw_mqtt_client_subscribe_with_msg_id(client, topic, qos, NULL);
}

esp_err_t gw_mqtt_client_unsubscribe(gw_mqtt_client_t *client, const char *topic)
{
    if (client == NULL || client->esp_client == NULL || !valid_string(topic)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (get_status_locked(client) != GW_MQTT_CLIENT_STATUS_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_unsubscribe(client->esp_client, topic);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&client->lock);
    client->last_msg_id = msg_id;
    portEXIT_CRITICAL(&client->lock);
    return ESP_OK;
}

esp_err_t gw_mqtt_client_get_status(gw_mqtt_client_t *client, gw_mqtt_client_status_snapshot_t *out)
{
    if (client == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&client->lock);
    memset(out, 0, sizeof(*out));
    out->role = client->role;
    out->status = client->status;
    out->connected_count = client->connected_count;
    out->disconnected_count = client->disconnected_count;
    out->error_count = client->error_count;
    out->last_msg_id = client->last_msg_id;
    out->last_esp_err = client->last_esp_err;
    out->last_connect_return_code = client->last_connect_return_code;
    strlcpy(out->name, client->name, sizeof(out->name));
    strlcpy(out->broker_uri, client->broker_uri, sizeof(out->broker_uri));
    portEXIT_CRITICAL(&client->lock);
    return ESP_OK;
}

const char *gw_mqtt_client_status_to_string(gw_mqtt_client_status_t status)
{
    switch (status) {
    case GW_MQTT_CLIENT_STATUS_IDLE:
        return "idle";
    case GW_MQTT_CLIENT_STATUS_CONNECTING:
        return "connecting";
    case GW_MQTT_CLIENT_STATUS_CONNECTED:
        return "connected";
    case GW_MQTT_CLIENT_STATUS_DISCONNECTED:
        return "disconnected";
    case GW_MQTT_CLIENT_STATUS_AUTH_FAILED:
        return "auth_failed";
    case GW_MQTT_CLIENT_STATUS_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

const char *gw_mqtt_client_event_to_string(gw_mqtt_client_event_id_t event_id)
{
    switch (event_id) {
    case GW_MQTT_CLIENT_EVENT_CONNECTED:
        return "connected";
    case GW_MQTT_CLIENT_EVENT_DISCONNECTED:
        return "disconnected";
    case GW_MQTT_CLIENT_EVENT_AUTH_FAILED:
        return "auth_failed";
    case GW_MQTT_CLIENT_EVENT_SUBSCRIBED:
        return "subscribed";
    case GW_MQTT_CLIENT_EVENT_UNSUBSCRIBED:
        return "unsubscribed";
    case GW_MQTT_CLIENT_EVENT_PUBLISHED:
        return "published";
    case GW_MQTT_CLIENT_EVENT_DATA:
        return "data";
    case GW_MQTT_CLIENT_EVENT_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
