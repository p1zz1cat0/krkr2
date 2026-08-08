cmake_minimum_required(VERSION 3.19)
project(archive_tar LANGUAGES CXX)

set(ARCHIVE_TAR_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BASE_PATH ${CMAKE_SOURCE_DIR}/cpp/core/base)

add_library(${PROJECT_NAME} STATIC
    ${ARCHIVE_TAR_PATH}/TARArchive.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${ARCHIVE_TAR_PATH}
    ${BASE_PATH}
    ${BASE_PATH}/impl
)

target_link_libraries(${PROJECT_NAME} PUBLIC core_storage archive_xp3)
