#pragma once

/* 4G module and USB ECM hardware parameters. */
#define GW_4G_USB_VID                         0x2ECC
#define GW_4G_USB_PID                         0x3012
#define GW_4G_USB_AT_INTERFACE                2U
#define GW_4G_PIN_POWER_EN                    12
#define GW_4G_PIN_POWER_CTRL                  46
#define GW_4G_PIN_RESET                       11
#define GW_4G_WAIT_USB_TIMEOUT_MS             30000U
#define GW_4G_ECM_USB_REENUM_TIMEOUT_MS       35000U
#define GW_4G_WAIT_IP_TIMEOUT_MS              45000U
#define GW_4G_POWER_ON_DELAY_MS               3000U
#define GW_4G_POWER_STABLE_MS                 500U
#define GW_4G_POWER_OFF_SETTLE_MS             5000U
#define GW_4G_RESET_PULSE_MS                  200U
#define GW_4G_RESET_DELAY_MS                  5000U
#define GW_4G_HARD_RESET_AFTER_FAILURES       2U
#define GW_4G_USB_TEARDOWN_RETRIES            3U
#define GW_4G_USB_HOST_RELEASE_MS             3000U
#define GW_4G_USB_HOST_POLL_MS                20U
#define GW_4G_CDC_TASK_STACK_BYTES            (12U * 1024U)
#define GW_4G_CDC_TASK_PRIORITY               9U
#define GW_4G_CDC_RX_BUFFER_BYTES             (32U * 1024U)
#define GW_4G_CDC_TX_BUFFER_BYTES             (16U * 1024U)
#define GW_4G_CDC_TRANSFER_BUFFER_BYTES       512U
#define GW_4G_AT_RESPONSE_BUFFER_BYTES        512U
#define GW_4G_AT_READ_TIMEOUT_MS              30U

/* CH397 USB Ethernet hardware parameters. */
#define GW_ETH_PIN_POWER_CTRL                 13
#define GW_ETH_POWER_CTRL_ACTIVE_LEVEL        1
#define GW_ETH_POWER_STABLE_MS                500U
#define GW_ETH_WAIT_USB_TIMEOUT_MS            30000U
#define GW_ETH_WAIT_IP_TIMEOUT_MS             45000U
#define GW_ETH_USB_HOST_RELEASE_MS            3000U
#define GW_ETH_USB_HOST_POLL_MS               20U
#define GW_ETH_CDC_TASK_STACK_BYTES           (12U * 1024U)
#define GW_ETH_CDC_TASK_PRIORITY              9U

/* Board GPIO and fixed peripherals. */
#define GW_BOARD_LED_COUNT                    3U
#define GW_BOARD_LED0_GPIO                    40
#define GW_BOARD_LED1_GPIO                    41
#define GW_BOARD_LED2_GPIO                    42
#define GW_BOARD_LED_ACTIVE_LEVEL             0
#define GW_BOARD_LED_INACTIVE_LEVEL           1
#define GW_BOARD_MQTT_LED_INDEX               0U
#define GW_BOARD_TASK_LED_INDEX               1U
#define GW_BOARD_IDENTIFY_LED_INDEX           2U
#define GW_BOARD_IDENTIFY_BLINK_MS            350U
#define GW_BOARD_RS485_UART                   1
#define GW_BOARD_RS485_TX_GPIO                18
#define GW_BOARD_RS485_RX_GPIO                8
#define GW_BOARD_RS485_BAUDRATE               115200U
#define GW_BOARD_RS485_RX_BUF_BYTES           2048U
#define GW_BOARD_RS485_TX_BUF_BYTES           512U
#define GW_BOARD_RS485_WAIT_TX_MS             1000U

/* External hardware watchdog. */
#define GW_HARDWARE_WATCHDOG_ENABLED          1
#define GW_HARDWARE_WATCHDOG_GPIO             4
#define GW_HARDWARE_WATCHDOG_PULSE_HIGH_MS    100U
#define GW_HARDWARE_WATCHDOG_FEED_INTERVAL_MS (30U * 1000U)
#define GW_HARDWARE_WATCHDOG_TASK_STACK       4096U
#define GW_HARDWARE_WATCHDOG_TASK_PRIORITY    3U

/* CH37x USB host bridge. */
#define GW_CH37X_DEFAULT_INTERFACE            0U
#define GW_CH37X_MAX_INTERFACE                3U
#define GW_CH37X_HOST_TASK_STACK              8192U
#define GW_CH37X_HOST_TASK_PRIORITY           5U
#define GW_CH37X_RX_QUEUE_LEN                 2048U
