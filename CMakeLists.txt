# SPDX-FileCopyrightText: 2026 Alliance
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.25)

project(
    teleop
    VERSION 0.1.0
    DESCRIPTION "RoboMaster custom-terminal client"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CTest)

# ---------------------------------------------------------------------------
# 语言与构建
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(TELEOP_WERROR "把警告当错误" OFF)
option(TELEOP_BUILD_TOOLS "构建模拟器与工具" ON)

# ccache：装了就用；容器里 /ccache 是持久卷
find_program(TELEOP_CCACHE ccache)
if(TELEOP_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${TELEOP_CCACHE}")
endif()

# ---------------------------------------------------------------------------
# 公共编译选项：target_link_libraries(<target> PRIVATE teleop_options)
# ---------------------------------------------------------------------------
add_library(teleop_options INTERFACE)

target_compile_options(
    teleop_options
    INTERFACE -Wall
              -Wextra
              -Wpedantic
              -Wshadow
              -Wnon-virtual-dtor
              -Woverloaded-virtual
              -Wdouble-promotion
              -Wformat=2
)

if(TELEOP_WERROR)
    target_compile_options(teleop_options INTERFACE -Werror)
endif()

# x86-64-v2 比 native 保守，换机器不会 SIGILL（架构 §14.3 第 11 条）
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(teleop_options INTERFACE -march=x86-64-v2)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_link_options(-Wl,-z,relro,-z,now)
endif()

# 预设里可能通过 cache 打开 LTO；这里只做兜底与展示，不覆盖用户的设置
if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
endif()

# ---------------------------------------------------------------------------
# 公共 include 根
# 不只是给 target 用：clangd 给头文件推断编译命令时，会借用某条 TU 的命令，
# 只有把这些根放到全局，头文件才能在任何一条命令下都解析得到。
# ---------------------------------------------------------------------------
include_directories(
    ${PROJECT_SOURCE_DIR}/proto
    ${PROJECT_SOURCE_DIR}/libs/rm_proto/include
    ${PROJECT_SOURCE_DIR}/libs/teleop_ipc/include
    ${PROJECT_SOURCE_DIR}/libs/teleop_ipc/third_party
    ${PROJECT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# 子目录：按批次逐行打开（见 docs/teleop-client-implementation-plan.md）
# ---------------------------------------------------------------------------
add_subdirectory(proto)
add_subdirectory(libs/rm_proto)
add_subdirectory(libs/teleop_ipc)
add_subdirectory(src)

if(TELEOP_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# B4: src/node（在 src/CMakeLists.txt 里打开）

message(STATUS "teleop ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  werror     : ${TELEOP_WERROR}")
message(STATUS "  lto        : ${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
message(STATUS "  ccache     : ${TELEOP_CCACHE}")
message(STATUS "  tools      : ${TELEOP_BUILD_TOOLS}")
