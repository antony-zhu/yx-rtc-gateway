#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the production implementation, including its static reassembly/sender. */
#include "../../embedded/components/gw_app/src/mqtt/gw_business_mqtt.c"

struct gw_mqtt_client { int unused; };
static struct gw_mqtt_client host_client;
static gw_storage_identity_t host_identity;
static int64_t host_now_us;
static unsigned host_critical_depth;
static size_t host_allocations;
static bool host_fail_allocation;
static bool host_sender_running;
static jmp_buf host_sender_idle;
static unsigned host_wait_count;
static unsigned host_destroy_count;

#define HOST_RECORD_CAPACITY 128U

typedef struct {
    int64_t time_us;
    unsigned char first_byte;
    size_t payload_len;
    esp_err_t result;
    char topic[GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
} host_attempt_t;
static host_attempt_t host_attempts[HOST_RECORD_CAPACITY];
static size_t host_attempt_count;
static esp_err_t host_first_attempt_result;
static int64_t host_old_publish_times[HOST_RECORD_CAPACITY];
static size_t host_old_publish_count;
static char host_subscribe_topics[8][GW_BUSINESS_MQTT_TOPIC_MAX_LEN];
static int host_subscribe_qos[8];
static size_t host_subscribe_count;
static int host_done_order[HOST_RECORD_CAPACITY];
static size_t host_done_count;

typedef struct host_done_context {
    unsigned calls;
    esp_err_t result;
    int index;
    struct host_done_context *enqueue_from_callback;
    bool publish_old_from_callback;
} host_done_context_t;

static void assert_no_transport_locks(void)
{
    assert(host_critical_depth == 0U);
    assert(!s_business.client_mutex->held);
    assert(!s_business.gateway_outbound_mutex->held);
    assert(!s_business.outbound_mutex->held);
}

static void host_gateway_done(void *context, esp_err_t result)
{
    host_done_context_t *done = context;
    assert_no_transport_locks();
    assert(done->calls == 0U);
    ++done->calls;
    done->result = result;
    assert(host_done_count < HOST_RECORD_CAPACITY);
    host_done_order[host_done_count++] = done->index;
    if (done->enqueue_from_callback != NULL) {
        unsigned char payload = 'R';
        assert(gw_business_mqtt_publish_gateway(&payload, 1U, host_gateway_done,
                                                done->enqueue_from_callback) == ESP_OK);
    }
    if (done->publish_old_from_callback) {
        assert(gw_business_mqtt_publish_wait(s_business.up_topic, "a", 1U, 0U) == ESP_OK);
        assert(gw_business_mqtt_publish_wait(s_business.up_config_topic, "b", 1U, 0U) == ESP_OK);
    }
}

void host_enter_critical(portMUX_TYPE *lock)
{
    assert(*lock == 0U);
    *lock = 1U;
    ++host_critical_depth;
}

void host_exit_critical(portMUX_TYPE *lock)
{
    assert(*lock == 1U && host_critical_depth > 0U);
    *lock = 0U;
    --host_critical_depth;
}

size_t strlcpy(char *target, const char *source, size_t size)
{
    size_t length = strlen(source);
    if (size > 0U) {
        size_t copied = length < size - 1U ? length : size - 1U;
        memcpy(target, source, copied);
        target[copied] = '\0';
    }
    return length;
}

typedef struct {
    max_align_t alignment;
    size_t size;
} host_allocation_t;

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    if (host_fail_allocation) {
        host_fail_allocation = false;
        return NULL;
    }
    assert(size == 0U || count <= (SIZE_MAX - sizeof(host_allocation_t)) / size);
    host_allocation_t *allocation = calloc(1U, sizeof(*allocation) + count * size);
    assert(allocation != NULL);
    allocation->size = count * size;
    ++host_allocations;
    return allocation + 1;
}

