cmake_minimum_required(VERSION 3.16)

set(GW_NETWORK_TARGET "4G" CACHE STRING "Gateway network target: 4G or ETHERNET")
set_property(CACHE GW_NETWORK_TARGET PROPERTY STRINGS 4G ETHERNET)
string(TOUPPER "${GW_NETWORK_TARGET}" GW_NETWORK_TARGET_UPPER)
set(GW_NETWORK_TARGET_4G 0)
set(GW_NETWORK_TARGET_ETHERNET 0)
if(GW_NETWORK_TARGET_UPPER STREQUAL "4G")
    set(GW_NETWORK_TARGET_4G 1)
elseif(GW_NETWORK_TARGET_UPPER STREQUAL "ETHERNET" OR
       GW_NETWORK_TARGET_UPPER STREQUAL "ETH")
    set(GW_NETWORK_TARGET_ETHERNET 1)
else()
    message(FATAL_ERROR "GW_NETWORK_TARGET must be 4G or ETHERNET")
endif()

set(GW_NETWORK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GW_NETWORK_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/gw_network_build.h.in"
    "${GW_NETWORK_GENERATED_DIR}/gw_network_build.h"
    @ONLY
)

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(yx_gateway)
