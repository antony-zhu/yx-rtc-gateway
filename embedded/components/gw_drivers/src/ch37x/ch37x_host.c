/* CH37x USB Host 实现 - 优化版 */
/** @brief 模块化、事件驱动、支持多接口的旧 CH37x host 移植实现。 */

#include <stdio.h>
/**
 * @file ch37x_host.c
 * @brief CH37X USB host 枚举、传输和事件派发实现。
 *
 * 该文件直接与 CH37X 寄存器和 FIFO 交互，负责 USB 设备检测、复位、地址分配、
 * 配置描述符解析以及指定 interface 的批量端点读写。
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ch37x.h"
#include "ch37x_inc.h"
#include "ch37x_port.h"
#include "ch37x_port_spi.h"

static const char *TAG = "ch37x_host";

#define CH37X_EVENT_TASK_STACK 8192
/** @brief 内部传输状态：调用方绝对截止时间已到。 */
#define CH37X_USB_INT_DEADLINE 0xFBU

/** @brief 当前枚举出的 USB 设备状态缓存。 */
static ch37x_device_t g_device = {0};

/** @brief 上层事件回调，在 CH37X host 任务上下文触发。 */
static ch37x_event_callback_t g_event_callback = NULL;

/** @brief 上层数据回调，在 USB IN 数据读取后触发。 */
static ch37x_data_callback_t g_data_callback = NULL;

/** @brief 保护 g_device 和端点 toggle 状态的互斥锁。 */
static SemaphoreHandle_t g_device_mutex = NULL;

/** @brief CH37X 事件队列，由 event_dispatch_task 串行派发。 */
static QueueHandle_t g_event_queue = NULL;

/** @brief 控制 USB 读任务退出的标志。 */
static volatile bool g_stop_reading = false;

/** @brief USB 设备端点 0 的最大包尺寸，枚举过程中会按设备描述符更新。 */
static uint8_t UsbDevEndpSize = DEFAULT_ENDP0_SIZE;

/** @brief USB 标准请求：获取设备描述符。 */
static const uint8_t SetupGetDevDescr[] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00};

/** @brief USB 标准请求：获取配置描述符头部。 */
static const uint8_t SetupGetCfgDescr[] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x04, 0x00};

/** @brief USB 标准请求：设置设备地址。 */
static const uint8_t SetupSetUsbAddr[] = {0x00, 0x05, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};

/** @brief USB 标准请求：设置当前配置。 */
static const uint8_t SetupSetUsbConfig[] = {0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void init_device_state(void);
static void set_device_state(ch37x_device_state_t state);
static void post_event(ch37x_event_type_t type);
static void post_event_ex(ch37x_event_t *event);

static uint8_t HostTransact374(uint8_t endp_addr,
                               uint8_t pid,
                               bool tog,
                               uint64_t deadline_us,
                               bool deadline_enabled);
static uint8_t WaitHostTransact374(uint8_t endp_addr, uint8_t pid, bool tog, uint16_t timeout_ms);
static uint8_t HostCtrlTransfer374(const uint8_t *ReqBuf, uint8_t *DatBuf, uint8_t *RetLen);

static bool Query374DeviceIn(void);
static bool Query374DevFullSpeed(void);
static bool Query374Interrupt(void);
static uint8_t Wait374Interrupt(uint64_t deadline_us, bool deadline_enabled);

static void HostDetectInterrupt(void);
static void Init374Host(void);
static void HostSetBusFree(void);
static void HostSetBusReset(void);
static void HostSetFullSpeed(void);
static void HostSetLowSpeed(void);
static void SetHostUsbAddr(uint8_t addr);

static uint8_t GetDeviceDescr(uint8_t *buf);
static uint8_t GetConfigDescr(uint8_t *buf);
static uint8_t SetUsbAddress(uint8_t addr);
static uint8_t SetUsbConfig(uint8_t cfg);
static uint8_t ParseConfigDescriptor(uint8_t *buf, uint16_t total_len);

static void usb_enumerate_task(void);
static void usb_read_task(void);
static void event_dispatch_task(void *args);

/**
 * @brief 获取设备状态互斥锁。
 *
 * @param timeout_ticks 等待时间，单位 FreeRTOS tick。
 * @return true 表示加锁成功。
 */
static bool device_lock(TickType_t timeout_ticks)
{
    if (g_device_mutex == NULL) {
        ESP_LOGW(TAG, "Device mutex not initialized");
        return false;
    }
    return (xSemaphoreTake(g_device_mutex, timeout_ticks) == pdTRUE);
}

/** @brief 释放设备状态互斥锁。 */
static void device_unlock(void)
{
    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }
}

/** @brief 设置 CH37X 事件回调。 */
void ch37x_set_event_callback(ch37x_event_callback_t callback)
{
    g_event_callback = callback;
}

/** @brief 将毫秒等待时间向上取整为 tick，避免 100 Hz 时钟下提前超时。 */
static TickType_t timeout_ms_to_ticks_ceil(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return 0;
    }

    uint64_t ticks = ((uint64_t)timeout_ms * (uint64_t)configTICK_RATE_HZ + 999ULL) / 1000ULL;
    if (ticks == 0U) {
        ticks = 1U;
    }
    if (ticks >= (uint64_t)portMAX_DELAY) {
        ticks = (uint64_t)portMAX_DELAY - 1ULL;
    }
    return (TickType_t)ticks;
}

/** @brief 设置 CH37X 数据回调。 */
void ch37x_set_data_callback(ch37x_data_callback_t callback)
{
    g_data_callback = callback;
}

/**
 * @brief 向指定 interface 的 OUT 端点写入数据。
 *
 * 数据会按端点最大包大小分块发送，并维护 OUT DATA toggle。设备未完成配置或接口
 * 没有 OUT 端点时返回负值。
 */