void heap_caps_free(void *memory)
{
    if (memory != NULL) {
        assert(host_allocations > 0U);
        --host_allocations;
        free((host_allocation_t *)memory - 1);
    }
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    buffer->held = false;
    return buffer;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)ticks;
    assert(semaphore != NULL && !semaphore->held);
    semaphore->held = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore != NULL && semaphore->held);
    semaphore->held = false;
    return pdTRUE;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *buffer)
{
    *buffer = (StaticQueue_t) {
        .capacity = capacity,
        .item_size = item_size,
        .storage = storage,
    };
    return buffer;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    (void)ticks;
    assert(queue != NULL);
    if (queue->count == queue->capacity) {
        return pdFALSE;
    }
    size_t tail = (queue->head + queue->count) % queue->capacity;
    memcpy(queue->storage + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    (void)ticks;
    assert(queue != NULL);
    if (queue->count == 0U) {
        return pdFALSE;
    }
    memcpy(item, queue->storage + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    queue->count = 0U;
    queue->head = 0U;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    return (UBaseType_t)queue->count;
}

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *buffer)
{
    buffer->bits = 0U;
    return buffer;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    return group->bits |= bits;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    EventBits_t previous = group->bits;
    group->bits &= ~bits;
    return previous;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                               BaseType_t clear, BaseType_t all, TickType_t ticks)
{
    (void)bits;
    (void)clear;
    (void)all;
    (void)ticks;
    return group->bits;
}

TaskHandle_t xTaskCreateStatic(TaskFunction_t function, const char *name,
                               uint32_t stack_depth, void *argument,
                               UBaseType_t priority, StackType_t *stack,
                               StaticTask_t *buffer)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)stack;
    buffer->function = function;
    buffer->argument = argument;
    return buffer;
}

void vTaskDelete(TaskHandle_t task)
{
    task->function = NULL;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(host_now_us / 1000LL);
}

void xTaskNotifyGive(TaskHandle_t task)
{
    assert(task != NULL);
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    (void)clear;
    assert(host_sender_running);
    assert_no_transport_locks();
    if (ticks == portMAX_DELAY) {
        longjmp(host_sender_idle, 1);
    }
    assert(ticks > 0U && host_wait_count < HOST_RECORD_CAPACITY);
    ++host_wait_count;
    host_now_us += (int64_t)ticks * 1000LL;
    return 0U;
}

int64_t esp_timer_get_time(void) { return host_now_us; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "host-error"; }
void esp_restart(void) { assert(!"unexpected restart"); }
esp_err_t gw_board_mqtt_led_set(bool online) { (void)online; return ESP_OK; }
void gw_state_set_device_status(gw_device_status_t status) { (void)status; }
void gw_state_set_mqtt_status(gw_mqtt_status_t status) { (void)status; }
esp_err_t gw_state_get_snapshot(gw_state_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}
esp_err_t gw_net_get_status(gw_net_status_snapshot_t *out)
{
    out->public_status = GW_NETWORK_STATUS_CONNECTED;
    return ESP_OK;
}
esp_err_t gw_storage_get_identity(gw_storage_identity_t *out)
{
    *out = host_identity;
    return ESP_OK;
}
esp_err_t gw_business_mqtt_register(const gw_business_mqtt_register_config_t *config,
                                   gw_business_mqtt_credentials_t *credentials,
                                   gw_result_t *result)
{
    (void)config;
    (void)credentials;
    (void)result;
    return ESP_FAIL;
}
void gw_business_mqtt_credentials_clear(gw_business_mqtt_credentials_t *credentials)
{
    memset(credentials, 0, sizeof(*credentials));
}
esp_err_t gw_mqtt_client_create(const gw_mqtt_client_config_t *config,
                                gw_mqtt_client_event_cb_t callback, void *context,
                                gw_mqtt_client_t **out)
{
    (void)config;
    (void)callback;
    (void)context;
    *out = &host_client;
    return ESP_OK;
}
esp_err_t gw_mqtt_client_connect(gw_mqtt_client_t *client)
{
    assert(client == &host_client);
    return ESP_OK;
}
esp_err_t gw_mqtt_client_destroy(gw_mqtt_client_t *client)
{
    assert(client == &host_client);
    ++host_destroy_count;
    return ESP_OK;
}
esp_err_t gw_mqtt_client_get_status(gw_mqtt_client_t *client,
                                    gw_mqtt_client_status_snapshot_t *out)
{
    assert(client == &host_client);
    memset(out, 0, sizeof(*out));
    out->status = GW_MQTT_CLIENT_STATUS_CONNECTED;
    return ESP_OK;
}
esp_err_t gw_mqtt_client_subscribe_with_msg_id(gw_mqtt_client_t *client,
                                             const char *topic, int qos, int *out_id)
{
    assert(client == &host_client && host_subscribe_count < 8U);
    strlcpy(host_subscribe_topics[host_subscribe_count], topic,
            sizeof(host_subscribe_topics[0]));
    host_subscribe_qos[host_subscribe_count] = qos;
    *out_id = 100 + (int)++host_subscribe_count;
    return ESP_OK;
}
esp_err_t gw_mqtt_client_publish(gw_mqtt_client_t *client, const char *topic,
                                 const void *payload, size_t length, int qos, bool retain)
{
    assert(client == &host_client && qos == 0 && !retain);
    assert(strcmp(topic, s_business.up_topic) == 0 ||
           strcmp(topic, s_business.up_config_topic) == 0);
    assert(payload != NULL && length > 0U);
    assert(host_old_publish_count < HOST_RECORD_CAPACITY);
    host_old_publish_times[host_old_publish_count++] = host_now_us;
    return ESP_OK;
}
esp_err_t gw_mqtt_client_publish_immediate(gw_mqtt_client_t *client, const char *topic,
                                           const void *payload, size_t length)
{
    assert(client == &host_client && s_business.client_mutex->held);
    assert(!s_business.gateway_outbound_mutex->held && host_critical_depth == 0U);
    assert(host_attempt_count < HOST_RECORD_CAPACITY);
    host_attempt_t *attempt = &host_attempts[host_attempt_count];
    *attempt = (host_attempt_t) {
        .time_us = host_now_us,
        .first_byte = length > 0U ? *(const unsigned char *)payload : 0U,
        .payload_len = length,
        .result = host_attempt_count == 0U ? host_first_attempt_result : ESP_OK,
    };
    strlcpy(attempt->topic, topic, sizeof(attempt->topic));
    ++host_attempt_count;
    return attempt->result;
}

