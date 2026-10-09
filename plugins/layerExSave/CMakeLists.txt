cmake_minimum_required(VERSION 3.19)
project(layerExSave)

add_library(${PROJECT_NAME} STATIC)

target_sources(${PROJECT_NAME}
    PRIVATE
        Main.cpp
        SaveSnapshot.h
        WindowSaveImage.h
        WindowSaveImage.cpp
        utils.cpp
        savepng.cpp
        savetlg5.cpp
        tlg5/slide.cpp
        LodePNG/lodepng.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(ZLIB REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE krkr2plugin ZLIB::ZLIB)
