# Windows 友好的 MicroPython 生成规则。
#
# 官方 py/mkrules.cmake 在类 Unix 环境下工作良好，但在当前
# Windows + ESP-IDF + Ninja 组合下会遇到几类问题：
# 1. qstr 预处理命令过长，超过 cmd.exe 限制。
# 2. 后续规则依赖 touch/cat/sed/重定向，不适合默认 Windows shell。
# 3. 这些问题会在真正编译 C 代码前就把构建截断。
#
# 这里保留官方变量命名和产物结构，只替换为 Python/CMake 可稳定执行
# 的实现，方便后续继续跟官方规则对齐。

set(MICROPY_GENHDR_DIR "${CMAKE_BINARY_DIR}/genhdr")
set(MICROPY_MPVERSION "${MICROPY_GENHDR_DIR}/mpversion.h")
set(MICROPY_QSTRDEFS_PY "${MICROPY_PY_DIR}/qstrdefs.h")
set(MICROPY_QSTRDEFS_LAST "${MICROPY_GENHDR_DIR}/qstr.i.last")
set(MICROPY_QSTRDEFS_SPLIT "${MICROPY_GENHDR_DIR}/qstr.split")
set(MICROPY_QSTRDEFS_COLLECTED "${MICROPY_GENHDR_DIR}/qstrdefs.collected.h")
set(MICROPY_QSTRDEFS_PREPROCESSED "${MICROPY_GENHDR_DIR}/qstrdefs.preprocessed.h")
set(MICROPY_QSTRDEFS_GENERATED "${MICROPY_GENHDR_DIR}/qstrdefs.generated.h")
set(MICROPY_MODULEDEFS_SPLIT "${MICROPY_GENHDR_DIR}/moduledefs.split")
set(MICROPY_MODULEDEFS_COLLECTED "${MICROPY_GENHDR_DIR}/moduledefs.collected")
set(MICROPY_MODULEDEFS "${MICROPY_GENHDR_DIR}/moduledefs.h")
set(MICROPY_ROOT_POINTERS_SPLIT "${MICROPY_GENHDR_DIR}/root_pointers.split")
set(MICROPY_ROOT_POINTERS_COLLECTED "${MICROPY_GENHDR_DIR}/root_pointers.collected")
set(MICROPY_ROOT_POINTERS "${MICROPY_GENHDR_DIR}/root_pointers.h")
set(MICROPY_COMPRESSED_SPLIT "${MICROPY_GENHDR_DIR}/compressed.split")
set(MICROPY_COMPRESSED_COLLECTED "${MICROPY_GENHDR_DIR}/compressed.collected")
set(MICROPY_COMPRESSED_DATA "${MICROPY_GENHDR_DIR}/compressed.data.h")

if(NOT MICROPY_PREVIEW_VERSION_2)
    set(MICROPY_PREVIEW_VERSION_2 0)
endif()

if(MICROPY_BOARD)
    if(MICROPY_BOARD_VARIANT)
        set(MICROPY_BOARD_BUILD_NAME ${MICROPY_BOARD}-${MICROPY_BOARD_VARIANT})
    else()
        set(MICROPY_BOARD_BUILD_NAME ${MICROPY_BOARD})
    endif()

    target_compile_definitions(${MICROPY_TARGET} PRIVATE
        MICROPY_BOARD_BUILD_NAME="${MICROPY_BOARD_BUILD_NAME}"
    )
endif()

if(MICROPY_ROM_TEXT_COMPRESSION)
    target_compile_definitions(${MICROPY_TARGET} PUBLIC
        MICROPY_ROM_TEXT_COMPRESSION=\(1\)
    )
endif()

if(MICROPY_FROZEN_MANIFEST)
    target_compile_definitions(${MICROPY_TARGET} PUBLIC
        MICROPY_QSTR_EXTRA_POOL=mp_qstr_frozen_const_pool
        MICROPY_MODULE_FROZEN_MPY=\(1\)
    )
endif()

if(MICROPY_PREVIEW_VERSION_2)
    target_compile_definitions(${MICROPY_TARGET} PUBLIC
        MICROPY_PREVIEW_VERSION_2=\(1\)
    )
endif()

