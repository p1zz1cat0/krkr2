cmake_minimum_required(VERSION 3.16)
project(TestPlugins LANGUAGES CXX)

set(SOURCES
        getLangName-dll.cpp
        psbfile-dll.cpp
        shrinkCopy-dll.cpp
        layerExImage-dll.cpp
        extrans-dll.cpp
        alphamovie-dll.cpp
        extNagano-dll.cpp
        plugin-safety-dll.cpp
        motionplayer-physics.cpp
        motionplayer-param.cpp
)

string(REPLACE ".cpp" "" BASENAMES_SOURCES "${SOURCES}")
set(TARGETS ${BASENAMES_SOURCES})

set(COMMON_SOURCES
        main.cpp
        spatial_stubs.cpp
)

foreach(name ${TARGETS})
    if(name STREQUAL "motionplayer-physics")
        add_executable(${name}
            ${name}.cpp
            ${KRKR2PLUGIN_PATH}/motionplayer/MotionPhysics.cpp)
    else()
        add_executable(${name} ${name}.cpp ${COMMON_SOURCES})
    endif()
endforeach()

set(ALL_TARGETS
        ${TARGETS}
)

foreach(name ${ALL_TARGETS})
    if(name STREQUAL "motionplayer-physics")
        target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain)
    else()
        target_link_libraries(${name}
            PRIVATE
                Catch2::Catch2
            PUBLIC
                krkr2plugin krkr2core
        )
    endif()
    target_include_directories(${name}
        PRIVATE
            "${TEST_CONFIG_DIR}"
            "${KRKR2PLUGIN_PATH}"
    )
    catch_discover_tests(${name} PROPERTIES LABELS plugin)
endforeach()

add_custom_target(plugin-tests DEPENDS ${ALL_TARGETS})
