cmake_minimum_required(VERSION 3.20)
project(RemoteViewServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTS "Build test suite" ON)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)

# HueSpace SDK path as specified in project plan
set(HUESPACE_SDK_PATH "/home/rocky/HueSpace-13.3.0-449-gcc8.4.1-redhat-8-x86_64")

# Find required libraries
find_package(PkgConfig REQUIRED)
find_package(spdlog REQUIRED)
find_package(OpenSSL REQUIRED)

# Try to find nlohmann_json, fallback to header-only if not found
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json package not found, using header-only fallback")
    # Create header-only target with our local header
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
endif()

# Try to find httplib (cpp-httplib), fallback to header-only if not found
find_package(httplib QUIET)
if(NOT httplib_FOUND)
    message(STATUS "httplib package not found, using header-only fallback")
    # Create header-only target with our local header
    add_library(httplib INTERFACE)
    add_library(httplib::httplib ALIAS httplib)
    target_include_directories(httplib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
endif()

# HueSpace/OpenVDS includes and libraries
find_path(OPENVDS_INCLUDE_DIR 
    NAMES HueSpace3/ProxyInterfaceFactory.h
    PATHS ${HUESPACE_SDK_PATH}/include
    NO_DEFAULT_PATH
)

find_library(HUESPACE_LIBRARY 
    NAMES hueproxy
    PATHS ${HUESPACE_SDK_PATH}/lib
    NO_DEFAULT_PATH
)

if(NOT OPENVDS_INCLUDE_DIR OR NOT HUESPACE_LIBRARY)
    message(FATAL_ERROR "HueSpace SDK not found at ${HUESPACE_SDK_PATH}")
endif()

message(STATUS "Found HueSpace SDK at ${HUESPACE_SDK_PATH}")
message(STATUS "HueSpace include: ${OPENVDS_INCLUDE_DIR}")  
message(STATUS "HueSpace library: ${HUESPACE_LIBRARY}")

# Compression libraries
find_library(LZ4_LIBRARY lz4)
find_library(ZSTD_LIBRARY zstd)

# WebRTC - using system libwebrtc (optional for now)
pkg_check_modules(WEBRTC libwebrtc)
if(NOT WEBRTC_FOUND)
    message(STATUS "WebRTC not found, building without WebRTC support")
endif()

# Add modular subdirectories as per project plan
add_subdirectory(src/config)
add_subdirectory(src/vds_access)
add_subdirectory(src/tile_cache)
add_subdirectory(src/compressor)
add_subdirectory(src/rtc_channel)
add_subdirectory(src/adaptivity)
add_subdirectory(src/metrics)
add_subdirectory(src/protocol)
add_subdirectory(src/opengl_renderer)

# Main executable
add_executable(remoteview_server
    src/main.cpp
    src/server.cpp
)

target_link_libraries(remoteview_server PRIVATE
    remoteview_config
    remoteview_vds_access
    remoteview_tile_cache
    remoteview_compressor
    remoteview_rtc_channel
    remoteview_adaptivity
    remoteview_metrics
    remoteview_protocol
    remoteview_opengl_renderer
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Link HueSpace library
target_link_libraries(remoteview_server PRIVATE ${HUESPACE_LIBRARY})

if(LZ4_LIBRARY)
    target_link_libraries(remoteview_server PRIVATE ${LZ4_LIBRARY})
endif()

if(ZSTD_LIBRARY)
    target_link_libraries(remoteview_server PRIVATE ${ZSTD_LIBRARY})
endif()

target_include_directories(remoteview_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENVDS_INCLUDE_DIR}
)

# Link WebRTC if available
if(WEBRTC_FOUND)
    target_link_libraries(remoteview_server PRIVATE ${WEBRTC_LIBRARIES})
    target_include_directories(remoteview_server PRIVATE ${WEBRTC_INCLUDE_DIRS})
    target_compile_options(remoteview_server PRIVATE ${WEBRTC_CFLAGS_OTHER})
endif()

# Compiler-specific optimizations and warnings
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(remoteview_server PRIVATE 
        -Wall -Wextra -Werror -O3 -march=native
        -ffast-math -funroll-loops
        # Suppress warnings from third-party code (HueSpace SDK)
        -Wno-deprecated-declarations
        -Wno-strict-aliasing
        -Wno-reorder  
        -Wno-unused-parameter
        -Wno-unused-variable
    )
endif()

# Apply same warning flags to all our libraries
function(apply_warning_flags target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Werror
            -Wno-deprecated-declarations
            -Wno-strict-aliasing
            -Wno-reorder
            -Wno-unused-parameter
            -Wno-unused-variable
        )
    endif()
endfunction()

# Install rules
install(TARGETS remoteview_server
    RUNTIME DESTINATION bin
)

# Testing with extensive test suite as per plan
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_subdirectory(tests)
    else()
        message(STATUS "GTest not found, skipping tests")
    endif()
endif()

# Benchmarks
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()