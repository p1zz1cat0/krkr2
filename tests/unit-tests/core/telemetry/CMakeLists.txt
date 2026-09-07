cmake_minimum_required(VERSION 3.16)
project(TestTelemetryModule LANGUAGES CXX)

set(SOURCES
        telemetry-queue.cpp
        telemetry-ring.cpp
        telemetry-anomaly.cpp
        telemetry-encoder.cpp
        telemetry-collector.cpp
)

string(REPLACE ".cpp" "" BASENAMES_SOURCES "${SOURCES}")
set(TARGETS ${BASENAMES_SOURCES})

set(COMMON_SOURCES
        main.cpp
)

foreach(name ${TARGETS})
    add_executable(${name} ${name}.cpp ${COMMON_SOURCES})
endforeach()

set(ALL_TARGETS
        ${TARGETS}
)

foreach(name ${ALL_TARGETS})
    target_link_libraries(${name}
        PRIVATE
            Catch2::Catch2
        PUBLIC
            core_telemetry_module
    )
    target_include_directories(${name} PRIVATE "${TEST_CONFIG_DIR}")
    catch_discover_tests(${name} TEST_PREFIX "telemetry-")
endforeach()
