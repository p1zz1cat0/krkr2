cmake_minimum_required(VERSION 3.19)
project(core_msg LANGUAGES CXX)

set(MSG_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BASE_PATH ${CMAKE_SOURCE_DIR}/cpp/core/base)
set(UTILS_PATH ${CMAKE_SOURCE_DIR}/cpp/core/utils)

set(CORE_MSG_SOURCE_FILES
    ${BASE_PATH}/MsgIntf.cpp
    ${BASE_PATH}/impl/MsgImpl.cpp
)

add_library(${PROJECT_NAME} STATIC ${CORE_MSG_SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC
    ${BASE_PATH}
    ${BASE_PATH}/impl
)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${UTILS_PATH}
)
target_link_libraries(${PROJECT_NAME} PUBLIC tjs2)