static void run_sender_until_idle(void)
{
    assert(!host_sender_running);
    host_sender_running = true;
    if (setjmp(host_sender_idle) == 0) {
        s_business.gateway_sender_task->function(s_business.gateway_sender_task->argument);
        assert(!"sender must yield when idle");
    }
    host_sender_running = false;
    assert_no_transport_locks();
    assert(s_business.gateway_outbound_count == 0U);
}

static void cleanup_transport(void)
{
    assert(s_business.gateway_outbound_count == 0U);
    clear_fragment();
    clear_outbound();
    drain_event_queue(s_business.event_queue);
    drain_event_queue(s_business.data_queue);
    drain_message_queue(s_business.downlink_queue);
    drain_message_queue(s_business.management_queue);
    drain_message_queue(s_business.gateway_queue);
    free_psram_storage();
    assert(host_allocations == 0U);
}

static void reset_transport(void)
{
    assert(host_allocations == 0U);
    memset(&s_business, 0, sizeof(s_business));
    memset(host_attempts, 0, sizeof(host_attempts));
    memset(host_old_publish_times, 0, sizeof(host_old_publish_times));
    memset(host_subscribe_topics, 0, sizeof(host_subscribe_topics));
    memset(host_subscribe_qos, 0, sizeof(host_subscribe_qos));
    memset(host_done_order, 0, sizeof(host_done_order));
    host_now_us = 123456LL;
    host_critical_depth = 0U;
    host_fail_allocation = false;
    host_wait_count = 0U;
    host_destroy_count = 0U;
    host_attempt_count = 0U;
    host_old_publish_count = 0U;
    host_subscribe_count = 0U;
    host_done_count = 0U;
    host_first_attempt_result = ESP_OK;
    memset(&host_identity, 0, sizeof(host_identity));
    strcpy(host_identity.mac, "3C0F02CA4A7C");
    strcpy(host_identity.device_id, "OVERRIDE_1");
    assert(gw_business_mqtt_init(NULL) == ESP_OK);
    assert(strcmp(s_business.business_device_id, "3C0F02CA4A7C") == 0);
    assert(strcmp(s_business.gateway_up_topic,
                  "yx_gw/YxzhNode3C0F02CA4A7C/thing/model/config_up") == 0);
    assert(strcmp(s_business.gateway_down_topic,
                  "yx_gw/YxzhNode3C0F02CA4A7C/thing/model/config_down") == 0);
    assert(strcmp(s_business.up_topic, "yx_gw/YxzhNode3C0F02CA4A7C/thing/model/up_raw") == 0);
    assert(strcmp(s_business.up_config_topic,
                  "yx_gw/YxzhNode3C0F02CA4A7C/thing/model/up_config") == 0);
    assert(strcmp(s_business.down_topic, "yx_gw/YxzhNode3C0F02CA4A7C/thing/model/down_raw") == 0);
    assert(strcmp(s_business.down_config_topic,
                  "yx_gw/YxzhNode3C0F02CA4A7C/thing/model/down_config") == 0);
    s_business.run_requested = true;
    s_business.online = true;
    s_business.gateway_online = true;
    s_business.state = GW_BUSINESS_MQTT_STATE_ONLINE;
    s_business.client = &host_client;
    s_business.client_generation = 1U;
    s_business.callback_binding.generation = 1U;
}