if(NOT MICROPY_CPP_FLAGS)
    get_target_property(MICROPY_CPP_INC ${MICROPY_TARGET} INCLUDE_DIRECTORIES)
    get_target_property(MICROPY_CPP_DEF ${MICROPY_TARGET} COMPILE_DEFINITIONS)
endif()

if(MICROPY_CPP_INC STREQUAL "MICROPY_CPP_INC-NOTFOUND")
    set(MICROPY_CPP_INC "")
endif()
if(MICROPY_CPP_DEF STREQUAL "MICROPY_CPP_DEF-NOTFOUND")
    set(MICROPY_CPP_DEF "")
endif()

if(MICROPY_DIR)
    get_filename_component(_micropy_dir_abs "${MICROPY_DIR}" ABSOLUTE)
    list(APPEND MICROPY_CPP_INC ${_micropy_dir_abs})
endif()

list(APPEND MICROPY_CPP_INC ${MICROPY_CPP_INC_EXTRA})
list(APPEND MICROPY_CPP_DEF ${MICROPY_CPP_DEF_EXTRA})
list(FILTER MICROPY_CPP_INC EXCLUDE REGEX "^$")
list(FILTER MICROPY_CPP_INC EXCLUDE REGEX ".*-NOTFOUND$")
list(FILTER MICROPY_CPP_DEF EXCLUDE REGEX "^$")
list(FILTER MICROPY_CPP_DEF EXCLUDE REGEX ".*-NOTFOUND$")
list(REMOVE_DUPLICATES MICROPY_CPP_INC)
list(REMOVE_DUPLICATES MICROPY_CPP_DEF)

set(_prefix "-I")
foreach(_arg ${MICROPY_CPP_INC})
    list(APPEND MICROPY_CPP_FLAGS ${_prefix}${_arg})
endforeach()
set(_prefix "-D")
foreach(_arg ${MICROPY_CPP_DEF})
    list(APPEND MICROPY_CPP_FLAGS ${_prefix}${_arg})
endforeach()
list(APPEND MICROPY_CPP_FLAGS ${MICROPY_CPP_FLAGS_EXTRA})
list(REMOVE_DUPLICATES MICROPY_CPP_FLAGS)
list(FILTER MICROPY_CPP_FLAGS EXCLUDE REGEX "^-I$|^-D$|.*-NOTFOUND$")

if(DEFINED ENV{CFLAGS_EXTRA})
    set(CFLAGS_EXTRA $ENV{CFLAGS_EXTRA})
    string(APPEND CMAKE_C_FLAGS " ${CFLAGS_EXTRA}")
    separate_arguments(CFLAGS_EXTRA)
    list(APPEND MICROPY_CPP_FLAGS ${CFLAGS_EXTRA})
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

target_sources(${MICROPY_TARGET} PRIVATE
    ${MICROPY_MPVERSION}
    ${MICROPY_QSTRDEFS_GENERATED}
    ${MICROPY_MODULEDEFS}
    ${MICROPY_ROOT_POINTERS}
)

if(MICROPY_ROM_TEXT_COMPRESSION)
    target_sources(${MICROPY_TARGET} PRIVATE
        ${MICROPY_COMPRESSED_DATA}
    )
endif()

set_property(TARGET ${MICROPY_TARGET} APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "${MICROPY_GENHDR_DIR}"
)

set(MICROPY_CPP_FLAGS_FILE "${CMAKE_BINARY_DIR}/micropy_cpp_flags.txt")
set(MICROPY_CXX_FLAGS_FILE "${CMAKE_BINARY_DIR}/micropy_cxx_flags.txt")
set(MICROPY_QSTR_SOURCES_FILE "${CMAKE_BINARY_DIR}/micropy_qstr_sources.txt")
set(MICROPY_QSTR_PORTS_FILE "${CMAKE_BINARY_DIR}/micropy_qstr_ports.txt")

