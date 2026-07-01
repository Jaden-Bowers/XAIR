cmake_minimum_required(VERSION 3.16)

project(xair VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(xair STATIC
    src/xair.c
)

target_include_directories(xair
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(xair PRIVATE /W4)
else()
    target_compile_options(xair PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

add_executable(xair_core_tests
    tests/test_core.c
)

target_link_libraries(xair_core_tests PRIVATE xair)

add_test(NAME xair_core_tests COMMAND xair_core_tests)