static void test_fifo_capacity_rejects_new_without_callback(void)
{
    reset_transport();
    host_done_context_t contexts[33] = {0};
    for (size_t i = 0U; i < 32U; ++i) {
        contexts[i].index = (int)i;
        unsigned char payload = (unsigned char)i;
        assert(gw_business_mqtt_publish_gateway(&payload, 1U, host_gateway_done,
                                                &contexts[i]) == ESP_OK);
    }
    contexts[32].index = 32;
    unsigned char rejected = 99U;
    assert(gw_business_mqtt_publish_gateway(&rejected, 1U, host_gateway_done,
                                            &contexts[32]) == ESP_ERR_NO_MEM);
    assert(s_business.gateway_outbound_count == 32U && host_done_count == 0U);
    run_sender_until_idle();
    assert(host_attempt_count == 32U && host_done_count == 32U);
    assert(contexts[32].calls == 0U);
    for (size_t i = 0U; i < 32U; ++i) {
        assert(contexts[i].calls == 1U && contexts[i].result == ESP_OK);
        assert(host_done_order[i] == (int)i && host_attempts[i].first_byte == (unsigned char)i);
        assert(strcmp(host_attempts[i].topic, s_business.gateway_up_topic) == 0);
        if (i > 0U) {
            assert(host_attempts[i].time_us - host_attempts[i - 1U].time_us >= 1000000LL);
        }
    }
    cleanup_transport();
}

static void test_callback_reentry_and_old_topics_do_not_share_rate(void)
{
    reset_transport();
    host_done_context_t second = {.index = 2};
    host_done_context_t first = {
        .index = 1,
        .enqueue_from_callback = &second,
        .publish_old_from_callback = true,
    };
    assert(gw_business_mqtt_publish_gateway("A", 1U, host_gateway_done, &first) == ESP_OK);
    run_sender_until_idle();
    assert(first.calls == 1U && second.calls == 1U);
    assert(host_attempt_count == 2U && host_old_publish_count == 2U);
    assert(host_old_publish_times[0] == host_attempts[0].time_us);
    assert(host_old_publish_times[1] == host_attempts[0].time_us);
    assert(host_attempts[1].time_us - host_attempts[0].time_us >= 1000000LL);
    cleanup_transport();
}

static void test_failed_attempt_consumes_monotonic_interval(void)
{
    reset_transport();
    host_first_attempt_result = ESP_FAIL;
    host_done_context_t contexts[2] = {{.index = 0}, {.index = 1}};
    assert(gw_business_mqtt_publish_gateway("F", 1U, host_gateway_done, &contexts[0]) == ESP_OK);
    assert(gw_business_mqtt_publish_gateway("S", 1U, host_gateway_done, &contexts[1]) == ESP_OK);
    run_sender_until_idle();
    assert(host_attempt_count == 2U);
    assert(contexts[0].calls == 1U && contexts[0].result == ESP_FAIL);
    assert(contexts[1].calls == 1U && contexts[1].result == ESP_OK);
    assert(host_attempts[1].time_us - host_attempts[0].time_us >= 1000000LL);
    cleanup_transport();
}

