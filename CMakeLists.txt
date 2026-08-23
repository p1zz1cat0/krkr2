cmake_minimum_required(VERSION 3.28)

find_program(CCACHE_PROGRAM ccache)

if (CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Ccache found: ${CCACHE_PROGRAM}")
endif ()

add_compile_definitions(
        $<$<CONFIG:Debug>:DEBUG>
        $<$<CONFIG:Debug>:_DEBUG>   # 对于 MSVC 兼容性
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

if(ANDROID)
    set(VCPKG_TARGET_ANDROID ON)
    set(ENV{ANDROID_NDK_HOME} "$ENV{ANDROID_NDK}")
    add_compile_options(-Wno-inconsistent-missing-override)
    include(cmake/vcpkg_android.cmake)
else()
    set(CMAKE_TOOLCHAIN_FILE $ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake)
endif()

set(APP_NAME krkr2)

project(${APP_NAME})

option(ENABLE_TESTS "enable tests execute build(exclude android)" ON)
option(BUILD_TOOLS "build tools execute build(exclude android ios)" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(KRKR2CORE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cpp/core)
set(KRKR2PLUGIN_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cpp/plugins)

if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHsc>" "/MP" "/utf-8")
    add_link_options("/ignore:4099" "/INCREMENTAL" "/DEBUG:FASTLINK")
    # set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(ANDROID)
    add_library(${PROJECT_NAME} SHARED ${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/cpp/krkr2_android.cpp)
    find_package(unofficial-breakpad CONFIG REQUIRED)
    target_link_libraries(${PROJECT_NAME} PUBLIC unofficial::breakpad::libbreakpad_client)
elseif(LINUX)
    add_executable(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/platforms/linux/main.cpp)
elseif(WINDOWS)
    add_executable(${PROJECT_NAME}
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows/game.rc
    )
elseif(IOS)
#        list(APPEND GAME_HEADER
#                proj.ios_mac/ios/AppController.h
#                proj.ios_mac/ios/RootViewController.h
#        )
#        set(APP_UI_RES
#                proj.ios_mac/ios/LaunchScreen.storyboard
#                proj.ios_mac/ios/LaunchScreenBackground.png
#                proj.ios_mac/ios/Images.xcassets
#        )
#        list(APPEND GAME_SOURCE
#                proj.ios_mac/ios/main.m
#                proj.ios_mac/ios/AppController.mm
#                proj.ios_mac/ios/RootViewController.mm
#                proj.ios_mac/ios/Prefix.pch
#                ${APP_UI_RES}
#        )
elseif(MACOS)
    enable_language(OBJCXX)
    if(NOT YOGHOURT_SPATIAL_PRESENTER_DIR)
        message(FATAL_ERROR "YOGHOURT_SPATIAL_PRESENTER_DIR is required for the macOS KrKr2 build")
    endif()
    if(NOT YOGHOURT_SURFACE_RELAY_DIR)
        message(FATAL_ERROR "YOGHOURT_SURFACE_RELAY_DIR is required for the macOS KrKr2 build")
    endif()
    set(YOGHOURT_SPATIAL_PRESENTER_SOURCE
            ${YOGHOURT_SPATIAL_PRESENTER_DIR}/YoghourtSpatialPresenter.mm
            ${YOGHOURT_SPATIAL_PRESENTER_DIR}/YoghourtSpatialPresenter.h
            ${YOGHOURT_SPATIAL_PRESENTER_DIR}/cunny/cunny_metal.h
    )
    set(YOGHOURT_SURFACE_RELAY_SOURCE
            ${YOGHOURT_SURFACE_RELAY_DIR}/IOSurfaceRing.h
            ${YOGHOURT_SURFACE_RELAY_DIR}/IOSurfaceRing.mm
            ${YOGHOURT_SURFACE_RELAY_DIR}/IOSurfaceRingComponentTest.h
            ${YOGHOURT_SURFACE_RELAY_DIR}/IOSurfaceRingComponentTest.mm
    )
    set(APP_UI_RES
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/Icon.icns
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/Info.plist
    )
    add_executable(${PROJECT_NAME}
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/main.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/RendererSelfTest.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/RendererSelfTest.h
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/YoghourtSpatialAdapter.h
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/YoghourtSpatialAdapter.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/YoghourtPresentation.mm
            ${YOGHOURT_SPATIAL_PRESENTER_SOURCE}
            ${YOGHOURT_SURFACE_RELAY_SOURCE}
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/Prefix.pch
            ${APP_UI_RES}
    )
endif()

target_include_directories(${PROJECT_NAME} PUBLIC ${KRKR2CORE_PATH}/environ/cocos2d)
if(MACOS)
    target_include_directories(${PROJECT_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos
            ${YOGHOURT_SPATIAL_PRESENTER_DIR}
            ${YOGHOURT_SURFACE_RELAY_DIR}
    )
    set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/RendererSelfTest.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/YoghourtSpatialAdapter.mm
            ${YOGHOURT_SPATIAL_PRESENTER_DIR}/YoghourtSpatialPresenter.mm
            ${YOGHOURT_SURFACE_RELAY_DIR}/IOSurfaceRing.mm
            ${YOGHOURT_SURFACE_RELAY_DIR}/IOSurfaceRingComponentTest.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# external lib
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/cpp/external)

# build library
add_subdirectory(${KRKR2CORE_PATH})
add_subdirectory(${KRKR2PLUGIN_PATH})

if(MACOS)
    target_include_directories(core_environ_module PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos
    )
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC
    krkr2plugin krkr2core
)
if(MACOS)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(METALFX_FRAMEWORK MetalFX REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
            ${APPKIT_FRAMEWORK}
            ${IOSURFACE_FRAMEWORK}
            ${METAL_FRAMEWORK}
            ${METALFX_FRAMEWORK}
            ${QUARTZCORE_FRAMEWORK}
    )
endif()

## resources
if(NOT ANDROID)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CocosBuildHelpers.cmake")

    set(GAME_RES_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}/ui/cocos-studio")

    if(WINDOWS OR APPLE)
        cocos_mark_multi_resources(common_res_files RES_TO "Resources" FOLDERS ${GAME_RES_FOLDER})
    endif()

    # mark app resources
    setup_cocos_app_config(${APP_NAME})

    if(APPLE)
        set_target_properties(${APP_NAME} PROPERTIES RESOURCE "${APP_UI_RES}")
        target_sources(${APP_NAME} PRIVATE ${common_res_files})
        if(MACOSX)
            set_target_properties(${APP_NAME} PROPERTIES
                    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/apple/macos/Info.plist"
            )
        elseif(IOS)
#            cocos_pak_xcode(${APP_NAME} INFO_PLIST "iOSBundleInfo.plist.in")
#            set_xcode_property(${APP_NAME} ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
#            set_xcode_property(${APP_NAME} DEVELOPMENT_TEAM "")
#            set_xcode_property(${APP_NAME} CODE_SIGN_IDENTITY "iPhone Developer")
        endif()
    elseif(WINDOWS)
        target_sources(${PROJECT_NAME} PRIVATE ${common_res_files})
        cocos_copy_target_dll(${APP_NAME})
    endif()

    if(LINUX OR WINDOWS)
        cocos_get_resource_path(APP_RES_DIR ${APP_NAME})
        cocos_copy_target_res(${APP_NAME} LINK_TO ${APP_RES_DIR} FOLDERS ${GAME_RES_FOLDER})
    endif()
endif()

if(ENABLE_TESTS AND NOT (IOS OR ANDROID))
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_TOOLS AND NOT (IOS OR ANDROID))
    add_subdirectory(tools)
endif()
