cmake_minimum_required(VERSION 3.19)

project(layerExSave)

add_library(${PROJECT_NAME} STATIC)

target_sources(${PROJECT_NAME} PUBLIC
    LayerExSaveModule.cpp
    LayerExSaveTlg5.cpp
    LayerExSavePng.cpp
    LayerExSaveUtils.cpp
    LayerExSaveAsync.cpp
    thirdparty/lodepng/lodepng.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE krkr2plugin)

# savepng 的 deflate/CRC 与 lodepng custom_zlib 钩子使用 zlib
find_package(ZLIB REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