static void test_disconnect_and_reconnect_drop_old_generation(void)
{
    reset_transport();
    host_done_context_t contexts[3] = {{.index = 0}, {.index = 1}, {.index = 2}};
    assert(gw_business_mqtt_publish_gateway("a", 1U, host_gateway_done, &contexts[0]) == ESP_OK);
    assert(gw_business_mqtt_publish_gateway("b", 1U, host_gateway_done, &contexts[1]) == ESP_OK);
    destroy_client();
    assert(host_destroy_count == 1U && !s_business.gateway_online);
    /* Reconnection can finish before the sender drains the previous session. */
    s_business.client = &host_client;
    ++s_business.client_generation;
    s_business.gateway_online = true;
    assert(gw_business_mqtt_publish_gateway("c", 1U, host_gateway_done, &contexts[2]) == ESP_OK);
    run_sender_until_idle();
    assert(host_attempt_count == 1U && host_attempts[0].first_byte == 'c');
    assert(contexts[0].calls == 1U && contexts[0].result == ESP_ERR_INVALID_STATE);
    assert(contexts[1].calls == 1U && contexts[1].result == ESP_ERR_INVALID_STATE);
    assert(contexts[2].calls == 1U && contexts[2].result == ESP_OK);
    assert(host_done_order[0] == 0 && host_done_order[1] == 1 && host_done_order[2] == 2);
    cleanup_transport();
}

static void test_rate_survives_reconnect_and_stop_drops_pending(void)
{
    reset_transport();
    host_done_context_t contexts[3] = {{.index = 0}, {.index = 1}, {.index = 2}};
    assert(gw_business_mqtt_publish_gateway("a", 1U, host_gateway_done, &contexts[0]) == ESP_OK);
    run_sender_until_idle();
    int64_t first_time = host_attempts[0].time_us;
    destroy_client();
    s_business.client = &host_client;
    ++s_business.client_generation;
    s_business.gateway_online = true;
    host_now_us += 200000LL;
    assert(gw_business_mqtt_publish_gateway("b", 1U, host_gateway_done, &contexts[1]) == ESP_OK);
    run_sender_until_idle();
    assert(host_attempt_count == 2U && host_attempts[1].time_us - first_time >= 1000000LL);
    assert(gw_business_mqtt_publish_gateway("c", 1U, host_gateway_done, &contexts[2]) == ESP_OK);
    s_business.run_requested = false;
    s_business.stop_requested = true;
    handle_explicit_stop();
    assert(s_business.state == GW_BUSINESS_MQTT_STATE_STOPPED);
    run_sender_until_idle();
    assert(host_attempt_count == 2U);
    assert(contexts[2].calls == 1U && contexts[2].result == ESP_ERR_INVALID_STATE);
    cleanup_transport();
}

static void test_enqueue_errors_do_not_invoke_callback(void)
{
    reset_transport();
    host_done_context_t done = {.index = 0};
    host_fail_allocation = true;
    assert(gw_business_mqtt_publish_gateway("x", 1U, host_gateway_done, &done) == ESP_ERR_NO_MEM);
    s_business.gateway_online = false;
    assert(gw_business_mqtt_publish_gateway("x", 1U, host_gateway_done, &done) == ESP_ERR_INVALID_STATE);
    s_business.gateway_online = true;
    assert(gw_business_mqtt_publish_gateway(NULL, 1U, host_gateway_done, &done) == ESP_ERR_INVALID_ARG);
    assert(gw_business_mqtt_publish_gateway("x", (size_t)INT_MAX + 1U,
                                            host_gateway_done, &done) == ESP_ERR_INVALID_SIZE);
    assert(done.calls == 0U && s_business.gateway_outbound_count == 0U);
    cleanup_transport();
}

static void emit_data(const char *topic, const void *payload, int data_len,
                      int total_len, int offset, bool retained)
{
    gw_mqtt_client_event_t event = {
        .event_id = GW_MQTT_CLIENT_EVENT_DATA,
        .msg_id = 7,
        .topic = topic,
        .topic_len = topic != NULL ? (int)strlen(topic) : 0,
        .data = payload,
        .data_len = data_len,
        .total_data_len = total_len,
        .current_data_offset = offset,
        .retained = retained,
    };
    business_mqtt_event_cb(&host_client, &event, &s_business.callback_binding);
    process_data_queue();
}

