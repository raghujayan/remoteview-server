# Metrics module - Performance monitoring with JSON endpoint

add_library(remoteview_metrics
    metrics_collector.cpp
)

target_link_libraries(remoteview_metrics
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    remoteview_config
)

target_include_directories(remoteview_metrics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)