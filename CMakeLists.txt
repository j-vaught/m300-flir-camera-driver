cmake_minimum_required(VERSION 3.10)
project(CameraDriver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -pthread")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)

# Include directories
include_directories(include)

# Camera driver library
add_library(camera_driver STATIC
    src/CameraFrameCapture.cpp
)

target_link_libraries(camera_driver
    PRIVATE PkgConfig::FFMPEG PkgConfig::JPEG pthread
)

# Test executable
add_executable(camera_test src/main.cpp)
target_link_libraries(camera_test PRIVATE camera_driver)

# Installation
install(TARGETS camera_driver DESTINATION lib)
install(FILES include/CameraFrameCapture.hpp DESTINATION include)
