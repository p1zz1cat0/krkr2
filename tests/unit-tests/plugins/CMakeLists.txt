cmake_minimum_required(VERSION 3.16)
project(TestPlugins LANGUAGES CXX)

set(SOURCES
        getLangName-dll.cpp
        psbfile-dll.cpp
        shrinkCopy-dll.cpp
        extrans-dll.cpp
        alphamovie-dll.cpp
        extNagano-dll.cpp
        plugin-safety-dll.cpp
)

string(REPLACE ".cpp" "" BASENAMES_SOURCES "${SOURCES}")
set(TARGETS ${BASENAMES_SOURCES})

set(COMMON_SOURCES
        main.cpp
        spatial_stubs.cpp
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
            krkr2plugin krkr2core
    )
    target_include_directories(${name}
        PRIVATE
            "${TEST_CONFIG_DIR}"
            "${KRKR2PLUGIN_PATH}"
    )
    catch_discover_tests(${name} PROPERTIES LABELS plugin)
endforeach()

add_custom_target(plugin-tests DEPENDS ${ALL_TARGETS})
