if(CMAKE_BUILD_EARLY_EXPANSION)
    # ESP-IDF scans component requirements in a separate CMake process where
    # project-level network variables are not available. SRCS are ignored in
    # that pass, so defer source selection until the real configure pass.
    set(GW_NETWORK_DRIVER_SRC "")
elseif(GW_NETWORK_TARGET_4G)
    set(GW_NETWORK_DRIVER_SRC "src/gw_4g_ecm.c")
elseif(GW_NETWORK_TARGET_ETHERNET)
    set(GW_NETWORK_DRIVER_SRC "src/gw_ethernet.c")
else()
    message(FATAL_ERROR "A gateway network driver must be selected")
endif()

idf_component_register(
    SRCS
        "src/board/gw_board.c"
        "src/board/gw_ch37x_bridge.c"
        "src/drivers/gw_spi_driver.c"
        "src/ch37x/ch37x_host.c"
        "src/ch37x/ch37x_port.c"
        "src/ch37x/ch37x_port_spi.c"
        "src/gw_rtc_ysn8010.c"
        "src/meter/gw_dtsu666.c"
        "${GW_NETWORK_DRIVER_SRC}"
        "src/gw_hardware_watchdog.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/ch37x"
    REQUIRES
        esp_timer
        esp_event
        esp_netif
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_uart
        esp_system
        freertos
        heap
        log
        usb
        iot_eth
        iot_usbh_cdc
        iot_usbh_ecm
)