static void test_gateway_dynamic_fragments_preserve_first_retained(void)
{
    reset_transport();
    enum { LENGTH = 2 * GW_MQTT_MAX_PAYLOAD_SIZE + 17 };
    unsigned char *payload = malloc(LENGTH);
    assert(payload != NULL);
    for (size_t i = 0U; i < LENGTH; ++i) {
        payload[i] = (unsigned char)(i % 251U);
    }
    payload[0] = '{';
    emit_data(s_business.gateway_down_topic, payload, GW_MQTT_MAX_PAYLOAD_SIZE,
              LENGTH, 0, true);
    assert(s_business.fragment.active && s_business.fragment.total_len == LENGTH);
    emit_data(NULL, payload + GW_MQTT_MAX_PAYLOAD_SIZE, GW_MQTT_MAX_PAYLOAD_SIZE,
              LENGTH, GW_MQTT_MAX_PAYLOAD_SIZE, false);
    emit_data(NULL, payload + 2 * GW_MQTT_MAX_PAYLOAD_SIZE, 17,
              LENGTH, 2 * GW_MQTT_MAX_PAYLOAD_SIZE, false);
    assert(!s_business.fragment.active);
    assert(uxQueueMessagesWaiting(s_business.downlink_queue) == 0U);
    assert(uxQueueMessagesWaiting(s_business.management_queue) == 0U);
    gw_business_mqtt_owned_message_t message = {0};
    assert(gw_business_mqtt_recv_gateway(&message, 0U) == ESP_OK);
    assert(message.retained && message.payload_len == LENGTH);
    assert(strcmp(message.topic, s_business.gateway_down_topic) == 0);
    assert(memcmp(message.payload, payload, LENGTH) == 0);
    assert(message.payload[LENGTH] == 0U);
    gw_business_mqtt_message_release(&message);
    assert(message.topic == NULL && message.payload == NULL && !message.retained);
    assert(gw_business_mqtt_recv_gateway(&message, 0U) == ESP_ERR_TIMEOUT);
    free(payload);
    cleanup_transport();
}