string(REPLACE ";" "\n" _micropy_cpp_flags_content "${MICROPY_CPP_FLAGS}")
set(_micropy_qstr_cpp_flags ${MICROPY_CPP_FLAGS})
list(APPEND _micropy_qstr_cpp_flags -DNO_QSTR)
string(REPLACE ";" "\n" _micropy_qstr_cpp_flags_content "${_micropy_qstr_cpp_flags}")
string(REPLACE ";" "\n" _micropy_qstr_sources_content "${MICROPY_SOURCE_QSTR}")
string(REPLACE ";" "\n" _micropy_qstr_ports_content "${MICROPY_QSTRDEFS_PORT}")

file(GENERATE OUTPUT "${MICROPY_CPP_FLAGS_FILE}" CONTENT "${_micropy_qstr_cpp_flags_content}\n")
file(GENERATE OUTPUT "${MICROPY_CXX_FLAGS_FILE}" CONTENT "${_micropy_qstr_cpp_flags_content}\n")
file(GENERATE OUTPUT "${MICROPY_QSTR_SOURCES_FILE}" CONTENT "${_micropy_qstr_sources_content}\n")
file(GENERATE OUTPUT "${MICROPY_QSTR_PORTS_FILE}" CONTENT "${_micropy_qstr_ports_content}\n")

add_custom_target(
    BUILD_VERSION_HEADER ALL
    BYPRODUCTS ${MICROPY_MPVERSION}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MICROPY_GENHDR_DIR}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeversionhdr.py ${MICROPY_MPVERSION}
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_LAST}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PORT_DIR}/tools/micropython_qstr_pp.py
        --compiler ${CMAKE_C_COMPILER}
        --cflags-file ${MICROPY_CPP_FLAGS_FILE}
        --cxxflags-file ${MICROPY_CXX_FLAGS_FILE}
        --sources-file ${MICROPY_QSTR_SOURCES_FILE}
        --output ${MICROPY_QSTRDEFS_LAST}
    DEPENDS ${MICROPY_MPVERSION}
        ${MICROPY_SOURCE_QSTR}
        ${MICROPY_CPP_FLAGS_FILE}
        ${MICROPY_CXX_FLAGS_FILE}
        ${MICROPY_QSTR_SOURCES_FILE}
        ${MICROPY_PORT_DIR}/tools/micropython_qstr_pp.py
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_SPLIT}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py split qstr ${MICROPY_GENHDR_DIR}/qstr.i.last ${MICROPY_GENHDR_DIR}/qstr _
    COMMAND ${CMAKE_COMMAND} -E touch ${MICROPY_QSTRDEFS_SPLIT}
    DEPENDS ${MICROPY_QSTRDEFS_LAST}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_COLLECTED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py cat qstr _ ${MICROPY_GENHDR_DIR}/qstr ${MICROPY_QSTRDEFS_COLLECTED}
    BYPRODUCTS "${MICROPY_QSTRDEFS_COLLECTED}.hash"
    DEPENDS ${MICROPY_QSTRDEFS_SPLIT}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_PREPROCESSED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PORT_DIR}/tools/micropython_qstr_preprocessed.py
        --compiler ${CMAKE_C_COMPILER}
        --flags-file ${MICROPY_CPP_FLAGS_FILE}
        --qstrdefs ${MICROPY_QSTRDEFS_PY}
        --ports-file ${MICROPY_QSTR_PORTS_FILE}
        --collected ${MICROPY_QSTRDEFS_COLLECTED}
        --output ${MICROPY_QSTRDEFS_PREPROCESSED}
    DEPENDS ${MICROPY_QSTRDEFS_PY}
        ${MICROPY_QSTRDEFS_PORT}
        ${MICROPY_QSTRDEFS_COLLECTED}
        ${MICROPY_CPP_FLAGS_FILE}
        ${MICROPY_QSTR_PORTS_FILE}
        ${MICROPY_PORT_DIR}/tools/micropython_qstr_preprocessed.py
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_GENERATED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PORT_DIR}/tools/micropython_run_to_file.py
        ${MICROPY_PY_DIR}/makeqstrdata.py
        ${MICROPY_QSTRDEFS_GENERATED}
        ${MICROPY_QSTRDEFS_PREPROCESSED}
    DEPENDS ${MICROPY_QSTRDEFS_PREPROCESSED}
        ${MICROPY_PORT_DIR}/tools/micropython_run_to_file.py
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_MODULEDEFS_SPLIT}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py split module ${MICROPY_GENHDR_DIR}/qstr.i.last ${MICROPY_GENHDR_DIR}/module _
    COMMAND ${CMAKE_COMMAND} -E touch ${MICROPY_MODULEDEFS_SPLIT}
    DEPENDS ${MICROPY_QSTRDEFS_LAST}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_MODULEDEFS_COLLECTED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py cat module _ ${MICROPY_GENHDR_DIR}/module ${MICROPY_MODULEDEFS_COLLECTED}
    BYPRODUCTS "${MICROPY_MODULEDEFS_COLLECTED}.hash"
    DEPENDS ${MICROPY_MODULEDEFS_SPLIT}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_MODULEDEFS}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PORT_DIR}/tools/micropython_run_to_file.py
        ${MICROPY_PY_DIR}/makemoduledefs.py
        ${MICROPY_MODULEDEFS}
        ${MICROPY_MODULEDEFS_COLLECTED}
    DEPENDS ${MICROPY_MODULEDEFS_COLLECTED}
        ${MICROPY_PY_DIR}/makemoduledefs.py
        ${MICROPY_PORT_DIR}/tools/micropython_run_to_file.py
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_ROOT_POINTERS_SPLIT}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py split root_pointer ${MICROPY_GENHDR_DIR}/qstr.i.last ${MICROPY_GENHDR_DIR}/root_pointer _
    COMMAND ${CMAKE_COMMAND} -E touch ${MICROPY_ROOT_POINTERS_SPLIT}
    DEPENDS ${MICROPY_QSTRDEFS_LAST}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_ROOT_POINTERS_COLLECTED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py cat root_pointer _ ${MICROPY_GENHDR_DIR}/root_pointer ${MICROPY_ROOT_POINTERS_COLLECTED}
    BYPRODUCTS "${MICROPY_ROOT_POINTERS_COLLECTED}.hash"
    DEPENDS ${MICROPY_ROOT_POINTERS_SPLIT}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_ROOT_POINTERS}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_PORT_DIR}/tools/micropython_run_to_file.py
        ${MICROPY_PY_DIR}/make_root_pointers.py
        ${MICROPY_ROOT_POINTERS}
        ${MICROPY_ROOT_POINTERS_COLLECTED}
    DEPENDS ${MICROPY_ROOT_POINTERS_COLLECTED}
        ${MICROPY_PY_DIR}/make_root_pointers.py
        ${MICROPY_PORT_DIR}/tools/micropython_run_to_file.py
    VERBATIM
    COMMAND_EXPAND_LISTS
)

