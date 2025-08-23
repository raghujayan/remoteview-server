add_library(remoteview_tile_cache
    cache.cpp
)

target_link_libraries(remoteview_tile_cache
    spdlog::spdlog
    remoteview_config
    remoteview_vds_access
)

target_include_directories(remoteview_tile_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)