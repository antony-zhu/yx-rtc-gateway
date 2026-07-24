#include <string.h>

#include "ch37x_inc.h"
/**
 * @file ch37x_port_spi.c
 * @brief CH37X SPI 端口访问实现。
 *
 * 该文件把 CH37X 寄存器读写命令映射到项目通用 SPI 封装，供 host 枚举和数据
 * 传输流程调用。
 */
#include "ch37x_port_spi.h"
#include "esp_log.h"
#include "drivers/gw_spi_driver.h"

static const char *TAG = "CH37x_SPI_Port";

#define CMD_SPI_374READ   0xC0
#define CMD_SPI_374WRITE  0x80
#define CH37X_SPI_MAX_FRAME 257U

static gw_spi_t spi_handle = NULL;

/**
 * @brief 确保 CH37X SPI 句柄已创建。
 *
 * 该函数采用懒初始化方式，避免 CH37X 驱动未启用时提前占用 SPI 资源。
 *
 * @return 0 表示 SPI 已可用，负值表示初始化失败。
 */
static int ch37x_spi_ensure_ready(void)
{
    if (spi_handle != NULL) {
        return 0;
    }

    spi_handle = gw_spi_create(2);
    if (spi_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create SPI handle");
        return -1;
    }

    return 0;
}

/**
 * @brief 初始化 CH37X SPI 端口。
 */
void ch37x_port_spi_init(void)
{
    if (ch37x_spi_ensure_ready() != 0) {
        ESP_LOGE(TAG, "SPI init failed");
    }
}

/**
 * @brief 从 CH37X 寄存器读取一个字节。
 *
 * @param mAddr 寄存器地址。
 * @return 读取到的数据；失败时返回 0 并写错误日志。
 */
uint8_t ch37x_port_spi_read_byte(uint8_t mAddr)
{
    uint8_t data = 0;
    if (ch37x_port_spi_read_block(mAddr, 1, &data) < 0) {
        ESP_LOGE(TAG, "SPI read byte failed, addr=0x%02X", mAddr);
    }
    return data;
}

/**
 * @brief 向 CH37X 寄存器写入一个字节。
 *
 * @param mAddr 寄存器地址。
 * @param mData 待写入数据。
 * @return 成功返回写入长度，失败返回负值。
 */
int ch37x_port_spi_write_byte(uint8_t mAddr, uint8_t mData)
{
    return ch37x_port_spi_write_block(mAddr, 1, &mData);
}

/**
 * @brief 从 CH37X FIFO 或寄存器连续读取数据。
 *
 * @param mAddr 起始地址或 FIFO 地址。
 * @param mLen 读取长度。
 * @param mBuf 输出缓冲区。
 * @return 成功返回读取长度，失败返回负值。
 */
int ch37x_port_spi_read_block(uint8_t mAddr, uint8_t mLen, uint8_t *mBuf)
{
    if (mLen == 0 || mBuf == NULL) {
        return -1;
    }
    if (ch37x_spi_ensure_ready() != 0) {
        return -1;
    }

    uint8_t buffer[CH37X_SPI_MAX_FRAME];
    size_t frame_len = (size_t)mLen + 2U;
    if (frame_len > sizeof(buffer)) {
        ESP_LOGE(TAG, "SPI read frame too large: %u", (unsigned)frame_len);
        return -1;
    }

    buffer[0] = mAddr;
    buffer[1] = CMD_SPI_374READ;
    if (gw_spi_trans_data(spi_handle, buffer, frame_len) < 0) {
        ESP_LOGE(TAG, "SPI read block failed, addr=0x%02X len=%u", mAddr, mLen);
        return -1;
    }

    memcpy(mBuf, buffer + 2, mLen);
    return (int)mLen;
}

/**
 * @brief 向 CH37X FIFO 或寄存器连续写入数据。
 *
 * @param mAddr 起始地址或 FIFO 地址。
 * @param mLen 写入长度。
 * @param mBuf 输入缓冲区。
 * @return 成功返回写入长度，失败返回负值。
 */
int ch37x_port_spi_write_block(uint8_t mAddr, uint8_t mLen, const uint8_t *mBuf)
{
    if (mLen == 0 || mBuf == NULL) {
        return -1;
    }
    if (ch37x_spi_ensure_ready() != 0) {
        return -1;
    }

    uint8_t buffer[CH37X_SPI_MAX_FRAME];
    size_t frame_len = (size_t)mLen + 2U;
    if (frame_len > sizeof(buffer)) {
        ESP_LOGE(TAG, "SPI write frame too large: %u", (unsigned)frame_len);
        return -1;
    }

    buffer[0] = mAddr;
    buffer[1] = CMD_SPI_374WRITE;
    memcpy(buffer + 2, mBuf, mLen);
    if (gw_spi_trans_data1(spi_handle, buffer, NULL, frame_len) < 0) {
        ESP_LOGE(TAG, "SPI write block failed, addr=0x%02X len=%u", mAddr, mLen);
        return -1;
    }

    return (int)mLen;
}


