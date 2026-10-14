#pragma once 

#include <stdint.h>
#include <stdbool.h>

/**
 * @file ch37x_port_spi.h
 * @brief CH37X SPI 端口读写适配接口。
 *
 * CH37X 寄存器访问通过命令地址和数据字节完成，块读写用于 USB FIFO 收发。
 */

/**
 * @brief 初始化 CH37X SPI 端口。
 */
void ch37x_port_spi_init();

/**
 * @brief 从 CH37X 寄存器读取一个字节。
 *
 * @param mAddr 寄存器地址。
 * @return 读取到的数据字节。
 */
uint8_t ch37x_port_spi_read_byte(uint8_t mAddr);

/**
 * @brief 向 CH37X 寄存器写入一个字节。
 *
 * @param mAddr 寄存器地址。
 * @param mData 待写数据。
 * @return 0 表示成功，负值表示失败。
 */
int ch37x_port_spi_write_byte(uint8_t mAddr, uint8_t mData);

/**
 * @brief 从 CH37X FIFO/寄存器连续读取数据。
 *
 * @param mAddr 起始地址或 FIFO 地址。
 * @param mLen 读取长度。
 * @param mBuf 输出缓冲区。
 * @return 0 表示成功，负值表示失败。
 */
int ch37x_port_spi_read_block(uint8_t mAddr, uint8_t mLen, uint8_t *mBuf) ;

/**
 * @brief 向 CH37X FIFO/寄存器连续写入数据。
 *
 * @param mAddr 起始地址或 FIFO 地址。
 * @param mLen 写入长度。
 * @param mBuf 输入缓冲区。
 * @return 0 表示成功，负值表示失败。
 */
int ch37x_port_spi_write_block(uint8_t mAddr, uint8_t mLen, const uint8_t *mBuf);

