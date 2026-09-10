cmake_minimum_required(VERSION 3.16)
project(Tests LANGUAGES CXX)

find_package(Catch2 3 REQUIRED)

include(CTest)
include(Catch)

set(TEST_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}")

add_subdirectory(unit-tests/core/movie)
add_subdirectory(unit-tests/core/visual)
add_subdirectory(unit-tests/core/tjs2)
add_subdirectory(unit-tests/core/telemetry)

add_subdirectory(unit-tests/plugins)

# plugin-tests 的对应物：让质量门可以用一个目标构建全部 core 单元测试。
get_property(YOGHOURT_CORE_TEST_TARGETS GLOBAL PROPERTY YOGHOURT_CORE_TEST_TARGETS)
if(YOGHOURT_CORE_TEST_TARGETS)
    add_custom_target(core-tests DEPENDS ${YOGHOURT_CORE_TEST_TARGETS})
endif()

set(TEST_FILES_PATH ${CMAKE_CURRENT_SOURCE_DIR}/test_files)
configure_file(test_config.h.in test_config.h)
