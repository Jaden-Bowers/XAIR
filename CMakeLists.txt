cmake_minimum_required(VERSION 3.16)

project(xair VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(XAIR_STRICT_WARNINGS "Treat extended compiler warnings as errors" OFF)

function(xair_apply_warnings target)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4)
        if(XAIR_STRICT_WARNINGS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(XAIR_STRICT_WARNINGS)
            target_compile_options(${target} PRIVATE -Wconversion -Wsign-conversion -Werror)
        endif()
    endif()
endfunction()

function(xair_keep_asserts target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endfunction()

add_library(xair STATIC
    src/xair_binary.c
    src/xair.c
    src/xair_frontend.c
    src/xair_x86_decode.c
    src/xair_vex.c
)

target_include_directories(xair
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

xair_apply_warnings(xair)

enable_testing()

add_executable(xair_core_tests
    tests/test_core.c
)

target_link_libraries(xair_core_tests PRIVATE xair)
xair_apply_warnings(xair_core_tests)
xair_keep_asserts(xair_core_tests)

add_test(NAME xair_core_tests COMMAND xair_core_tests)

add_executable(xair_frontend_tests
    tests/test_frontend.c
)

target_link_libraries(xair_frontend_tests PRIVATE xair)
xair_apply_warnings(xair_frontend_tests)
xair_keep_asserts(xair_frontend_tests)

add_test(NAME xair_frontend_tests COMMAND xair_frontend_tests)

add_executable(xair_lift_raw
    tools/xair_lift_raw.c
)

target_link_libraries(xair_lift_raw PRIVATE xair)
xair_apply_warnings(xair_lift_raw)

add_executable(xair_case_run
    tools/xair_case_run.c
)

target_link_libraries(xair_case_run PRIVATE xair)
xair_apply_warnings(xair_case_run)

add_executable(xair_bench_lift
    tools/xair_bench_lift.c
)

target_link_libraries(xair_bench_lift PRIVATE xair)
xair_apply_warnings(xair_bench_lift)

add_executable(xair_lift_bin
    tools/xair_lift_bin.c
)

target_link_libraries(xair_lift_bin PRIVATE xair)
xair_apply_warnings(xair_lift_bin)

add_test(
    NAME xair_case_run_smoke
    COMMAND xair_case_run ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/mov_ret.xaircase
)

add_test(
    NAME xair_lift_bin_smoke
    COMMAND xair_lift_bin --json $<TARGET_FILE:xair_lift_bin>
)
