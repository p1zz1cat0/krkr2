cmake_minimum_required(VERSION 3.16)
project(TestPlugins LANGUAGES CXX)

set(SOURCES
        getLangName-dll.cpp
        psbfile-dll.cpp
        shrinkCopy-dll.cpp
        layerExImage-dll.cpp
        layerExAreaAverage-dll.cpp
        layerExSave-dll.cpp
        extrans-dll.cpp
        alphamovie-dll.cpp
        extNagano-dll.cpp
        plugin-safety-dll.cpp
        motionplayer-physics.cpp
        motionplayer-routed.cpp
        motionplayer-param.cpp
        motionplayer-source-dialect.cpp
        lzfs-dll.cpp
        json-dll.cpp
        multiimage-dll.cpp
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
            ${KRKR2PLUGIN_PATH}/motionplayer/MotionPhysics.cpp
            ${KRKR2PLUGIN_PATH}/motionplayer/RoutedTransition.cpp)
    elseif(name STREQUAL "motionplayer-routed")
        add_executable(${name}
            ${name}.cpp
            ${KRKR2PLUGIN_PATH}/motionplayer/RoutedTransition.cpp)
    else()
        add_executable(${name} ${name}.cpp ${COMMON_SOURCES})
    endif()
endforeach()

set(ALL_TARGETS
        ${TARGETS}
)

foreach(name ${ALL_TARGETS})
    if(name STREQUAL "motionplayer-physics" OR name STREQUAL "motionplayer-routed")
        target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain)
    else()
        target_link_libraries(${name}
            PRIVATE
                Catch2::Catch2
            PUBLIC
                krkr2plugin krkr2core
        )
        # layerExSave-dll 的 PNG 编码器直接使用 zlib（deflate/CRC/uncompress）
        find_package(ZLIB REQUIRED)
        target_link_libraries(${name} PRIVATE ZLIB::ZLIB)
        # lzfs-dll 用官方 LZ4F 生成样本并调用插件的解码核心。
        if(name STREQUAL "lzfs-dll")
            find_package(lz4 CONFIG REQUIRED)
            target_link_libraries(${name} PRIVATE lz4::lz4)
        endif()
    endif()
    target_include_directories(${name}
        PRIVATE
            "${TEST_CONFIG_DIR}"
            "${KRKR2PLUGIN_PATH}"
    )
    catch_discover_tests(${name} PROPERTIES LABELS plugin)
endforeach()

add_custom_target(plugin-tests DEPENDS ${ALL_TARGETS})
