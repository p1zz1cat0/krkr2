cmake_minimum_required(VERSION 3.19)
project(core_telemetry_module LANGUAGES CXX)

set(TELEMETRY_PATH ${CMAKE_CURRENT_LIST_DIR})
set(TELEMETRY_SOURCE_FILES
    ${TELEMETRY_PATH}/YoghourtTelemetryQueue.cpp
    ${TELEMETRY_PATH}/YoghourtTelemetryRing.cpp
    ${TELEMETRY_PATH}/YoghourtAnomalyDetector.cpp
    ${TELEMETRY_PATH}/YoghourtTelemetryEncoder.cpp
    ${TELEMETRY_PATH}/YoghourtTelemetryCollector.cpp
)

add_library(${PROJECT_NAME} STATIC ${TELEMETRY_SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC ${TELEMETRY_PATH})
