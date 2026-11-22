cmake_minimum_required(VERSION 3.16)
project(rekadeck_sim C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/lib/rekacore/src)
file(GLOB CORE_SOURCES ${CORE}/*.cpp)

add_executable(rekadeck_sim
  main.cpp
  hal_desktop.cpp
  ${CORE_SOURCES}
  ${CORE}/vendor/miniz_build.c
  ${CORE}/vendor/qrcodegen.c)

target_include_directories(rekadeck_sim PRIVATE ${CORE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(rekadeck_sim PRIVATE
  SIM_UI_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ui.html"
  REKA_DEFAULT_SERVER="http://127.0.0.1:8000")
find_package(Threads REQUIRED)
target_link_libraries(rekadeck_sim PRIVATE Threads::Threads)
