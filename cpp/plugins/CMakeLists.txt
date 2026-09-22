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
    layerExImage/layerExImage.cpp
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
    layerExAreaAverage/layerExAreaAverage.cpp
    alphamovie/AlphaMovie.cpp
    wuvorbis/WuVorbisShim.cpp
    windowMessageChannel/windowMessageChannel.cpp
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
add_subdirectory(layerExSave)
add_subdirectory(layerExDraw)
add_subdirectory(layerExBtoA)
add_subdirectory(motionplayer)

#add_subdirectory(json)
add_subdirectory(fstat)
add_subdirectory(packinone)
add_subdirectory(extNagano)
#add_subdirectory(steam)
#add_subdirectory(DrawDeviceForSteam)
# Oracle builds swap the emote implementation: linking both would register two
# Motion classes and the reference would lose the name.
set(_krkr2plugin_children
        psdfile
        psbfile
        layerExSave
        layerExDraw
        layerExBtoA
        fstat
        packinone
        extNagano)
if(NOT YOGHOURT_EMOTE_REF_PLUGIN)
    list(APPEND _krkr2plugin_children motionplayer)
endif()
target_link_libraries(${PROJECT_NAME} PUBLIC
        ${_krkr2plugin_children}
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

# Optional oracle build (YOGHOURT_EMOTE_REF_PLUGIN at the top level). The
# reference snapshot registers Motion under emoteplayer.dll; it is an untracked
# local copy, so this stays off for normal builds and is only enabled in a
# scratch build directory. Defined here so fmt/spdlog usage requirements are
# already available. An object library keeps every translation unit linked, and
# the copy takes its own anchor name because this plugin defines the same symbol
# in the same binary.
if(YOGHOURT_EMOTE_REF_PLUGIN)
    set(_emote_ref_dir "${CMAKE_CURRENT_SOURCE_DIR}/motionplayer/ref")
    if(NOT EXISTS "${_emote_ref_dir}/main.cpp")
        message(FATAL_ERROR "YOGHOURT_EMOTE_REF_PLUGIN=ON but ${_emote_ref_dir}/main.cpp is missing")
    endif()
    file(GLOB EMOTE_REF_SOURCES "${_emote_ref_dir}/*.cpp")
    add_library(emoteplayer-ref OBJECT ${EMOTE_REF_SOURCES})
    # The plugin it replaces is not linked in this configuration, so the
    # reference keeps the anchor name the internal registry calls.
    target_include_directories(emoteplayer-ref PRIVATE ${_emote_ref_dir})
    # Include dirs only: linking psbfile here would close a cycle through the
    # aggregate (psbfile -> krkr2plugin -> emoteplayer-ref). The runtime link
    # line already provides the symbols.
    target_include_directories(emoteplayer-ref PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/xp3filter
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/simplebinder)
    target_link_libraries(emoteplayer-ref PRIVATE krkr2core fmt::fmt spdlog::spdlog)
    target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:emoteplayer-ref>)
    # The snapshot drives desktop-GL entry points such as glClearDepth that the
    # ANGLE/EGL build does not carry; the framework supplies the symbols.
    if(APPLE)
        target_link_libraries(${PROJECT_NAME} PUBLIC "-framework OpenGL")
    endif()
endif()
