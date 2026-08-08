cmake_minimum_required(VERSION 3.19)
project(archive_zip LANGUAGES CXX)

set(ARCHIVE_ZIP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BASE_PATH ${CMAKE_SOURCE_DIR}/cpp/core/base)

add_library(${PROJECT_NAME} STATIC
    ${ARCHIVE_ZIP_PATH}/ZIPArchive.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${ARCHIVE_ZIP_PATH}
    ${BASE_PATH}
    ${BASE_PATH}/impl
)

target_link_libraries(${PROJECT_NAME} PUBLIC core_storage)

find_package(cocos2dx CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE cocos2dx::cocos2d ZLIB::ZLIB)
