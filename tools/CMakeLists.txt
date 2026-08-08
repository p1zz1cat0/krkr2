cmake_minimum_required(VERSION 3.28)
project(Tools LANGUAGES CXX)

if (WIN32)
    set(OS_PLATFORM win)
elseif (APPLE)
    set(OS_PLATFORM mac)
elseif (UNIX)
    set(OS_PLATFORM linux)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin/${OS_PLATFORM}/rel")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/bin/${OS_PLATFORM}/dbg")

# 工具链专用存储（依赖 krkr2core 已注册的 tjs2 / core_io / archive_xp3）
add_subdirectory(${CMAKE_SOURCE_DIR}/cpp/core/storage/tool ${CMAKE_CURRENT_BINARY_DIR}/storage_tool)

add_subdirectory(xp3)
