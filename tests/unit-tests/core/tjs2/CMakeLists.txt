cmake_minimum_required(VERSION 3.16)
project(TestTjs2 LANGUAGES CXX)

set(SOURCES
        tjs.cpp
        tjsString.cpp
)

string(REPLACE ".cpp" "" BASENAMES_SOURCES "${SOURCES}")
set(TARGETS ${BASENAMES_SOURCES})

set(COMMON_SOURCES
        main.cpp
        spatial_stubs.cpp
)

foreach(name ${TARGETS})
    add_executable(${name} ${name}.cpp ${COMMON_SOURCES})
endforeach()

set(ALL_TARGETS
        ${TARGETS}
)

foreach(name ${ALL_TARGETS})
    target_link_libraries(${name}
        PRIVATE
            Catch2::Catch2
        PUBLIC
            krkr2core
    )
    target_include_directories(${name} PRIVATE "${TEST_CONFIG_DIR}")
    catch_discover_tests(${name} PROPERTIES LABELS core)
endforeach()

# core 套件此前既无 label 也无聚合目标，质量门因此既不构建也不运行它们
# （门只 build plugin-tests 并 ctest -L plugin）。两者缺一都会让这些用例
# 静默消失，所以标签和聚合目标必须一起加。
set_property(GLOBAL APPEND PROPERTY YOGHOURT_CORE_TEST_TARGETS ${ALL_TARGETS})
