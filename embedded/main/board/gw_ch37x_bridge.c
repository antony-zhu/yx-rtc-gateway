/**
 * @file gw_ch37x_bridge.c
 * @brief CH37X host 与脚本层之间的桥接实现。
 */

#include "gw_ch37x_bridge.h"

#include <string.h>

#include "ch37x.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "gw_ch37x";

#ifdef GW_CH37X_BRIDGE_ENABLE_TEST_HOOKS
/* 测试工程用 fake 分配和 fake host task 隔离桥接状态机，正式固件不启用。 */
uint8_t *gw_ch37x_bridge_test_alloc_queue(size_t size);
void gw_ch37x_bridge_test_free_queue(void *ptr);
BaseType_t gw_ch37x_bridge_test_create_host_task(TaskHandle_t *out_handle);
#define CH37X_BRIDGE_QUEUE_ALLOC(size) gw_ch37x_bridge_test_alloc_queue(size)
#define CH37X_BRIDGE_QUEUE_FREE(ptr) gw_ch37x_bridge_test_free_queue(ptr)
#define CH37X_BRIDGE_CREATE_HOST_TASK(out_handle) gw_ch37x_bridge_test_create_host_task(out_handle)
#else
#define CH37X_BRIDGE_QUEUE_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CH37X_BRIDGE_QUEUE_FREE(ptr) heap_caps_free(ptr)
#define CH37X_BRIDGE_CREATE_HOST_TASK(out_handle) \
    xTaskCreate(ch37x_main, \
                "gw_ch37x_host", \
                GW_CH37X_HOST_TASK_STACK, \
                NULL, \
                GW_CH37X_HOST_TASK_PRIORITY, \
                (out_handle))
#endif

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buffer;
    QueueHandle_t rx_queue;
    StaticQueue_t rx_queue_buffer;
    uint8_t *rx_queue_storage;
    TaskHandle_t host_task_handle;
    volatile bool configured;
    volatile bool active;
    volatile bool host_started;
    volatile bool ready;
    volatile uint8_t interface_num;
    volatile uint32_t rx_drop_count;
    volatile uint32_t rx_bytes;
    volatile uint32_t tx_bytes;
    gw_ch37x_bridge_observer_t observer;
} gw_ch37x_bridge_state_t;

static gw_ch37x_bridge_state_t s_ch37x = {
    .interface_num = GW_CH37X_DEFAULT_INTERFACE,
};

/** @brief 获取桥接层互斥锁，首次使用时静态创建。 */
static SemaphoreHandle_t bridge_get_lock(void)
{
    if (s_ch37x.lock == NULL) {
        s_ch37x.lock = xSemaphoreCreateMutexStatic(&s_ch37x.lock_buffer);
    }
    return s_ch37x.lock;
}

/** @brief 加锁桥接状态。 */
static bool bridge_lock(void)
{
    SemaphoreHandle_t lock = bridge_get_lock();
    return lock != NULL && xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE;
}

/** @brief 按调用方预算获取桥接锁，tick 始终向上取整。 */
static bool bridge_lock_timeout(uint32_t timeout_ms)
{
    SemaphoreHandle_t lock = bridge_get_lock();
    if (lock == NULL) {
        return false;
    }
    if (timeout_ms == 0U) {
        return xSemaphoreTake(lock, 0) == pdTRUE;
    }

    uint64_t ticks = ((uint64_t)timeout_ms * (uint64_t)configTICK_RATE_HZ + 999ULL) / 1000ULL;
    if (ticks == 0U) {
        ticks = 1U;
    }
    if (ticks >= (uint64_t)portMAX_DELAY) {
        ticks = (uint64_t)portMAX_DELAY - 1ULL;
    }
    return xSemaphoreTake(lock, (TickType_t)ticks) == pdTRUE;
}

/** @brief 解锁桥接状态。 */
static void bridge_unlock(void)
{
    if (s_ch37x.lock != NULL) {
        (void)xSemaphoreGive(s_ch37x.lock);
    }
}

/** @brief 通知上层观察者清空协议残包。 */
static void notify_reset(const gw_ch37x_bridge_observer_t *observer)
{
    if (observer != NULL && observer->on_reset != NULL) {
        observer->on_reset(observer->ctx);
    }
}

/** @brief 清空脚本层接收队列，避免设备重连后误读旧字节。 */
static void flush_rx_queue_locked(void)
{
    uint8_t dummy = 0U;

    if (s_ch37x.rx_queue == NULL) {
        return;
    }
    while (xQueueReceive(s_ch37x.rx_queue, &dummy, 0) == pdTRUE) {
    }
}

