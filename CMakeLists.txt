cmake_minimum_required(VERSION 3.9)
project(zipdownloader LANGUAGES CXX)

find_package(Qt5 5.14 REQUIRED COMPONENTS Core Network)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

add_library(zipdownloader STATIC
  src/miniz.cpp
  src/miniz.h
  src/zipdownloader.cpp
  src/zipdownloader.hpp
)
target_include_directories(zipdownloader PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(zipdownloader PRIVATE cxx_std_17)

include(GenerateExportHeader)
generate_export_header(zipdownloader)

target_link_libraries(zipdownloader Qt5::Core Qt5::Network)
set(ZIPDOWNLOADER_EXAMPLES 1)
if(ZIPDOWNLOADER_EXAMPLES)
  add_executable(zdl_example examples/example.cpp)
  target_link_libraries(zdl_example PRIVATE zipdownloader)
endif()
