add_library(remoteview_config
    config.cpp
)

target_link_libraries(remoteview_config
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

target_include_directories(remoteview_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)