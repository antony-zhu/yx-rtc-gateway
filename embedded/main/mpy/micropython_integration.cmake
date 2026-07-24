set(MICROPY_DIR ${CMAKE_CURRENT_LIST_DIR}/../../third_party/micropython)
set(MICROPY_PORT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(MICROPY_TARGET gw_micropython)
set(USER_C_MODULES ${MICROPY_PORT_DIR}/micropython.cmake)

if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

if(NOT EXISTS ${MICROPY_DIR}/py/mpconfig.h)
    message(FATAL_ERROR "MicroPython source is missing: ${MICROPY_DIR}")
endif()

add_library(${MICROPY_TARGET} STATIC)

target_include_directories(${MICROPY_TARGET} PUBLIC
    ${MICROPY_PORT_DIR}
    ${MICROPY_PORT_DIR}/..
    ${MICROPY_PORT_DIR}/../include
)

function(gw_micropy_add_component_includes component_name)
    idf_component_get_property(gw_component_dir ${component_name} COMPONENT_DIR)
    idf_component_get_property(gw_component_include_dirs ${component_name} INCLUDE_DIRS)
    set(gw_component_include_dirs_abs "")

    foreach(gw_component_inc ${gw_component_include_dirs})
        if(IS_ABSOLUTE "${gw_component_inc}")
            list(APPEND gw_component_include_dirs_abs "${gw_component_inc}")
        else()
            list(APPEND gw_component_include_dirs_abs "${gw_component_dir}/${gw_component_inc}")
        endif()
    endforeach()

    target_include_directories(${MICROPY_TARGET} PUBLIC
        ${gw_component_include_dirs_abs}
    )
endfunction()

gw_micropy_add_component_includes(esp_timer)
gw_micropy_add_component_includes(esp_system)
gw_micropy_add_component_includes(freertos)
gw_micropy_add_component_includes(heap)
gw_micropy_add_component_includes(log)
gw_micropy_add_component_includes(spi_flash)
gw_micropy_add_component_includes(esp_driver_gpio)
gw_micropy_add_component_includes(esp_driver_spi)
gw_micropy_add_component_includes(mbedtls)

include(${MICROPY_DIR}/py/py.cmake)
include(${MICROPY_DIR}/py/usermod.cmake)

list(APPEND MICROPY_QSTRDEFS_PORT
    ${MICROPY_PORT_DIR}/qstrdefsport.h
)

list(APPEND MICROPY_SOURCE_SHARED
    ${MICROPY_DIR}/shared/runtime/gchelper_generic.c
    ${MICROPY_DIR}/shared/timeutils/timeutils.c
)

list(APPEND MICROPY_SOURCE_PORT
    ${MICROPY_DIR}/extmod/modjson.c
    ${MICROPY_DIR}/extmod/modtime.c
    ${MICROPY_PORT_DIR}/mphalport.c
    ${MICROPY_PORT_DIR}/micropython_embed.c
)

set(GW_MICROPY_SERVICE_SOURCES
    ${MICROPY_PORT_DIR}/../crypto/gw_crypto.c
    ${MICROPY_PORT_DIR}/../drivers/gw_spi_driver.c
    ${MICROPY_PORT_DIR}/../ch37x/ch37x_host.c
    ${MICROPY_PORT_DIR}/../ch37x/ch37x_port.c
    ${MICROPY_PORT_DIR}/../ch37x/ch37x_port_spi.c
    ${MICROPY_PORT_DIR}/../board/gw_ch37x_bridge.c
)

list(APPEND MICROPY_SOURCE_QSTR
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_USERMOD}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_PORT}
)

target_sources(${MICROPY_TARGET} PRIVATE
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_PORT}
    ${GW_MICROPY_SERVICE_SOURCES}
)

target_include_directories(${MICROPY_TARGET} PUBLIC
    ${MICROPY_INC_CORE}
    ${MICROPY_INC_USERMOD}
    ${MICROPY_PORT_DIR}
    ${CMAKE_BINARY_DIR}
)

target_compile_options(${MICROPY_TARGET} PUBLIC
    -Wno-clobbered
    -Wno-missing-field-initializers
)

target_compile_definitions(${MICROPY_TARGET} PUBLIC
    MBEDTLS_CONFIG_FILE="mbedtls/esp_config.h"
)

idf_build_get_property(idf_compile_options COMPILE_OPTIONS)
target_compile_options(usermod INTERFACE ${idf_compile_options})
target_link_libraries(${MICROPY_TARGET} PRIVATE usermod)
target_link_libraries(${MICROPY_TARGET} PUBLIC
    idf::esp_driver_spi
)

foreach(comp ${__COMPONENT_NAMES_RESOLVED})
    micropy_gather_target_properties(__idf_${comp})
    micropy_gather_target_properties(${comp})
endforeach()

include(${MICROPY_PORT_DIR}/micropython_mkrules.cmake)

target_link_libraries(${COMPONENT_LIB} PRIVATE ${MICROPY_TARGET})
