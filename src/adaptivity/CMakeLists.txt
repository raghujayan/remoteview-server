# Adaptivity module - Dynamic quality adjustments

add_library(remoteview_adaptivity
    adapt.cpp
)

target_link_libraries(remoteview_adaptivity
    spdlog::spdlog
    remoteview_config
)

target_include_directories(remoteview_adaptivity PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)