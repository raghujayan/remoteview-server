add_library(remoteview_compressor
    compressor.cpp
)

target_link_libraries(remoteview_compressor
    ${LZ4_LIBRARY}
    ${ZSTD_LIBRARY}
    spdlog::spdlog
    remoteview_config
)

target_include_directories(remoteview_compressor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)