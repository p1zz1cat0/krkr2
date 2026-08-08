cmake_minimum_required(VERSION 3.19)
project(core_storage_tool LANGUAGES CXX)

set(STORAGE_TOOL_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BASE_PATH ${CMAKE_SOURCE_DIR}/cpp/core/base)
set(ARCHIVE_XP3_PATH ${CMAKE_SOURCE_DIR}/cpp/core/archive/xp3)

set(STORAGE_TOOL_SOURCE_FILES
    ${STORAGE_TOOL_PATH}/ArchiveBase.cpp
    ${STORAGE_TOOL_PATH}/CreateStreamMin.cpp
    ${STORAGE_TOOL_PATH}/MemoryStreamMin.cpp
    ${STORAGE_TOOL_PATH}/OpenArchiveMin.cpp
    ${STORAGE_TOOL_PATH}/StorageUtilMin.cpp
    ${STORAGE_TOOL_PATH}/ToolRuntimeStubs.cpp
    ${STORAGE_TOOL_PATH}/ToolMsgMin.cpp
)

add_library(${PROJECT_NAME} STATIC ${STORAGE_TOOL_SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC
    ${STORAGE_TOOL_PATH}
    ${BASE_PATH}
    ${BASE_PATH}/impl
    ${ARCHIVE_XP3_PATH}
)
target_link_libraries(${PROJECT_NAME} PUBLIC tjs2 core_io archive_xp3)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