if(MICROPY_ROM_TEXT_COMPRESSION)
    add_custom_command(
        OUTPUT ${MICROPY_COMPRESSED_SPLIT}
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py split compress ${MICROPY_QSTRDEFS_LAST} ${MICROPY_GENHDR_DIR}/compress _
        COMMAND ${CMAKE_COMMAND} -E touch ${MICROPY_COMPRESSED_SPLIT}
        DEPENDS ${MICROPY_QSTRDEFS_LAST}
        VERBATIM
        COMMAND_EXPAND_LISTS
    )

    add_custom_command(
        OUTPUT ${MICROPY_COMPRESSED_COLLECTED}
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py cat compress _ ${MICROPY_GENHDR_DIR}/compress ${MICROPY_COMPRESSED_COLLECTED}
        BYPRODUCTS "${MICROPY_COMPRESSED_COLLECTED}.hash"
        DEPENDS ${MICROPY_COMPRESSED_SPLIT}
        VERBATIM
        COMMAND_EXPAND_LISTS
    )

    add_custom_command(
        OUTPUT ${MICROPY_COMPRESSED_DATA}
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PORT_DIR}/tools/micropython_run_to_file.py
            ${MICROPY_PY_DIR}/makecompresseddata.py
            ${MICROPY_COMPRESSED_DATA}
            ${MICROPY_COMPRESSED_COLLECTED}
        DEPENDS ${MICROPY_COMPRESSED_COLLECTED}
            ${MICROPY_PY_DIR}/makecompresseddata.py
            ${MICROPY_PORT_DIR}/tools/micropython_run_to_file.py
        VERBATIM
        COMMAND_EXPAND_LISTS
    )
endif()
