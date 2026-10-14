set(GW_APP_SRCS
        "src/gw_app.c"
        "src/common/gw_event.c"
        "src/common/gw_redact.c"
        "src/time/gw_time.c"
        "src/time/gw_time_network.c"
        "src/time/gw_scheduler.c"
        "src/time/gw_cron.c"
        "src/log/gw_log.c"
        "src/supervisor/gw_health.c"
        "src/crypto/gw_crypto.c"
        "src/storage/gw_storage.c"
        "src/fs/gw_fs.c"
        "src/gw/gw_state.c"
        "src/gw/gw_supervisor.c"
        "src/gw/gw_reboot_manager.c"
        "src/log/gw_log_task.c"
        "src/net/gw_net_manager.c"
        "src/http/gw_http_client.c"
        "src/mqtt/gw_mqtt_client.c"
        "src/mqtt/gw_business_mqtt_register.c"
        "src/mqtt/gw_business_mqtt.c"
        "src/mqtt/gw_dongle_filter.c"
        "src/mqtt/gw_dongle_mqtt_bridge.c"
        "src/ymzl/gw_ymzl_protocol.c"
        "src/ymzl/gw_gs200_protocol.c"
        "src/config/gw_config_manager.c"
        "src/ota/gw_ota.c"
        "src/gw/gw_mqtt_manager.c"
        "src/gw/gw_protocol_manager.c"
        "src/gw/gw_gateway_protocol.c"
        "src/time/gw_gateway_timer.c"
        "src/gw/gw_command_dispatcher.c"
        "src/gw/gw_action_executor.c"
        "src/meter/gw_meter_reporter.c"
)

if(CONFIG_BT_ENABLED)
    list(APPEND GW_APP_SRCS "src/ble/gw_ble_config.c")
endif()

set(GW_APP_REQUIRES
        gw_drivers
        app_update
        esp_timer
        esp_event
        esp_netif
        esp_partition
        esp_hw_support
        esp_system
        heap
        esp_http_client
        esp-tls
        freertos
        json
        log
        nvs_flash
        mbedtls
        mqtt
        spi_flash
        littlefs
)

if(CONFIG_BT_ENABLED)
    list(APPEND GW_APP_REQUIRES bt)
endif()

idf_component_register(
    SRCS ${GW_APP_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/crypto" "src/ota"
    REQUIRES ${GW_APP_REQUIRES}
)

target_include_directories(${COMPONENT_LIB} PRIVATE "${GW_NETWORK_GENERATED_DIR}")
