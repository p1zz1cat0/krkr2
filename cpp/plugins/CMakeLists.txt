cmake_minimum_required(VERSION 3.19)
project(krkr2plugin)

add_library(${PROJECT_NAME} STATIC)

# The aggregate owns these registration translation units. Keep them private so
# child plugin targets link the aggregate instead of compiling every built-in
# plugin a second time (which can duplicate ncbind registrations).
target_sources(${PROJECT_NAME} PRIVATE
    KAGParser/kagparserex.cpp
    EmotePlayer/EmotePlayer.cpp
    TextRender/TextRender.cpp
    ScriptsEx/scriptsEx.cpp
    win32dialog/win32dialog.cpp
    dirlist/dirlist.cpp
    csvParser/csvParser.cpp
    layerExMovie/layerExMovie.cpp
    varfile/varfile.cpp
    saveStruct/saveStruct.cpp
    getabout/getabout.cpp
    addFont/addFont.cpp
    wutcwf/wutcwf.cpp
    windowEx/windowEx.cpp
    getSample/getSample.cpp
    layerExPerspective/layerExPerspective.cpp
    fftgraph/fftgraph.cpp
    common/LayerExBase.cpp
    Extrans/Extrans.cpp
    Extrans/ExtransMath.cpp
    xp3filter/xp3filter.cpp
)

# These registration translation units belong only to the aggregate plugin
# library. The historical PUBLIC source list is inherited by child plugin
# targets, which would otherwise compile and register them multiple times.
# PluginImpl.cpp references explicit anchors so the static linker retains them.
target_sources(${PROJECT_NAME} PRIVATE
    getLangName/getLangName.cpp
    shrinkCopy/shrinkCopy.cpp
    layerExRaster/layerExRaster.cpp
    alphamovie/AlphaMovie.cpp
    wuvorbis/WuVorbisShim.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${KRKR2CORE_PATH}/
    ${KRKR2CORE_PATH}/base
    ${KRKR2CORE_PATH}/base/win32
    ${KRKR2CORE_PATH}/environ
    ${KRKR2CORE_PATH}/environ/win32
    ${KRKR2CORE_PATH}/environ/android
    ${KRKR2CORE_PATH}/environ/sdl
    ${KRKR2CORE_PATH}/msg
    ${KRKR2CORE_PATH}/msg/win32
    ${KRKR2CORE_PATH}/extension
    ${KRKR2CORE_PATH}/sound
    ${KRKR2CORE_PATH}/sound/win32
    ${KRKR2CORE_PATH}/tjs2
    ${KRKR2CORE_PATH}/utils
    ${KRKR2CORE_PATH}/utils/win32
    ${KRKR2CORE_PATH}/visual
    ${KRKR2CORE_PATH}/visual/ARM
    ${KRKR2CORE_PATH}/visual/win32
    ${KRKR2CORE_PATH}/visual/RenderScript/rs
)

add_definitions(-D__STDC_CONSTANT_MACROS -DTJS_TEXT_OUT_CRLF)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${KRKR2PLUGIN_PATH}
    ${KRKR2PLUGIN_PATH}/common
    ${KRKR2PLUGIN_PATH}/simplebinder
    ${KRKR2PLUGIN_PATH}/xp3filter
)
target_link_libraries(${PROJECT_NAME} PUBLIC krkr2core)

add_subdirectory(psdfile)
add_subdirectory(psbfile)
add_subdirectory(layerExDraw)
add_subdirectory(layerExBtoA)
add_subdirectory(motionplayer)

#add_subdirectory(json)
add_subdirectory(fstat)
add_subdirectory(packinone)
add_subdirectory(extNagano)
#add_subdirectory(steam)
#add_subdirectory(DrawDeviceForSteam)
target_link_libraries(${PROJECT_NAME} PUBLIC
        psdfile
        psbfile
        layerExDraw
        layerExBtoA
        motionplayer
        fstat
        packinone
        extNagano
#        json steam DrawDeviceForSteam
)

find_package(fmt CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)

# AlphaMovie 的 zlib-alpha 路径需要 zlib (来自 vcpkg manifest，静态 triplet)。
# 走 CMake 内建模块模式：vcpkg-cmake-wrapper 会把查找路由到 vcpkg 静态库，
# 并提供 ZLIB::ZLIB 导入目标。
find_package(ZLIB REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
