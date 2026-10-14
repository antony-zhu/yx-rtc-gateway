#pragma once 

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @file ch37x_def.h
 * @brief CH37X USB 描述符与端点信息定义。
 *
 * 该头文件保存从 CH37X 枚举流程中解析出来的 USB 标准描述符结构。字段名称尽量
 * 与 USB 规范保持一致，便于对照抓包和芯片手册。
 */

/**
 * @brief USB 设备描述符。
 *
 * 字段含义对应标准 USB device descriptor，用于保存枚举阶段从设备读取到的
 * VID/PID、类别和字符串索引等信息。
 */
typedef struct ch37x_usb_dev_descr{
    uint8_t   length;
    uint8_t   descriptorType;
    uint16_t  bcdUSB;
    uint8_t   deviceClass;
    uint8_t   deviceSubClass;
    uint8_t   deviceProtocol;
    uint8_t   maxPacketSize0;
    uint16_t  vid;
    uint16_t  pid;
    uint16_t  bcdDevice;
    uint8_t   manufacturer;
    uint8_t   product;
    uint8_t   serialNumber;
    uint8_t   numConfigurations;
}ch37x_usb_dev_descr_t;

/**
 * @brief USB 配置描述符。
 *
 * 当前只保存配置头部字段，接口和端点描述符由后续结构单独承载。
 */
typedef struct ch37x_usb_cfg_descr{
    uint8_t  length;
    uint8_t  descriptorType;
    uint16_t totalLength;
    uint8_t  mumInterfaces;
    uint8_t  configurationValue;
    uint8_t  configuration;
    uint8_t  attributes;
    uint8_t  maxPower;
}ch37x_usb_cfg_descr_t;



/**
 * @brief USB 端点描述符。
 */
typedef struct ch37x_usb_ep_descr {
    uint8_t  length;
    uint8_t  descriptorType;
    uint8_t  endpointAddress;
    uint8_t  attributes;
    uint16_t maxPacketSize;
    uint8_t  interval;
} ch37x_usb_ep_descr_t;

/**
 * @brief USB 接口描述符。
 */
typedef struct ch37x_usb_if_descr {
    uint8_t length;
    uint8_t descriptorType;
    uint8_t interfaceNumber;
    uint8_t alternateSetting;
    uint8_t numEndpoints;
    uint8_t interfaceClass;
    uint8_t interfaceSubClass;
    uint8_t interfaceProtocol;
    uint8_t interface;
} ch37x_usb_if_descr_t;

/**
 * @brief 单个 USB interface 的端点运行信息。
 *
 * `*_tog` 保存 DATA0/DATA1 同步位，CH37X 传输时需要维持该状态。
 */
typedef struct {
    uint8_t in_endp_addr;      /**< IN 端点地址，读取数据用。 */
    uint8_t out_endp_addr;     /**< OUT 端点地址，发送数据用。 */
    uint8_t int_endp_addr;     /**< 中断端点地址。 */
    uint16_t in_endp_size;     /**< IN 端点最大包大小。 */
    uint16_t out_endp_size;    /**< OUT 端点最大包大小。 */
    uint16_t int_endp_size;    /**< 中断端点最大包大小。 */
    bool in_endp_tog;          /**< IN 端点 DATA toggle 标志。 */
    bool out_endp_tog;         /**< OUT 端点 DATA toggle 标志。 */
} usb_device_info_t;