/** @brief 确保接收队列存在；队列存储放在 PSRAM。 */
static esp_err_t ensure_rx_queue_locked(void)
{
    if (s_ch37x.rx_queue != NULL) {
        return ESP_OK;
    }

    s_ch37x.rx_queue_storage = CH37X_BRIDGE_QUEUE_ALLOC(GW_CH37X_RX_QUEUE_LEN);
    if (s_ch37x.rx_queue_storage == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_ch37x.rx_queue = xQueueCreateStatic((UBaseType_t)GW_CH37X_RX_QUEUE_LEN,
                                          sizeof(uint8_t),
                                          s_ch37x.rx_queue_storage,
                                          &s_ch37x.rx_queue_buffer);
    if (s_ch37x.rx_queue == NULL) {
        CH37X_BRIDGE_QUEUE_FREE(s_ch37x.rx_queue_storage);
        s_ch37x.rx_queue_storage = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** @brief 判断目标 interface 是否枚举出了可用端点。 */
static bool interface_has_endpoint(const ch37x_device_t *device, uint8_t interface_num)
{
    if (device == NULL || interface_num > GW_CH37X_MAX_INTERFACE) {
        return false;
    }
    return device->interfaces[interface_num].in_endp_addr != 0U ||
           device->interfaces[interface_num].out_endp_addr != 0U ||
           device->interfaces[interface_num].int_endp_addr != 0U;
}

/** @brief 根据底层 USB 状态刷新脚本层 ready 标志。 */
static bool refresh_ready_locked(void)
{
    ch37x_device_t device = {0};

    if (!s_ch37x.active || !ch37x_get_device_info(&device)) {
        s_ch37x.ready = false;
        return false;
    }
    if (device.state != CH37X_DEV_STATE_CONFIGURED ||
        !interface_has_endpoint(&device, s_ch37x.interface_num)) {
        s_ch37x.ready = false;
        return false;
    }

    s_ch37x.ready = true;
    return true;
}

/**
 * @brief CH37X 事件回调。
 *
 * 回调在底层 host 任务上下文执行，只做状态更新、清队列和日志，不执行 Python。
 */
static void bridge_event_callback(ch37x_event_t *event)
{
    gw_ch37x_bridge_observer_t observer = {0};
    bool should_notify_reset = false;

    if (event == NULL) {
        return;
    }

    if (bridge_lock()) {
        observer = s_ch37x.observer;
        switch (event->type) {
        case CH37X_EVENT_DEVICE_CONNECT:
            s_ch37x.ready = false;
            flush_rx_queue_locked();
            should_notify_reset = true;
            break;
        case CH37X_EVENT_DEVICE_READY:
            /*
             * READY 由事件任务异步派发，首个 IN 包可能已经先到达数据回调。CONNECT
             * 阶段已清理旧连接残包，此处只刷新状态，不能再清队列或重置解析器。
             */
            (void)refresh_ready_locked();
            break;
        case CH37X_EVENT_DEVICE_DISCONNECT:
            s_ch37x.ready = false;
            flush_rx_queue_locked();
            should_notify_reset = true;
            break;
        default:
            break;
        }
        bridge_unlock();
    }

    if (should_notify_reset) {
        notify_reset(&observer);
    }

    if (event->type == CH37X_EVENT_DEVICE_READY) {
        ESP_LOGI(TAG,
                 "ch37x ready: intf=%u vid=0x%04X pid=0x%04X class=0x%02X speed=%u",
                 (unsigned)s_ch37x.interface_num,
                 (unsigned)event->vid,
                 (unsigned)event->pid,
                 (unsigned)event->device_class,
                 (unsigned)event->speed);
    } else if (event->type == CH37X_EVENT_DEVICE_DISCONNECT) {
        ESP_LOGW(TAG, "ch37x disconnected");
    } else if (event->type == CH37X_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG,
                 "ch37x transfer error: intf=%u code=0x%02X",
                 (unsigned)event->interface_num,
                 (unsigned)event->error_code);
    }
}

/**
 * @brief CH37X 原始数据回调。
 *
 * 如果 C 层 observer 已注册，字节流直接镜像给 observer；否则进入脚本读队列。
 */
static int bridge_data_callback(uint8_t interface_num, const uint8_t *data, uint32_t data_len)
{
    if (data == NULL || data_len == 0U || s_ch37x.rx_queue == NULL) {
        return -1;
    }
    if (!s_ch37x.active || interface_num != s_ch37x.interface_num) {
        return 0;
    }

    gw_ch37x_bridge_observer_t observer = s_ch37x.observer;
    bool mirror_to_queue = observer.on_data == NULL;

    if (mirror_to_queue) {
        for (uint32_t i = 0U; i < data_len; ++i) {
            if (xQueueSend(s_ch37x.rx_queue, &data[i], 0) != pdTRUE) {
                s_ch37x.rx_drop_count += data_len - i;
                if ((s_ch37x.rx_drop_count & 0x3FU) == 1U) {
                    ESP_LOGW(TAG,
                             "ch37x rx queue full: intf=%u dropped=%lu",
                             (unsigned)interface_num,
                             (unsigned long)s_ch37x.rx_drop_count);
                }
                break;
            }
        }
    }
    s_ch37x.rx_bytes += data_len;

    if (observer.on_data != NULL) {
        observer.on_data(interface_num, data, (size_t)data_len, observer.ctx);
    }
    return 0;
}

/** @brief 启动底层 CH37X host 任务。 */
static esp_err_t start_host_locked(void)
{
    ch37x_set_event_callback(bridge_event_callback);
    ch37x_set_data_callback(bridge_data_callback);

    if (s_ch37x.host_started) {
        return ESP_OK;
    }

    BaseType_t created = CH37X_BRIDGE_CREATE_HOST_TASK(&s_ch37x.host_task_handle);
    if (created != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    s_ch37x.host_started = true;
    ESP_LOGI(TAG,
             "ch37x host started: stack=%u interface=%u",
             (unsigned)GW_CH37X_HOST_TASK_STACK,
             (unsigned)s_ch37x.interface_num);
    return ESP_OK;
}

esp_err_t gw_ch37x_bridge_configure(uint8_t interface_num)
{
    if (interface_num > GW_CH37X_MAX_INTERFACE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bridge_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    s_ch37x.configured = true;
    s_ch37x.interface_num = interface_num;
    gw_ch37x_bridge_observer_t observer = s_ch37x.observer;
    flush_rx_queue_locked();
    (void)refresh_ready_locked();
    bridge_unlock();

    notify_reset(&observer);
    ESP_LOGI(TAG, "ch37x configured: interface=%u", (unsigned)interface_num);
    return ESP_OK;
}

esp_err_t gw_ch37x_bridge_start(void)
{
    if (!bridge_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_ch37x.configured) {
        s_ch37x.configured = true;
        s_ch37x.interface_num = GW_CH37X_DEFAULT_INTERFACE;
    }

    esp_err_t err = ensure_rx_queue_locked();
    if (err == ESP_OK) {
        err = start_host_locked();
    }
    gw_ch37x_bridge_observer_t observer = {0};
    if (err == ESP_OK) {
        s_ch37x.active = true;
        observer = s_ch37x.observer;
        (void)refresh_ready_locked();
    }

    bridge_unlock();
    if (err == ESP_OK) {
        notify_reset(&observer);
    }
    return err;
}

void gw_ch37x_bridge_stop(void)
{
    gw_ch37x_bridge_observer_t observer = {0};

    if (!bridge_lock()) {
        return;
    }
    s_ch37x.active = false;
    s_ch37x.ready = false;
    observer = s_ch37x.observer;
    flush_rx_queue_locked();
    bridge_unlock();
    notify_reset(&observer);
}

void gw_ch37x_bridge_destroy(void)
{
    gw_ch37x_bridge_observer_t observer = {0};

    if (!bridge_lock()) {
        return;
    }
    s_ch37x.configured = false;
    s_ch37x.active = false;
    s_ch37x.ready = false;
    s_ch37x.interface_num = GW_CH37X_DEFAULT_INTERFACE;
    s_ch37x.rx_drop_count = 0U;
    s_ch37x.rx_bytes = 0U;
    s_ch37x.tx_bytes = 0U;
    observer = s_ch37x.observer;
    flush_rx_queue_locked();
    bridge_unlock();
    notify_reset(&observer);
}

esp_err_t gw_ch37x_bridge_disconnect(void)
{
    gw_ch37x_bridge_observer_t observer = {0};

    ch37x_disconnect_device();
    if (bridge_lock()) {
        s_ch37x.ready = false;
        observer = s_ch37x.observer;
        flush_rx_queue_locked();
        bridge_unlock();
    }
    notify_reset(&observer);
    return ESP_OK;
}

bool gw_ch37x_bridge_is_ready(void)
{
    return s_ch37x.active && s_ch37x.ready;
}

size_t gw_ch37x_bridge_pending(void)
{
    if (s_ch37x.rx_queue == NULL) {
        return 0U;
    }
    return (size_t)uxQueueMessagesWaiting(s_ch37x.rx_queue);
}

int gw_ch37x_bridge_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    if (buf == NULL || cap == 0U) {
        return -1;
    }
    if (!s_ch37x.active) {
        return -1;
    }
    if (!s_ch37x.ready || s_ch37x.rx_queue == NULL) {
        return 0;
    }

    TickType_t wait_ticks = timeout_ms == 0U ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0U && wait_ticks == 0) {
        wait_ticks = 1;
    }

    size_t received = 0U;
    uint8_t byte = 0U;
    if (xQueueReceive(s_ch37x.rx_queue, &byte, wait_ticks) != pdTRUE) {
        return 0;
    }
    buf[received++] = byte;

    while (received < cap) {
        if (xQueueReceive(s_ch37x.rx_queue, &byte, 0) != pdTRUE) {
            break;
        }
        buf[received++] = byte;
    }
    return (int)received;
}

esp_err_t gw_ch37x_bridge_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ch37x.active || !s_ch37x.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t deadline_us = (uint64_t)esp_timer_get_time() +
                           ((uint64_t)timeout_ms * 1000ULL);
    if (!bridge_lock_timeout(timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_ch37x.active || !s_ch37x.ready) {
        bridge_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t remaining_ms = 0U;
    if (timeout_ms > 0U) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (now_us >= deadline_us) {
            bridge_unlock();
            return ESP_ERR_TIMEOUT;
        }
        remaining_ms = (uint32_t)((deadline_us - now_us + 999ULL) / 1000ULL);
    }

    int written = ch37x_write_timeout((int)s_ch37x.interface_num,
                                      data,
                                      (uint32_t)len,
                                      remaining_ms);
    if (written >= 0 && (size_t)written == len) {
        s_ch37x.tx_bytes += (uint32_t)len;
        bridge_unlock();
        return ESP_OK;
    }

    bridge_unlock();
    if (written == CH37X_WRITE_ERR_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGW(TAG,
             "ch37x write failed: interface=%u len=%u ret=%d",
             (unsigned)s_ch37x.interface_num,
             (unsigned)len,
             written);
    return ESP_FAIL;
}

esp_err_t gw_ch37x_bridge_get_info(gw_ch37x_bridge_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_info, 0, sizeof(*out_info));
    if (!bridge_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    out_info->configured = s_ch37x.configured;
    out_info->active = s_ch37x.active;
    out_info->host_started = s_ch37x.host_started;
    out_info->ready = s_ch37x.ready;
    out_info->interface_num = s_ch37x.interface_num;
    out_info->pending_bytes = s_ch37x.rx_queue != NULL ?
                              (size_t)uxQueueMessagesWaiting(s_ch37x.rx_queue) :
                              0U;
    out_info->queue_capacity = GW_CH37X_RX_QUEUE_LEN;
    out_info->rx_drop_count = s_ch37x.rx_drop_count;
    out_info->rx_bytes = s_ch37x.rx_bytes;
    out_info->tx_bytes = s_ch37x.tx_bytes;
    bridge_unlock();

    ch37x_device_t device = {0};
    if (ch37x_get_device_info(&device)) {
        out_info->device_state = device.state;
        out_info->vid = device.vid;
        out_info->pid = device.pid;
        out_info->device_class = device.device_class;
        out_info->speed = device.speed;
        out_info->num_interfaces = device.num_interfaces;
        out_info->config_value = device.config_value;

        if (out_info->interface_num <= GW_CH37X_MAX_INTERFACE) {
            out_info->in_endpoint = device.interfaces[out_info->interface_num].in_endp_addr;
            out_info->out_endpoint = device.interfaces[out_info->interface_num].out_endp_addr;
            out_info->int_endpoint = device.interfaces[out_info->interface_num].int_endp_addr;
            out_info->in_max_packet = device.interfaces[out_info->interface_num].in_endp_size;
            out_info->out_max_packet = device.interfaces[out_info->interface_num].out_endp_size;
            out_info->int_max_packet = device.interfaces[out_info->interface_num].int_endp_size;
        }
    } else {
        out_info->device_state = ch37x_get_device_state();
    }

    return ESP_OK;
}

esp_err_t gw_ch37x_bridge_set_observer(const gw_ch37x_bridge_observer_t *observer)
{
    if (!bridge_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if (observer != NULL) {
        s_ch37x.observer = *observer;
    } else {
        memset(&s_ch37x.observer, 0, sizeof(s_ch37x.observer));
    }
    bridge_unlock();

    if (observer != NULL && observer->on_reset != NULL) {
        observer->on_reset(observer->ctx);
    }
    return ESP_OK;
}

#ifdef GW_CH37X_BRIDGE_ENABLE_TEST_HOOKS
void gw_ch37x_bridge_test_reset(void)
{
    if (s_ch37x.rx_queue != NULL) {
        vQueueDelete(s_ch37x.rx_queue);
    }
    if (s_ch37x.rx_queue_storage != NULL) {
        CH37X_BRIDGE_QUEUE_FREE(s_ch37x.rx_queue_storage);
    }
    if (s_ch37x.lock != NULL) {
        vSemaphoreDelete(s_ch37x.lock);
    }

    memset(&s_ch37x, 0, sizeof(s_ch37x));
    s_ch37x.interface_num = GW_CH37X_DEFAULT_INTERFACE;
}
#endif
