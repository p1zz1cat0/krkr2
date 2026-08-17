cmake_minimum_required(VERSION 3.19)
project(layerExDraw LANGUAGES CXX C)

add_library(${PROJECT_NAME} STATIC)
target_link_libraries(${PROJECT_NAME} PRIVATE krkr2plugin)

if(WINDOWS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE -DUNICODE -D_UNICODE)
    target_sources(${PROJECT_NAME} PUBLIC
            windows/main.cpp
            windows/LayerExDraw.cpp
    )
else()
    find_package(blend2d CONFIG REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE blend2d::blend2d)
    target_sources(${PROJECT_NAME} PUBLIC blend2d/LayerExDraw.cpp)
endif ()
