cmake_minimum_required(VERSION 3.19)
project(core_base_module LANGUAGES CXX)

set(BASE_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BASE_SOURCE_FILES
    ${BASE_PATH}/BinaryStream.cpp
    ${BASE_PATH}/CharacterSet.cpp
    ${BASE_PATH}/EventIntf.cpp
    ${BASE_PATH}/ScriptMgnIntf.cpp
    ${BASE_PATH}/SysInitIntf.cpp
    ${BASE_PATH}/SystemIntf.cpp
    ${BASE_PATH}/TextStream.cpp
    ${BASE_PATH}/KAGParser.cpp

    ${BASE_PATH}/impl/EventImpl.cpp
    ${BASE_PATH}/impl/FileSelector.cpp
    ${BASE_PATH}/impl/NativeEventQueue.cpp
    ${BASE_PATH}/impl/ScriptMgnImpl.cpp
    ${BASE_PATH}/impl/StorageImpl.cpp
    ${BASE_PATH}/impl/LocalFileBackend.cpp
    ${BASE_PATH}/impl/FileBackendInit.cpp
    ${BASE_PATH}/impl/EngineArchiveInit.cpp
    ${BASE_PATH}/impl/SysInitImpl.cpp
    ${BASE_PATH}/impl/SystemImpl.cpp
)

set(BASE_HEADERS_DIR
        ${BASE_PATH}/
        ${BASE_PATH}/impl
)

add_library(${PROJECT_NAME} STATIC ${BASE_SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC ${BASE_HEADERS_DIR})
target_link_libraries(${PROJECT_NAME} PUBLIC tjs2 core_msg archive_xp3 archive_zip archive_7z archive_tar core_io core_storage PRIVATE core_visual_module core_plugin_module core_environ_module core_extension_module core_sound_module core_utils_module)

find_package(uchardet CONFIG REQUIRED)
find_package(unrar CONFIG REQUIRED)
find_package(LibXml2 REQUIRED)
include_directories(${LIBXML2_INCLUDE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBXML2_LIBRARIES})
find_package(LibArchive REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(cocos2dx CONFIG REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
    uchardet::libuchardet
    unrar::unrar
    LibArchive::LibArchive
    zstd::libzstd

    cocos2dx::cocos2d
)