static void test_old_size_limit_and_routing_remain(void)
{
    reset_transport();
    unsigned char *oversized = calloc(1U, GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    assert(oversized != NULL);
    emit_data(s_business.down_topic, oversized, GW_MQTT_MAX_PAYLOAD_SIZE + 1,
              GW_MQTT_MAX_PAYLOAD_SIZE + 1, 0, false);
    assert(s_business.downlink_dropped_count == 1U);
    assert(uxQueueMessagesWaiting(s_business.downlink_queue) == 0U);
    assert(uxQueueMessagesWaiting(s_business.management_queue) == 0U);
    assert(uxQueueMessagesWaiting(s_business.gateway_queue) == 0U);
    free(oversized);

    unsigned char binary[] = {0x01U, 0x02U, 0x03U};
    emit_data(s_business.down_topic, binary, 3, 3, 0, false);
    emit_data(s_business.down_topic, "{\"legacy\":1}", 12, 12, 0, true);
    emit_data(s_business.down_config_topic, binary, 3, 3, 0, false);
    emit_data(s_business.gateway_down_topic, binary, 3, 3, 0, true);
    assert(uxQueueMessagesWaiting(s_business.downlink_queue) == 1U);
    assert(uxQueueMessagesWaiting(s_business.management_queue) == 2U);
    assert(uxQueueMessagesWaiting(s_business.gateway_queue) == 1U);
    gw_business_mqtt_owned_message_t message = {0};
    assert(gw_business_mqtt_recv_raw(&message, 0U) == ESP_OK);
    assert(message.payload_len == 3U && !message.retained);
    gw_business_mqtt_message_release(&message);
    assert(gw_business_mqtt_recv_management(&message, 0U) == ESP_OK);
    assert(strcmp(message.topic, s_business.down_topic) == 0 && message.retained);
    gw_business_mqtt_message_release(&message);
    assert(gw_business_mqtt_recv_management(&message, 0U) == ESP_OK);
    assert(strcmp(message.topic, s_business.down_config_topic) == 0);
    gw_business_mqtt_message_release(&message);
    assert(gw_business_mqtt_recv_gateway(&message, 0U) == ESP_OK);
    assert(message.payload_len == 3U && message.retained);
    gw_business_mqtt_message_release(&message);
    cleanup_transport();
}

static void test_gateway_suback_is_independent_from_old_gate(void)
{
    reset_transport();
    s_business.state = GW_BUSINESS_MQTT_STATE_CONNECTING;
    s_business.online = false;
    s_business.gateway_online = false;
    business_mqtt_event_t event = {
        .source_client = &host_client,
        .generation = s_business.client_generation,
        .event_id = GW_MQTT_CLIENT_EVENT_CONNECTED,
    };
    assert(process_mqtt_event(&event));
    assert(host_subscribe_count == 2U);
    assert(host_subscribe_qos[0] == 1 && host_subscribe_qos[1] == 0);
    assert(strcmp(host_subscribe_topics[0], s_business.down_topic) == 0);
    assert(strcmp(host_subscribe_topics[1], s_business.gateway_down_topic) == 0);
    host_done_context_t done = {.index = 0};
    assert(gw_business_mqtt_publish_gateway("a", 1U, host_gateway_done, &done) == ESP_ERR_INVALID_STATE);
    event.event_id = GW_MQTT_CLIENT_EVENT_SUBSCRIBED;
    event.msg_id = s_business.gateway_subscribe_msg_id;
    assert(process_mqtt_event(&event));
    assert(s_business.gateway_online && !s_business.online);
    assert(s_business.state == GW_BUSINESS_MQTT_STATE_SUBSCRIBING);
    assert(gw_business_mqtt_publish_gateway("a", 1U, host_gateway_done, &done) == ESP_OK);
    run_sender_until_idle();
    assert(done.calls == 1U && host_attempt_count == 1U);
    event.msg_id = s_business.subscribe_msg_id;
    assert(process_mqtt_event(&event));
    assert(host_subscribe_count == 3U && host_subscribe_qos[2] == 1);
    assert(strcmp(host_subscribe_topics[2], s_business.down_config_topic) == 0);
    assert(!s_business.online);
    event.msg_id = s_business.subscribe_msg_id;
    assert(process_mqtt_event(&event));
    assert(s_business.online && s_business.state == GW_BUSINESS_MQTT_STATE_ONLINE);
    cleanup_transport();
}

static void test_large_gateway_publish_and_subscription_failure_leave_old_online(void)
{
    reset_transport();
    size_t length = GW_MQTT_MAX_PAYLOAD_SIZE + 100U;
    unsigned char *payload = calloc(1U, length);
    assert(payload != NULL);
    payload[0] = 'L';
    host_done_context_t done = {.index = 0};
    assert(gw_business_mqtt_publish_gateway(payload, length, host_gateway_done, &done) == ESP_OK);
    free(payload);
    run_sender_until_idle();
    assert(host_attempt_count == 1U && host_attempts[0].payload_len == length);
    assert(done.calls == 1U && done.result == ESP_OK);

    s_business.gateway_online = false;
    begin_gateway_subscribe();
    business_mqtt_event_t event = {
        .source_client = &host_client,
        .generation = s_business.client_generation,
        .event_id = GW_MQTT_CLIENT_EVENT_SUBSCRIBED,
        .msg_id = s_business.gateway_subscribe_msg_id,
        .subscribe_failed = true,
    };
    assert(process_mqtt_event(&event));
    assert(!s_business.gateway_online && s_business.online);
    assert(s_business.client == &host_client && host_destroy_count == 0U);
    assert(s_business.state == GW_BUSINESS_MQTT_STATE_ONLINE);

    begin_gateway_subscribe();
    host_now_us += (int64_t)GW_BUSINESS_MQTT_SUBSCRIBE_TIMEOUT_MS * 1000LL;
    s_business.credentials_valid = true;
    s_business.network_ready_seen = true;
    worker_step();
    assert(s_business.gateway_subscribe_msg_id == -1 && !s_business.gateway_online);
    assert(s_business.online && s_business.client == &host_client && host_destroy_count == 0U);
    assert(s_business.state == GW_BUSINESS_MQTT_STATE_ONLINE);
    cleanup_transport();
}

int main(void)
{
    unsigned count = 0U;
#define RUN(test) do { test(); ++count; puts(#test " passed"); } while (0)
    RUN(test_fifo_capacity_rejects_new_without_callback);
    RUN(test_callback_reentry_and_old_topics_do_not_share_rate);
    RUN(test_failed_attempt_consumes_monotonic_interval);
    RUN(test_disconnect_and_reconnect_drop_old_generation);
    RUN(test_rate_survives_reconnect_and_stop_drops_pending);
    RUN(test_enqueue_errors_do_not_invoke_callback);
    RUN(test_gateway_dynamic_fragments_preserve_first_retained);
    RUN(test_old_size_limit_and_routing_remain);
    RUN(test_gateway_suback_is_independent_from_old_gate);
    RUN(test_large_gateway_publish_and_subscription_failure_leave_old_online);
#undef RUN
    printf("Gateway transport: %u tests passed\n", count);
    return 0;
}