int ch37x_write_timeout(int interface_num,
                        const uint8_t *data,
                        uint32_t data_len,
                        uint32_t timeout_ms)
{
    if (interface_num < 0 || interface_num >= 4) {
        ESP_LOGE(TAG, "Invalid interface number: %d", interface_num);
        return -1;
    }
    
    if (data == NULL || data_len == 0) {
        ESP_LOGE(TAG, "Invalid data or length");
        return -2;
    }
    
    const bool no_wait = timeout_ms == 0U;
    const uint64_t deadline_us = (uint64_t)esp_timer_get_time() +
                                 ((uint64_t)timeout_ms * 1000ULL);

    // 发送与 IN 轮询共用设备锁，剩余预算会继续传给 OUT 事务。
    if (!device_lock(timeout_ms_to_ticks_ceil(timeout_ms))) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return CH37X_WRITE_ERR_TIMEOUT;
    }
    
    // 检查设备状态
    if (g_device.state != CH37X_DEV_STATE_CONFIGURED) {
        ESP_LOGW(TAG, "Device not ready (state=%d)", g_device.state);
        device_unlock();
        return -4;
    }
    
    // 检查端点是否有效
    uint8_t out_endp = g_device.interfaces[interface_num].out_endp_addr;
    uint16_t max_packet_size = g_device.interfaces[interface_num].out_endp_size;
    
    if (out_endp == 0) {
        ESP_LOGE(TAG, "No OUT endpoint for interface %d", interface_num);
        device_unlock();
        return -5;
    }

    if (max_packet_size == 0) {
        ESP_LOGE(TAG, "Invalid OUT max packet size for interface %d", interface_num);
        device_unlock();
        return -6;
    }
    
    // 分块发送数据
    uint32_t sent = 0;
    uint8_t s;
    
    while (sent < data_len) {
        uint16_t transaction_timeout_ms = 0U;
        if (!no_wait) {
            uint64_t now_us = (uint64_t)esp_timer_get_time();
            if (now_us >= deadline_us) {
                device_unlock();
                return CH37X_WRITE_ERR_TIMEOUT;
            }
            uint64_t remaining_ms = (deadline_us - now_us + 999ULL) / 1000ULL;
            if (remaining_ms > 500ULL) {
                remaining_ms = 500ULL;
            }
            transaction_timeout_ms = (uint16_t)remaining_ms;
        }

        uint32_t chunk_size = (data_len - sent) > max_packet_size ? max_packet_size : (data_len - sent);
        
        // 写入发送缓冲区
        if (ch37x_port_spi_write_block(RAM_HOST_TRAN, (uint8_t)chunk_size, data + sent) < 0 ||
            ch37x_port_spi_write_byte(REG_USB_LENGTH, (uint8_t)chunk_size) < 0) {
            ESP_LOGE(TAG, "SPI write failed while sending data");
            device_unlock();
            return -(int)sent;
        }
        
        // 执行 OUT 传输
        s = WaitHostTransact374(
            out_endp,
            DEF_USB_PID_OUT,
            g_device.interfaces[interface_num].out_endp_tog,
            transaction_timeout_ms
        );
        
        if (s != USB_INT_SUCCESS) {
            ESP_LOGE(TAG, "Write failed: endpoint=0x%02X, error=0x%02X", out_endp, s);
            device_unlock();
            if (s == (DEF_USB_PID_NAK | 0x20) || s == CH37X_USB_INT_DEADLINE) {
                return CH37X_WRITE_ERR_TIMEOUT;
            }
            return -(int)sent;
        }
        
        // 翻转同步标志
        g_device.interfaces[interface_num].out_endp_tog = !g_device.interfaces[interface_num].out_endp_tog;
        
        sent += chunk_size;
        
        // 更新最后活动时间
        g_device.last_activity_time = esp_timer_get_time() / 1000;
    }
    
    device_unlock();
    
    ESP_LOGD(TAG, "Write success: interface=%d, sent=%lu bytes", interface_num, sent);
    return (int)sent;
}

int ch37x_write(int interface_num, const uint8_t *data, uint32_t data_len)
{
    return ch37x_write_timeout(interface_num, data, data_len, 1000U);
}

/**
 * @brief 获取当前 CH37X 设备状态。
 *
 * @return 当前设备状态；无法获取互斥锁时返回 CH37X_DEV_STATE_ERROR。
 */
ch37x_device_state_t ch37x_get_device_state(void)
{
    ch37x_device_state_t state;
    
    if (device_lock(pdMS_TO_TICKS(100))) {
        state = g_device.state;
        device_unlock();
    } else {
        state = CH37X_DEV_STATE_ERROR;
    }
    
    return state;
}

/**
 * @brief 获取当前 USB 设备信息快照。
 *
 * @param device 输出设备信息，不能为空。
 * @return true 表示成功复制快照。
 */
bool ch37x_get_device_info(ch37x_device_t *device)
{
    if (device == NULL) {
        return false;
    }
    
    if (device_lock(pdMS_TO_TICKS(100))) {
        memcpy(device, &g_device, sizeof(ch37x_device_t));
        device_unlock();
        return true;
    }
    
    return false;
}

/**
 * @brief 主动标记设备断开。
 *
 * 该函数用于上层要求停止读任务或清理当前设备状态，不直接释放 SPI 端口。
 */
void ch37x_disconnect_device(void)
{
    if (device_lock(pdMS_TO_TICKS(1000))) {
        g_stop_reading = true;
        set_device_state(CH37X_DEV_STATE_DISCONNECTED);
        device_unlock();
    }
}

/**
 * @brief CH37X host 主任务入口。
 *
 * 该函数内部创建事件队列、初始化 SPI 和 CH37X host 状态，并循环处理 USB 枚举与
 * 数据读取；调用方应把它作为独立 FreeRTOS 任务运行。
 */
