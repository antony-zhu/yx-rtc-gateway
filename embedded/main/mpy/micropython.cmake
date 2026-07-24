add_library(usermod_gateway_builtin INTERFACE)

target_sources(usermod_gateway_builtin INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mod_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_system.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_fs.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_config.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_net.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_mqtt.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_http.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_board.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_ch37x.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_crypto.c
    ${CMAKE_CURRENT_LIST_DIR}/gw_py_bridge.c
)

target_include_directories(usermod_gateway_builtin INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..
    ${CMAKE_CURRENT_LIST_DIR}/../include
    ${CMAKE_CURRENT_LIST_DIR}/../board
    ${CMAKE_CURRENT_LIST_DIR}/../drivers
    ${CMAKE_CURRENT_LIST_DIR}/../ch37x
    ${CMAKE_CURRENT_LIST_DIR}/../crypto
)

target_link_libraries(usermod INTERFACE usermod_gateway_builtin)
