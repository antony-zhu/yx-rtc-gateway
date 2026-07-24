idf_component_register(SRCS "iot_usbh_ecm.c" "iot_usbh_ecm_descriptor.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES esp_ringbuf iot_usbh_cdc iot_eth esp_netif esp_event
                       )

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
