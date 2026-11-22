# Unit tests for the device core against an in-memory board (fake_hal.h).
#   cmake -S tests/core -B build/core && cmake --build build/core -j && ctest --test-dir build/core --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(rekacore_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/lib/rekacore/src)
file(GLOB CORE_SOURCES ${CORE}/*.cpp)

add_executable(rekacore_tests
  test_core.cpp
  ${CORE_SOURCES}
  ${CORE}/vendor/miniz_build.c
  ${CORE}/vendor/qrcodegen.c)
target_include_directories(rekacore_tests PRIVATE ${CORE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(rekacore_tests PRIVATE REKA_DEFAULT_SERVER="https://deck.example")
if(NOT MSVC)
  target_compile_options(rekacore_tests PRIVATE -Wall -fsanitize=address,undefined)
  target_link_options(rekacore_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME rekacore COMMAND rekacore_tests)
