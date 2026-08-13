cmake_minimum_required(VERSION 3.19)
project(extNagano)

add_library(${PROJECT_NAME} STATIC)

target_sources(${PROJECT_NAME} PRIVATE
    Main.cpp
    3duniversal.cpp
    blurfade.cpp
    scanline.cpp
    zoomfade.cpp
    rgbfade.cpp
    spin.cpp
    flutter.cpp
    book.cpp
    imagewipe.cpp
    honeyturn.cpp
    morphing.cpp
    multiripple.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE krkr2plugin)
