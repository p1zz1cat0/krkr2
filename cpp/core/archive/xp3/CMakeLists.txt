cmake_minimum_required(VERSION 3.19)
project(archive_xp3 LANGUAGES CXX)

set(ARCHIVE_XP3_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BASE_PATH ${CMAKE_SOURCE_DIR}/cpp/core/base)
set(UTILS_PATH ${CMAKE_SOURCE_DIR}/cpp/core/utils)

add_library(${PROJECT_NAME} STATIC
    ${ARCHIVE_XP3_PATH}/XP3Archive.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${ARCHIVE_XP3_PATH}
    ${BASE_PATH}
    ${BASE_PATH}/impl
    ${UTILS_PATH}
)

target_link_libraries(${PROJECT_NAME} PUBLIC tjs2)

find_package(ZLIB REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
