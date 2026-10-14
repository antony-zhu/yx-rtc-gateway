#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_spi_driver.h
 * @brief SPI 简化封装接口。
 *
 * 本模块对 ESP-IDF 的 SPI 主机能力做轻量封装，向上层提供创建、读写和
 * 全双工传输接口。当前主要供 CH37X 等外设桥接层使用。
 */

/** @brief SPI 上下文句柄。 */
typedef void *gw_spi_t;

/**
 * @brief 创建 SPI 句柄。
 *
 * @param spi_id SPI 总线编号。
 * @return 成功返回 SPI 句柄，失败返回 NULL。
 */
gw_spi_t gw_spi_create(int spi_id);

/**
 * @brief 销毁 SPI 句柄并释放资源。
 *
 * @param ctx 由 @ref gw_spi_create 返回的句柄。
 */
void gw_spi_destroy(gw_spi_t ctx);

/**
 * @brief 写入一个字节。
 *
 * @param ctx SPI 句柄。
 * @param data 待发送的单字节。
 * @return 0 表示成功，负值表示失败。
 */
int gw_spi_write_byte(gw_spi_t ctx, uint8_t data);

/**
 * @brief 连续写入一段数据。
 *
 * @param ctx SPI 句柄。
 * @param data 发送缓冲区。
 * @param data_len 缓冲区长度。
 * @return 成功写入的字节数或错误码。
 */
int gw_spi_write(gw_spi_t ctx, const uint8_t *data, size_t data_len);

/**
 * @brief 连续读取一段数据。
 *
 * @param ctx SPI 句柄。
 * @param data 接收缓冲区。
 * @param data_len 希望读取的长度。
 * @return 成功读取的字节数或错误码。
 */
int gw_spi_read(gw_spi_t ctx, uint8_t *data, size_t data_len);

/**
 * @brief 原地收发同一个缓冲区。
 *
 * 常用于“发送命令同时读取返回”的场景。
 *
 * @param ctx SPI 句柄。
 * @param data 输入输出共用缓冲区。
 * @param data_len 传输长度。
 * @return 0 表示成功，负值表示失败。
 */
int gw_spi_trans_data(gw_spi_t ctx, uint8_t *data, size_t data_len);

/**
 * @brief 使用独立的发送缓冲区和接收缓冲区执行全双工传输。
 *
 * @param ctx SPI 句柄。
 * @param data 发送缓冲区。
 * @param rx_buffer 接收缓冲区。
 * @param data_len 传输长度。
 * @return 0 表示成功，负值表示失败。
 */
int gw_spi_trans_data1(gw_spi_t ctx, const uint8_t *data, uint8_t *rx_buffer, size_t data_len);

#ifdef __cplusplus
}
#endif


