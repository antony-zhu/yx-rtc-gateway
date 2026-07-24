#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ch37x_device.h"
#include "gw_defaults.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_ch37x_bridge.h
 * @brief CH37X host 与脚本/业务层之间的轻量桥接接口。
 *
 * 底层 `ch37x_*` 负责 USB host 枚举和端点收发；本模块负责脚本层的 interface
 * 绑定、接收缓存、状态快照和主动断开。它不做任何业务协议拆包。
 */

/**
 * @brief CH37X 桥接状态快照。
 */
typedef struct {
    bool configured;             /*!< 是否已配置目标 interface */
    bool active;                 /*!< 脚本层是否允许使用 */
    bool host_started;           /*!< 底层 host 任务是否已启动 */
    bool ready;                  /*!< 当前 interface 是否可读写 */
    uint8_t interface_num;       /*!< 当前绑定 interface */
    size_t pending_bytes;        /*!< 接收缓存中待读字节数 */
    size_t queue_capacity;       /*!< 接收缓存容量 */
    uint32_t rx_drop_count;      /*!< 因缓存满丢弃的接收字节数 */
    uint32_t rx_bytes;           /*!< 累计接收字节数 */
    uint32_t tx_bytes;           /*!< 累计发送字节数 */
    ch37x_device_state_t device_state; /*!< 底层 USB 设备状态 */
    uint16_t vid;                /*!< USB VID */
    uint16_t pid;                /*!< USB PID */
    uint8_t device_class;        /*!< USB device class */
    uint8_t speed;               /*!< USB 速度 */
    uint8_t num_interfaces;      /*!< USB interface 数 */
    uint8_t config_value;        /*!< 当前 USB 配置值 */
    uint8_t in_endpoint;         /*!< 目标 interface IN 端点 */
    uint8_t out_endpoint;        /*!< 目标 interface OUT 端点 */
    uint8_t int_endpoint;        /*!< 目标 interface interrupt 端点 */
    uint16_t in_max_packet;      /*!< IN 端点最大包长 */
    uint16_t out_max_packet;     /*!< OUT 端点最大包长 */
    uint16_t int_max_packet;     /*!< interrupt 端点最大包长 */
} gw_ch37x_bridge_info_t;

/**
 * @brief CH37X 原始数据观察者。
 *
 * observer 适合后续把 USB 字节流直接喂给 C 层协议解析器。未设置 observer 时，
 * 收到的数据会进入脚本读取缓存。
 */
typedef struct {
    void (*on_data)(uint8_t interface_num, const uint8_t *data, size_t data_len, void *ctx);
    void (*on_reset)(void *ctx);
    void *ctx;
} gw_ch37x_bridge_observer_t;

/**
 * @brief 配置脚本层绑定的 CH37X interface。
 *
 * @param interface_num interface 编号，范围 0..GW_CH37X_MAX_INTERFACE。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_ch37x_bridge_configure(uint8_t interface_num);

/**
 * @brief 启动 CH37X 桥接。
 *
 * 首次调用会创建接收缓存、注册底层回调并拉起 CH37X host 任务；后续调用只恢复
 * active 状态。底层 host 任务启动后不会被脚本 stop 销毁。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_ch37x_bridge_start(void);

/**
 * @brief 停止脚本层使用 CH37X。
 *
 * 该函数只关闭 active 状态并清空接收缓存，不销毁底层 host 任务。
 */
void gw_ch37x_bridge_stop(void);

/**
 * @brief 清空脚本层配置和统计。
 *
 * 底层 host 任务启动后仍保留运行。
 */
void gw_ch37x_bridge_destroy(void);

/**
 * @brief 主动断开当前 USB 设备。
 *
 * @return ESP_OK 表示已发出断开请求。
 */
esp_err_t gw_ch37x_bridge_disconnect(void);

/**
 * @brief 查询当前是否可直接读写绑定 interface。
 */
bool gw_ch37x_bridge_is_ready(void);

/**
 * @brief 查询接收缓存中待读字节数。
 */
size_t gw_ch37x_bridge_pending(void);

/**
 * @brief 读取 CH37X 接收缓存。
 *
 * @param buf 输出缓冲区。
 * @param cap 最大读取字节数。
 * @param timeout_ms 等待首字节超时，0 表示不等待。
 * @return >0 表示读取字节数；0 表示无数据或未 ready；<0 表示参数或状态错误。
 */
int gw_ch37x_bridge_read(uint8_t *buf, size_t cap, uint32_t timeout_ms);

/**
 * @brief 写入 CH37X 当前绑定 interface。
 *
 * @param data 数据缓冲区。
 * @param len 数据长度。
 * @param timeout_ms 获取桥接锁、设备锁和等待端点 NAK 的预算；0 表示不等待锁或
 *                   NAK 重试，但锁可立即取得时仍执行一次同步 USB 事务。
 * @return ESP_OK 表示写入成功；等待预算耗尽返回 ESP_ERR_TIMEOUT。
 */
esp_err_t gw_ch37x_bridge_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief 获取 CH37X 桥接状态快照。
 */
esp_err_t gw_ch37x_bridge_get_info(gw_ch37x_bridge_info_t *out_info);

/**
 * @brief 设置 CH37X 原始数据观察者。
 *
 * 仅保留单个观察者；传 NULL 表示清空。
 */
esp_err_t gw_ch37x_bridge_set_observer(const gw_ch37x_bridge_observer_t *observer);

#ifdef __cplusplus
}
#endif
