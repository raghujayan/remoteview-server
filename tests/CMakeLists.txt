# Test suite for RemoteView server

# Find GTest package
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# Enable testing
enable_testing()

# Test sources
set(TEST_SOURCES
    test_compressor.cpp
    test_tile_cache.cpp
    test_protocol.cpp
    test_main.cpp
)

# Create test executable
add_executable(remoteview_tests ${TEST_SOURCES})

# Link test libraries
target_link_libraries(remoteview_tests
    ${GTEST_BOTH_LIBRARIES}
    pthread
    remoteview_compressor
    remoteview_tile_cache
    remoteview_protocol
    remoteview_config
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ${HUESPACE_LIBRARY}
    dl
)

target_include_directories(remoteview_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    ${GTEST_INCLUDE_DIRS}
)

# Register tests
add_test(NAME RemoteViewTests COMMAND remoteview_tests)

# Add compiler flags for tests
target_compile_options(remoteview_tests PRIVATE
    -Wall -Wextra -Werror
)