void ch37x_main(void *args)
{
    ESP_LOGI(TAG, "CH37x USB Host starting...");
    
    // 初始化同步机制
    g_device_mutex = xSemaphoreCreateMutex();
    g_event_queue = xQueueCreate(10, sizeof(ch37x_event_t));
    
    if (g_device_mutex == NULL || g_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create synchronization objects");
        return;
    }
    
    // 初始化设备状态
    init_device_state();
    
    // 初始化 SPI
    ch37x_port_delay_ms(50);
    ch37x_port_spi_init();
    
    // 初始化 CH37x
    Init374Host();
    
    // 创建事件分发任务
    if (xTaskCreate(event_dispatch_task, "ch37x_event", CH37X_EVENT_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create event dispatch task");
        return;
    }
    
    ESP_LOGI(TAG, "CH37x USB Host initialized");
    
    // 主循环：检测设备插拔和枚举
    while (1) {
        // 等待设备插入
        while (!Query374DeviceIn()) {
            if (Query374Interrupt()) {
                HostDetectInterrupt();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        ESP_LOGI(TAG, "Device detected");
        post_event(CH37X_EVENT_DEVICE_CONNECT);
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // 重置停止标志
        g_stop_reading = false;
        
        // 枚举设备
        usb_enumerate_task();
        
        // 如果枚举成功，启动数据读取
        if (g_device.state == CH37X_DEV_STATE_CONFIGURED) {
            usb_read_task();
        }
        
        // 等待设备拔出
        ESP_LOGI(TAG, "Waiting for device removal...");
        while (Query374DeviceIn()) {
            if (Query374Interrupt()) {
                HostDetectInterrupt();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        ESP_LOGI(TAG, "Device removed");
        post_event(CH37X_EVENT_DEVICE_DISCONNECT);
        
        // 重置设备状态
        init_device_state();
        HostSetBusFree();
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============================================================================
// 内部辅助函数实现
// ============================================================================

static void init_device_state(void)
{
    if (device_lock(pdMS_TO_TICKS(1000))) {
        memset(&g_device, 0, sizeof(ch37x_device_t));
        g_device.state = CH37X_DEV_STATE_DISCONNECTED;
        UsbDevEndpSize = DEFAULT_ENDP0_SIZE;
        device_unlock();
    }
}

static void set_device_state(ch37x_device_state_t state)
{
    g_device.state = state;
    ESP_LOGD(TAG, "Device state changed to: %d", state);
}

static void post_event(ch37x_event_type_t type)
{
    ch37x_event_t event = {
        .type = type,
        .interface_num = 0,
        .speed = g_device.speed,
        .vid = g_device.vid,
        .pid = g_device.pid,
        .device_class = g_device.device_class,
        .error_code = 0,
        .user_data = NULL,
    };
    
    post_event_ex(&event);
}

static void post_event_ex(ch37x_event_t *event)
{
    if (g_event_queue != NULL) {
        if (xQueueSend(g_event_queue, event, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Event queue full, event dropped");
        }
    }
}

static void event_dispatch_task(void *args)
{
    ch37x_event_t event;
    
    ESP_LOGI(TAG, "Event dispatch task started");

    if (g_event_queue == NULL) {
        ESP_LOGE(TAG, "Event queue not initialized");
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        if (xQueueReceive(g_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            // 调用用户回调
            if (g_event_callback != NULL) {
                g_event_callback(&event);
            }
            
            // 打印事件日志
            const char *event_names[] = {
                "CONNECT", "DISCONNECT", "READY", "TRANSFER_COMPLETE",
                "TRANSFER_ERROR", "BUS_RESET", "BUS_SUSPEND", "BUS_RESUME"
            };
            
            if (event.type < sizeof(event_names) / sizeof(event_names[0])) {
                ESP_LOGI(TAG, "Event: %s (VID=0x%04X, PID=0x%04X)",
                         event_names[event.type], event.vid, event.pid);
            }
        }
    }
}

// ============================================================================
// 低层 CH37x 操作函数
// ============================================================================

static bool Query374DeviceIn(void)
{
    return (ch37x_port_spi_read_byte(REG_HUB_SETUP) & BIT_HUB0_ATTACH) ? true : false;
}

static bool Query374DevFullSpeed(void)
{
    return (ch37x_port_spi_read_byte(REG_SYS_INFO) & BIT_INFO_USB_DP) ? true : false;
}

static bool Query374Interrupt(void)
{
    return (ch37x_port_spi_read_byte(REG_INTER_FLAG) & BIT_IF_INTER_FLAG) ? true : false;
}

static uint8_t Wait374Interrupt(uint64_t deadline_us, bool deadline_enabled)
{
    uint16_t i;
    for (i = 0; i < 10000; i++) {
        if (deadline_enabled && (uint64_t)esp_timer_get_time() >= deadline_us) {
            return CH37X_USB_INT_DEADLINE;
        }
        if (Query374Interrupt()) {
            return 0;
        }
    }
    return ERR_USB_UNKNOWN;
}

static void SetHostUsbAddr(uint8_t addr)
{
    ch37x_port_spi_write_byte(REG_USB_ADDR, addr);
}

static void HostSetBusFree(void)
{
    ch37x_port_spi_write_byte(REG_USB_SETUP, BIT_SETP_HOST_MODE);
    ch37x_port_spi_write_byte(REG_USB_SETUP, BIT_SETP_HOST_MODE | BIT_SETP_AUTO_SOF);
    ch37x_port_spi_write_byte(REG_HUB_SETUP, 0x00);
    ch37x_port_spi_write_byte(REG_HUB_CTRL, 0x00);
}

static void HostSetBusReset(void)
{
    UsbDevEndpSize = DEFAULT_ENDP0_SIZE;
    SetHostUsbAddr(0x00);
    ch37x_port_spi_write_byte(REG_USB_H_CTRL, 0x00);
    ch37x_port_spi_write_byte(REG_HUB_SETUP, BIT_HUB0_RESET);
    ch37x_port_delay_ms(15);
    ch37x_port_spi_write_byte(REG_HUB_SETUP, 0x00);
    ch37x_port_delay_ms(1);
    ch37x_port_spi_write_byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_DEV_DETECT | BIT_IF_USB_SUSPEND);
}

static void HostSetFullSpeed(void)
{
    ch37x_port_spi_write_byte(REG_USB_SETUP, BIT_SETP_HOST_MODE | BIT_SETP_AUTO_SOF);
    ch37x_port_spi_write_byte(REG_HUB_SETUP, BIT_HUB0_EN);
}

static void HostSetLowSpeed(void)
{
    ch37x_port_spi_write_byte(REG_USB_SETUP, BIT_SETP_HOST_MODE | BIT_SETP_AUTO_SOF | BIT_SETP_LOW_SPEED);
    ch37x_port_spi_write_byte(REG_HUB_SETUP, BIT_HUB0_EN | BIT_HUB0_POLAR);
}

static void Init374Host(void)
{
    ch37x_port_spi_write_byte(REG_USB_SETUP, 0x00);
    SetHostUsbAddr(0x00);
    ch37x_port_spi_write_byte(REG_USB_H_CTRL, 0x00);
    ch37x_port_spi_write_byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_INTER_FLAG);
    ch37x_port_spi_write_byte(REG_INTER_EN, BIT_IE_TRANSFER | BIT_IE_DEV_DETECT);
    ch37x_port_spi_write_byte(REG_SYS_CTRL, BIT_CTRL_OE_POLAR);
    HostSetBusFree();
}

static void HostDetectInterrupt(void)
{
    uint8_t s;
    s = ch37x_port_spi_read_byte(REG_INTER_FLAG);
    
    if (s & BIT_IF_DEV_DETECT) {
        ch37x_port_spi_write_byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_DEV_DETECT);
        
        if (s & BIT_IF_DEV_ATTACH) {
            (void)ch37x_port_spi_read_byte(REG_USB_SETUP);
            ESP_LOGD(TAG, "USB device attach detected");
        } else {
            ESP_LOGD(TAG, "USB device detach detected");
        }
    } else {
        ch37x_port_spi_write_byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_TRANSFER | 
                                  BIT_IF_USB_SUSPEND | BIT_IF_WAKE_UP);
    }
}

static uint8_t HostTransact374(uint8_t endp_addr,
                               uint8_t pid,
                               bool tog,
                               uint64_t deadline_us,
                               bool deadline_enabled)
{
    uint8_t retry;
    uint8_t s, r, u;
    
    for (retry = 0; retry < 3; retry++) {
        if (deadline_enabled && (uint64_t)esp_timer_get_time() >= deadline_us) {
            return CH37X_USB_INT_DEADLINE;
        }
        ch37x_port_spi_write_byte(REG_USB_H_PID, M_MK_HOST_PID_ENDP(pid, endp_addr));
        if (deadline_enabled && (uint64_t)esp_timer_get_time() >= deadline_us) {
            return CH37X_USB_INT_DEADLINE;
        }
        ch37x_port_spi_write_byte(REG_USB_H_CTRL, 
                                 (tog ? (BIT_HOST_START | BIT_HOST_TRAN_TOG | BIT_HOST_RECV_TOG) : BIT_HOST_START));
        
        s = Wait374Interrupt(deadline_us, deadline_enabled);
        if (s == ERR_USB_UNKNOWN || s == CH37X_USB_INT_DEADLINE) {
            return s;
        }
        
        s = ch37x_port_spi_read_byte(REG_INTER_FLAG);
        
        if (s & BIT_IF_DEV_DETECT) {
            ch37x_port_delay_us(200);
            ch37x_port_spi_write_byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_DEV_DETECT | BIT_IF_TRANSFER);
            
            if (s & BIT_IF_DEV_ATTACH) {
                u = ch37x_port_spi_read_byte(REG_USB_SETUP);
                if (s & BIT_IF_USB_DX_IN) {
                    return (u & BIT_SETP_USB_SPEED) ? USB_INT_CONNECT_LS : USB_INT_CONNECT;
                } else {
                    return (u & BIT_SETP_USB_SPEED) ? USB_INT_CONNECT : USB_INT_CONNECT_LS;
                }
            } else {
                return USB_INT_DISCONNECT;
            }
        } else if (s & BIT_IF_TRANSFER) {
            ch37x_port_spi_write_byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_TRANSFER);
            s = ch37x_port_spi_read_byte(REG_USB_STATUS);
            r = s & BIT_STAT_DEV_RESP;
            
            switch (pid) {
                case DEF_USB_PID_SETUP:
                case DEF_USB_PID_OUT:
                    if (r == DEF_USB_PID_ACK) {
                        return USB_INT_SUCCESS;
                    } else if (r == DEF_USB_PID_STALL || r == DEF_USB_PID_NAK) {
                        return (r | 0x20);
                    } else if (!M_IS_HOST_TIMEOUT(s)) {
                        return (r | 0x20);
                    }
                    break;
                    
                case DEF_USB_PID_IN:
                    if (M_IS_HOST_IN_DATA(s)) {
                        if (s & BIT_STAT_TOG_MATCH) {
                            return USB_INT_SUCCESS;
                        }
                    } else if (r == DEF_USB_PID_STALL || r == DEF_USB_PID_NAK) {
                        return (r | 0x20);
                    } else if (!M_IS_HOST_TIMEOUT(s)) {
                        return (r | 0x20);
                    }
                    break;
                    
                default:
                    return ERR_USB_UNKNOWN;
            }
        } else {
            ch37x_port_delay_us(200);
            ch37x_port_spi_write_byte(REG_INTER_FLAG, BIT_IF_USB_PAUSE | BIT_IF_INTER_FLAG);
            if (retry) {
                return ERR_USB_UNKNOWN;
            }
        }
    }
    
    return 0x20;
}

static uint8_t WaitHostTransact374(uint8_t endp_addr, uint8_t pid, bool tog, uint16_t timeout_ms)
{
    uint8_t s = (DEF_USB_PID_NAK | 0x20);
    bool first_attempt = true;
    uint64_t deadline_us = (uint64_t)esp_timer_get_time() +
                           ((uint64_t)timeout_ms * 1000ULL);
    
    while (1) {
        /* 首次事务始终允许；NAK 重试绝不能在调用方截止时间之后启动。 */
        if (!first_attempt && timeout_ms > 0U &&
            (uint64_t)esp_timer_get_time() >= deadline_us) {
            return s;
        }
        first_attempt = false;
        s = HostTransact374(endp_addr,
                            pid,
                            tog,
                            deadline_us,
                            timeout_ms > 0U);

        /*
         * 绝对截止时间是 WaitHostTransact 的正常超时结果：IN 轮询据此继续等待，
         * OUT 写路径则把 NAK 超时映射为 CH37X_WRITE_ERR_TIMEOUT。内部哨兵不能
         * 泄漏为设备传输错误事件。
         */
        if (s == CH37X_USB_INT_DEADLINE) {
            return (DEF_USB_PID_NAK | 0x20);
        }
        
        if (s != (DEF_USB_PID_NAK | 0x20)) {
            return s;
        }

        /* timeout=0 只尝试一次；其余情况按绝对截止时间决定下一次 NAK 重试。 */
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (timeout_ms == 0U || now_us >= deadline_us) {
            return s;
        }

        uint64_t wait_us = deadline_us - now_us;
        if (wait_us > 10000ULL) {
            wait_us = 10000ULL;
        }
        vTaskDelay(timeout_ms_to_ticks_ceil((uint32_t)((wait_us + 999ULL) / 1000ULL)));
    }
}

static uint8_t HostCtrlTransfer374(const uint8_t *ReqBuf, uint8_t *DatBuf, uint8_t *RetLen)
{
    uint8_t s, len, count, total = 0;
    bool tog;

    if (ReqBuf == NULL) {
        return ERR_USB_UNKNOWN;
    }
    if (ch37x_port_spi_write_block(RAM_HOST_TRAN, 8, ReqBuf) < 0 ||
        ch37x_port_spi_write_byte(REG_USB_LENGTH, 8) < 0) {
        ESP_LOGE(TAG, "SETUP SPI write failed");
        return ERR_USB_UNKNOWN;
    }
    s = WaitHostTransact374(0, DEF_USB_PID_SETUP, false, 200);
    
    if (s != USB_INT_SUCCESS) {
        ESP_LOGE(TAG, "SETUP failed: 0x%02X", s);
        return s;
    }
    
    tog = true;
    total = ReqBuf[6];
    
    if (total && DatBuf) {
        len = total;
        
        if (ReqBuf[0] & 0x80) {
            // IN 传输（读取）
            while (len) {
                s = WaitHostTransact374(0, DEF_USB_PID_IN, tog, 200);
                
                if (s != USB_INT_SUCCESS) {
                    ESP_LOGE(TAG, "IN failed: 0x%02X", s);
                    break;
                }
                
                count = ch37x_port_spi_read_byte(REG_USB_LENGTH);
                if (count > 0 && ch37x_port_spi_read_block(RAM_HOST_RECV, count, DatBuf) < 0) {
                    ESP_LOGE(TAG, "Failed to read data stage from CH37x");
                    s = ERR_USB_UNKNOWN;
                    break;
                }
                DatBuf += count;
                
                if (count <= len) {
                    len -= count;
                } else {
                    len = 0;
                }
                
                if (count == 0 || (count & (UsbDevEndpSize - 1))) {
                    break;  // 短包
                }
                
                tog = !tog;
            }
            tog = false;  // STATUS 阶段为 OUT
        } else {
            // OUT 传输（写入）
            while (len) {
                count = len >= UsbDevEndpSize ? UsbDevEndpSize : len;
                if (ch37x_port_spi_write_block(RAM_HOST_TRAN, count, DatBuf) < 0 ||
                    ch37x_port_spi_write_byte(REG_USB_LENGTH, count) < 0) {
                    ESP_LOGE(TAG, "Failed to write data stage to CH37x");
                    s = ERR_USB_UNKNOWN;
                    break;
                }
                s = WaitHostTransact374(0, DEF_USB_PID_OUT, tog, 200);
                
                if (s != USB_INT_SUCCESS) {
                    ESP_LOGE(TAG, "OUT failed: 0x%02X", s);
                    break;
                }
                
                DatBuf += count;
                len -= count;
                tog = !tog;
            }
            tog = true;  // STATUS 阶段为 IN
        }
        
        total -= len;
    }
    
    if (s == USB_INT_SUCCESS) {
        // STATUS 阶段
        if (ch37x_port_spi_write_byte(REG_USB_LENGTH, 0) < 0) {
            return ERR_USB_UNKNOWN;
        }
        ch37x_port_delay_us(100);
        s = WaitHostTransact374(0, (tog ? DEF_USB_PID_IN : DEF_USB_PID_OUT), true, 200);
        
        if (tog && s == USB_INT_SUCCESS) {
            if (ch37x_port_spi_read_byte(REG_USB_LENGTH)) {
                s = USB_INT_BUF_OVER;
            }
        }
    }
    
    if (RetLen) {
        *RetLen = total;
    }
    
    return s;
}

static uint8_t GetDeviceDescr(uint8_t *buf)
{
    uint8_t s, len;
    if (buf == NULL) {
        return ERR_USB_UNKNOWN;
    }
    s = HostCtrlTransfer374(SetupGetDevDescr, buf, &len);
    
    if (s == USB_INT_SUCCESS) {
        if (len < 8) {
            return USB_INT_BUF_OVER;
        }
        UsbDevEndpSize = ((PUSB_DEV_DESCR)buf)->bMaxPacketSize0;
        
        if (len < ((PUSB_SETUP_REQ)SetupGetDevDescr)->wLengthL) {
            s = USB_INT_BUF_OVER;
        }
    }
    
    return s;
}

static uint8_t GetConfigDescr(uint8_t *buf)
{
    uint8_t s, len;
    uint8_t BufLogDescr[sizeof(SetupGetCfgDescr)];

    if (buf == NULL) {
        return ERR_USB_UNKNOWN;
    }
    
    s = HostCtrlTransfer374(SetupGetCfgDescr, buf, &len);
    
    if (s == USB_INT_SUCCESS) {
        if (len < ((PUSB_SETUP_REQ)SetupGetCfgDescr)->wLengthL) {
            s = USB_INT_BUF_OVER;
        } else {
            memcpy(BufLogDescr, SetupGetCfgDescr, sizeof(SetupGetCfgDescr));
            ((PUSB_SETUP_REQ)BufLogDescr)->wLengthL = ((PUSB_CFG_DESCR)buf)->wTotalLengthL;
            s = HostCtrlTransfer374(BufLogDescr, buf, &len);
            
            if (s == USB_INT_SUCCESS) {
                if (len < ((PUSB_CFG_DESCR)buf)->wTotalLengthL) {
                    s = USB_INT_BUF_OVER;
                }
            }
        }
    }
    
    return s;
}

static uint8_t SetUsbAddress(uint8_t addr)
{
    uint8_t s;
    uint8_t BufSetAddr[sizeof(SetupSetUsbAddr)];
    
    memcpy(BufSetAddr, SetupSetUsbAddr, sizeof(SetupSetUsbAddr));
    ((PUSB_SETUP_REQ)BufSetAddr)->wValueL = addr;
    s = HostCtrlTransfer374(BufSetAddr, NULL, NULL);
    
    if (s == USB_INT_SUCCESS) {
        SetHostUsbAddr(addr);
    }
    
    ch37x_port_delay_ms(3);
    return s;
}

static uint8_t SetUsbConfig(uint8_t cfg)
{
    uint8_t BufSetCfg[sizeof(SetupSetUsbConfig)];
    
    memcpy(BufSetCfg, SetupSetUsbConfig, sizeof(SetupSetUsbConfig));
    ((PUSB_SETUP_REQ)BufSetCfg)->wValueL = cfg;
    
    return HostCtrlTransfer374(BufSetCfg, NULL, NULL);
}

static uint8_t ParseConfigDescriptor(uint8_t *buf, uint16_t total_len)
{
    uint16_t offset = 0;
    uint8_t desc_len, desc_type;
    uint8_t current_interface = 0xFF;
    ch37x_usb_ep_descr_t ep_desc;
    ch37x_usb_if_descr_t if_desc;
    
    if (buf == NULL || total_len < 9) {
        ESP_LOGE(TAG, "Invalid descriptor buffer");
        return ERR_USB_UNKNOWN;
    }

    if (!device_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ERR_USB_UNKNOWN;
    }
    
    // 初始化端点信息
    for (int i = 0; i < 4; i++) {
        memset(&g_device.interfaces[i], 0, sizeof(g_device.interfaces[i]));
    }
    
    // 跳过配置描述符（9字节）
    offset = 9;
    
    ESP_LOGI(TAG, "Parsing configuration descriptor (total %u bytes)...", total_len);
    
    while ((offset + 1U) < total_len) {
        desc_len = buf[offset];
        desc_type = buf[offset + 1];
        
        if (desc_len == 0) {
            ESP_LOGW(TAG, "Invalid descriptor length at offset %u", offset);
            break;
        }
        if ((offset + desc_len) > total_len) {
            ESP_LOGW(TAG, "Descriptor overruns buffer at offset %u", offset);
            break;
        }
        
        switch (desc_type) {
            case 0x04:  // 接口描述符
                if (desc_len >= 9) {
                    if_desc.length = buf[offset + 0];
                    if_desc.descriptorType = buf[offset + 1];
                    if_desc.interfaceNumber = buf[offset + 2];
                    if_desc.alternateSetting = buf[offset + 3];
                    if_desc.numEndpoints = buf[offset + 4];
                    if_desc.interfaceClass = buf[offset + 5];
                    if_desc.interfaceSubClass = buf[offset + 6];
                    if_desc.interfaceProtocol = buf[offset + 7];
                    if_desc.interface = buf[offset + 8];
                    
                    current_interface = if_desc.interfaceNumber;
                    
                    ESP_LOGI(TAG, "  Interface %u: Class=0x%02X, SubClass=0x%02X, Protocol=0x%02X, %u endpoints",
                             if_desc.interfaceNumber,
                             if_desc.interfaceClass,
                             if_desc.interfaceSubClass,
                             if_desc.interfaceProtocol,
                             if_desc.numEndpoints);
                }
                break;
                
            case 0x05:  // 端点描述符
                if (desc_len >= 7 && current_interface < 4) {
                    ep_desc.length = buf[offset + 0];
                    ep_desc.descriptorType = buf[offset + 1];
                    ep_desc.endpointAddress = buf[offset + 2];
                    ep_desc.attributes = buf[offset + 3];
                    ep_desc.maxPacketSize = buf[offset + 4] | (buf[offset + 5] << 8);
                    ep_desc.interval = buf[offset + 6];
                    
                    uint8_t ep_dir = ep_desc.endpointAddress & 0x80;
                    uint8_t ep_type = ep_desc.attributes & 0x03;
                    
                    ESP_LOGI(TAG, "    Endpoint 0x%02X: %s, Type=%s, MaxPacket=%u",
                             ep_desc.endpointAddress,
                             ep_dir ? "IN" : "OUT",
                             (ep_type == 0) ? "Control" :
                             (ep_type == 1) ? "Isochronous" :
                             (ep_type == 2) ? "Bulk" : "Interrupt",
                             ep_desc.maxPacketSize);
                    
                    // 保存端点信息
                    if (ep_type == 2) {  // Bulk 端点
                        if (ep_dir) {  // IN
                            g_device.interfaces[current_interface].in_endp_addr = ep_desc.endpointAddress;
                            g_device.interfaces[current_interface].in_endp_size = ep_desc.maxPacketSize;
                            g_device.interfaces[current_interface].in_endp_tog = false;
                        } else {  // OUT
                            g_device.interfaces[current_interface].out_endp_addr = ep_desc.endpointAddress;
                            g_device.interfaces[current_interface].out_endp_size = ep_desc.maxPacketSize;
                            g_device.interfaces[current_interface].out_endp_tog = false;
                        }
                    } else if (ep_type == 3) {  // Interrupt 端点
                        if (ep_dir) {  // IN
                            g_device.interfaces[current_interface].int_endp_addr = ep_desc.endpointAddress;
                            g_device.interfaces[current_interface].int_endp_size = ep_desc.maxPacketSize;
                        }
                    }
                }
                break;
                
            default:
                // 其他描述符类型，跳过
                break;
        }
        
        offset += desc_len;
    }
    
    // 显示解析结果
    ESP_LOGI(TAG, "\nParsed endpoint information:");
    for (int i = 0; i < 4; i++) {
        if (g_device.interfaces[i].in_endp_addr || g_device.interfaces[i].out_endp_addr) {
            ESP_LOGI(TAG, "  Interface %d:", i);
            
            if (g_device.interfaces[i].in_endp_addr) {
                ESP_LOGI(TAG, "    IN  Endpoint: 0x%02X, MaxPacket=%u",
                         g_device.interfaces[i].in_endp_addr,
                         g_device.interfaces[i].in_endp_size);
            }
            
            if (g_device.interfaces[i].out_endp_addr) {
                ESP_LOGI(TAG, "    OUT Endpoint: 0x%02X, MaxPacket=%u",
                         g_device.interfaces[i].out_endp_addr,
                         g_device.interfaces[i].out_endp_size);
            }
            
            if (g_device.interfaces[i].int_endp_addr) {
                ESP_LOGI(TAG, "    INT Endpoint: 0x%02X, MaxPacket=%u",
                         g_device.interfaces[i].int_endp_addr,
                         g_device.interfaces[i].int_endp_size);
            }
        }
    }
    
    device_unlock();
    
    return USB_INT_SUCCESS;
}

// ============================================================================
// USB 枚举任务
// ============================================================================

static void usb_enumerate_task(void)
{
    uint8_t s, i;
    uint8_t buf[256];
    
    ESP_LOGI(TAG, "Starting device enumeration...");
    
    // 总线复位
    HostSetBusReset();
    
    for (i = 0; i < 100; i++) {
        if (Query374DeviceIn()) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (Query374Interrupt()) {
        HostDetectInterrupt();
    }
    
    if (!Query374DeviceIn()) {
        ESP_LOGE(TAG, "Device gone during reset");
        return;
    }
    
    // 设置速度
    if (device_lock(pdMS_TO_TICKS(1000))) {
        if (Query374DevFullSpeed()) {
            HostSetFullSpeed();
            g_device.speed = CH37X_SPEED_FULL;
            ESP_LOGI(TAG, "Full-Speed device detected (12 Mbps)");
        } else {
            HostSetLowSpeed();
            g_device.speed = CH37X_SPEED_LOW;
            ESP_LOGI(TAG, "Low-Speed device detected (1.5 Mbps)");
        }
        set_device_state(CH37X_DEV_STATE_RESET);
        device_unlock();
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 获取设备描述符
    s = GetDeviceDescr(buf);
    if (s != USB_INT_SUCCESS) {
        ESP_LOGE(TAG, "GetDeviceDescr failed: 0x%02X", s);
        return;
    }
    
    // 解析设备描述符
    ch37x_usb_dev_descr_t dev_descr;
    ch37x_usb_dev_descr_fill_by_buffer(&dev_descr, buf, 18);
    print_device_descriptor(&dev_descr);
    
    if (device_lock(pdMS_TO_TICKS(1000))) {
        g_device.vid = dev_descr.vid;
        g_device.pid = dev_descr.pid;
        g_device.device_class = dev_descr.deviceClass;
        g_device.max_packet_size0 = dev_descr.maxPacketSize0;
        device_unlock();
    }
    
    // 设置地址
    s = SetUsbAddress(0x02);
    if (s != USB_INT_SUCCESS) {
        ESP_LOGE(TAG, "SetUsbAddress failed: 0x%02X", s);
        return;
    }
    
    if (device_lock(pdMS_TO_TICKS(1000))) {
        g_device.address = 0x02;
        set_device_state(CH37X_DEV_STATE_ADDRESSED);
        device_unlock();
    }
    
    ESP_LOGI(TAG, "Device address set to 0x02");
    
    // 获取配置描述符
    s = GetConfigDescr(buf);
    if (s != USB_INT_SUCCESS) {
        ESP_LOGE(TAG, "GetConfigDescr failed: 0x%02X", s);
        return;
    }
    
    // 解析配置描述符
    ch37x_usb_cfg_descr_t cfg_descr;
    ch37x_usb_cfg_descr_fill_by_buffer(&cfg_descr, buf, 9);
    print_config_descriptor(&cfg_descr);
    
    // 解析端点
    s = ParseConfigDescriptor(buf, cfg_descr.totalLength);
    if (s != USB_INT_SUCCESS) {
        ESP_LOGE(TAG, "ParseConfigDescriptor failed: 0x%02X", s);
        return;
    }
    
    // 设置配置
    s = SetUsbConfig(cfg_descr.configurationValue);
    if (s != USB_INT_SUCCESS) {
        ESP_LOGE(TAG, "SetUsbConfig failed: 0x%02X", s);
        return;
    }
    
    if (device_lock(pdMS_TO_TICKS(1000))) {
        g_device.config_value = cfg_descr.configurationValue;
        g_device.num_interfaces = cfg_descr.mumInterfaces;
        set_device_state(CH37X_DEV_STATE_CONFIGURED);
        device_unlock();
    }
    
    ESP_LOGI(TAG, "✓ Device enumeration completed successfully");
    post_event(CH37X_EVENT_DEVICE_READY);
}

// ============================================================================
// USB 数据读取任务
// ============================================================================

static void usb_read_task(void)
{
    uint8_t s;
    uint8_t *recv_buf = malloc(1024);
    uint16_t recv_len;
    uint32_t timeout_count = 0;
    const uint32_t max_timeout = 100;
    
    ESP_LOGI(TAG, "Starting USB data read task...");

    if (recv_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate USB receive buffer");
        return;
    }
    
    // 遍历所有接口
    for (int intf = 0; intf < g_device.num_interfaces && intf < 4; intf++) {
        uint8_t in_endp = g_device.interfaces[intf].in_endp_addr;
        
        if (in_endp == 0) {
            continue;  // 该接口没有 IN 端点
        }
        
        ESP_LOGI(TAG, "Reading from interface %d, endpoint 0x%02X", intf, in_endp);
        
        // 循环读取数据
        while (!g_stop_reading) {
            /*
             * IN 轮询和外部 OUT 写都会访问同一组 CH37X SPI 寄存器。整个 USB
             * transaction 必须共用 device mutex；回调则在解锁后执行，避免上层
             * 处理反向阻塞硬件总线。
             */
            if (!device_lock(pdMS_TO_TICKS(1000))) {
                ESP_LOGW(TAG, "Failed to lock CH37X for IN transfer");
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            if (!Query374DeviceIn()) {
                device_unlock();
                ESP_LOGW(TAG, "Device disconnected");
                break;
            }
            
            // 检查中断
            if (Query374Interrupt()) {
                HostDetectInterrupt();
            }
            
            // 从 IN 端点读取数据
            s = WaitHostTransact374(
                in_endp,
                DEF_USB_PID_IN,
                g_device.interfaces[intf].in_endp_tog,
                200
            );
            
            if (s == USB_INT_SUCCESS) {
                // 读取成功，获取数据长度
                recv_len = ch37x_port_spi_read_byte(REG_USB_LENGTH);
                
                if (recv_len > 0) {
                    // 读取数据
                    if (ch37x_port_spi_read_block(RAM_HOST_RECV, (uint8_t)recv_len, recv_buf) < 0) {
                        device_unlock();
                        ESP_LOGW(TAG, "Failed to read %u bytes from CH37x FIFO", recv_len);
                        vTaskDelay(pdMS_TO_TICKS(10));
                        continue;
                    }
                    
                    // 翻转同步标志（重要！）
                    g_device.interfaces[intf].in_endp_tog = !g_device.interfaces[intf].in_endp_tog;

                    timeout_count = 0;
                    g_device.last_activity_time = esp_timer_get_time() / 1000;
                    device_unlock();

                    // 回调不持有设备锁，允许上层只做非阻塞投递。
                    if (g_data_callback != NULL) {
                        int ret = g_data_callback(intf, recv_buf, recv_len);
                        
                        if (ret < 0) {
                            ESP_LOGW(TAG, "Data callback returned error: %d", ret);
                        }
                    } else {
                        // 如果没有回调，打印接收到的数据
                        ESP_LOGI(TAG, "Received %u bytes from interface %d:", recv_len, intf);
                        ESP_LOG_BUFFER_HEX_LEVEL(TAG, recv_buf, recv_len, ESP_LOG_INFO);
                    }
                } else {
                    // 零长度包，翻转同步标志
                    g_device.interfaces[intf].in_endp_tog = !g_device.interfaces[intf].in_endp_tog;
                    device_unlock();
                }
                
            } else if (s == (DEF_USB_PID_NAK | 0x20)) {
                device_unlock();
                // NAK：设备暂时没有数据，继续等待
                timeout_count++;
                
                if (timeout_count >= max_timeout) {
                    // 长时间没有数据
                    timeout_count = 0;
                }
                
                vTaskDelay(pdMS_TO_TICKS(10));
                
            } else if (s == USB_INT_DISCONNECT) {
                device_unlock();
                // 设备断开
                ESP_LOGW(TAG, "Device disconnected during read");
                break;
                
            } else {
                device_unlock();
                // 其他错误
                ESP_LOGW(TAG, "Read error from interface %d: 0x%02X", intf, s);
                
                // 发送错误事件
                ch37x_event_t event = {
                    .type = CH37X_EVENT_TRANSFER_ERROR,
                    .interface_num = intf,
                    .error_code = s,
                };
                post_event_ex(&event);
                
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            
            // 让出 CPU 时间
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        if (g_stop_reading) {
            ESP_LOGI(TAG, "Read task stopped by request");
            break;
        }
    }
    
    ESP_LOGI(TAG, "USB read task ended");
    free(recv_buf);
}

// ============================================================================
// 描述符解析辅助函数
// ============================================================================

int ch37x_usb_dev_descr_fill_by_buffer(ch37x_usb_dev_descr_t *descr, 
                                        const uint8_t *buffer, 
                                        size_t buffer_size)
{
    if (descr == NULL || buffer == NULL || buffer_size < 2) {
        return -1;
    }
    
    if (buffer[1] != USB_DEVICE_DESCR_TYPE) {
        return -1;
    }
    
    if (buffer[0] < 18 || buffer_size < buffer[0]) {
        return -1;
    }
    
    memset(descr, 0, sizeof(ch37x_usb_dev_descr_t));
    
    descr->length           = buffer[0];
    descr->descriptorType   = buffer[1];
    descr->bcdUSB           = buffer[2] | (buffer[3] << 8);
    descr->deviceClass      = buffer[4];
    descr->deviceSubClass   = buffer[5];
    descr->deviceProtocol   = buffer[6];
    descr->maxPacketSize0   = buffer[7];
    descr->vid              = buffer[8] | (buffer[9] << 8);
    descr->pid              = buffer[10] | (buffer[11] << 8);
    descr->bcdDevice        = buffer[12] | (buffer[13] << 8);
    descr->manufacturer     = buffer[14];
    descr->product          = buffer[15];
    descr->serialNumber     = buffer[16];
    descr->numConfigurations = buffer[17];
    
    return 0;
}

int ch37x_usb_cfg_descr_fill_by_buffer(ch37x_usb_cfg_descr_t *descr, 
                                        const uint8_t *buffer, 
                                        size_t buffer_size)
{
    if (descr == NULL || buffer == NULL || buffer_size < 2) {
        return -1;
    }
    
    if (buffer[1] != USB_CONFIG_DESCR_TYPE) {
        return -1;
    }
    
    if (buffer[0] < 9 || buffer_size < buffer[0]) {
        return -1;
    }
    
    memset(descr, 0, sizeof(ch37x_usb_cfg_descr_t));
    
    descr->length              = buffer[0];
    descr->descriptorType      = buffer[1];
    descr->totalLength         = buffer[2] | (buffer[3] << 8);
    descr->mumInterfaces       = buffer[4];
    descr->configurationValue  = buffer[5];
    descr->configuration       = buffer[6];
    descr->attributes          = buffer[7];
    descr->maxPower            = buffer[8];
    
    return 0;
}

void print_device_descriptor(const ch37x_usb_dev_descr_t *descr)
{
    printf("\n");
    printf("╔════════════════════════════════════════════╗\n");
    printf("║     USB Device Descriptor                  ║\n");
    printf("╠════════════════════════════════════════════╣\n");
    printf("║ USB Version:        %x.%x%x                   ║\n",
           descr->bcdUSB >> 8, (descr->bcdUSB >> 4) & 0x0F, descr->bcdUSB & 0x0F);
    printf("║ Device Class:       0x%02X                   ║\n", descr->deviceClass);
    printf("║ Device SubClass:    0x%02X                   ║\n", descr->deviceSubClass);
    printf("║ Device Protocol:    0x%02X                   ║\n", descr->deviceProtocol);
    printf("║ Max Packet Size:    %-3u bytes              ║\n", descr->maxPacketSize0);
    printf("║ Vendor ID:          0x%04X                 ║\n", descr->vid);
    printf("║ Product ID:         0x%04X                 ║\n", descr->pid);
    printf("║ Device Version:     %x.%02x                 %s║\n", 
           descr->bcdDevice >> 8, descr->bcdDevice & 0xFF,
          ((descr->bcdDevice >> 8)/16) ? "" : " "/*高位无数据就在后面补0，用于对齐*/);
    printf("║ Num Configurations: %-3u                    ║\n", descr->numConfigurations);
    printf("╚════════════════════════════════════════════╝\n");
    printf("\n");
}

void print_config_descriptor(const ch37x_usb_cfg_descr_t *descr)
{
    printf("\n");
    printf("╔════════════════════════════════════════════╗\n");
    printf("║   USB Configuration Descriptor             ║\n");
    printf("╠════════════════════════════════════════════╣\n");
    printf("║ Total Length:       %-5u bytes            ║\n", descr->totalLength);
    printf("║ Num Interfaces:     %-3u                    ║\n", descr->mumInterfaces);
    printf("║ Configuration Val:  %-3u                    ║\n", descr->configurationValue);
    printf("║ Attributes:         0x%02X                   ║\n", descr->attributes);
    
    if (descr->attributes & 0x40) {
        printf("║   - Self Powered                           ║\n");
    }
    if (descr->attributes & 0x20) {
        printf("║   - Remote Wakeup                          ║\n");
    }
    if ((descr->attributes & 0x9F) == 0x80) {
        printf("║   - Bus Powered                            ║\n");
    }
    
    printf("║ Max Power:          %-5u mA               ║\n", descr->maxPower * 2);
    printf("╚════════════════════════════════════════════╝\n");
    printf("\n");
}
