add_library(remoteview_protocol
    control_message.cpp
    tile_message.cpp
)

target_link_libraries(remoteview_protocol
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

target_include_directories(remoteview_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)