cmake_minimum_required(VERSION 3.19)
project(core_storage LANGUAGES CXX)

set(STORAGE_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BASE_PATH ${CMAKE_SOURCE_DIR}/cpp/core/base)
set(UTILS_PATH ${CMAKE_SOURCE_DIR}/cpp/core/utils)

set(CORE_STORAGE_SOURCE_FILES
    ${BASE_PATH}/StorageIntf.cpp
    ${BASE_PATH}/UtilStreams.cpp
    ${STORAGE_PATH}/impl/ArchiveRegistry.cpp
    ${STORAGE_PATH}/impl/ArchiveFilename.cpp
    ${STORAGE_PATH}/impl/ArchiveStream.cpp
    ${STORAGE_PATH}/ToolStorageInit.cpp
)

add_library(${PROJECT_NAME} STATIC ${CORE_STORAGE_SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC
    ${STORAGE_PATH}
    ${STORAGE_PATH}/impl
    ${BASE_PATH}
    ${BASE_PATH}/impl
)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${UTILS_PATH}
)
target_link_libraries(${PROJECT_NAME} PUBLIC tjs2 core_io core_msg archive_xp3)

find_package(unrar CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE unrar::unrar LibArchive::LibArchive)
