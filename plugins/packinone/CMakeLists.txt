cmake_minimum_required(VERSION 3.19)
project(packinone)

add_library(${PROJECT_NAME} STATIC)

target_sources(${PROJECT_NAME} PRIVATE
    packinone_register.cpp
    storage_fstat.cpp
    temporary_files.cpp
    lz4_stream.cpp
    scripts_add.cpp
    layerExImage.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE krkr2plugin)
