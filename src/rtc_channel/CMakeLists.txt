# RTC Channel module - WebSocket server and WebRTC DataChannel implementation

add_library(remoteview_rtc_channel
    webrtc_server.cpp
    data_channel.cpp
)

target_link_libraries(remoteview_rtc_channel
    spdlog::spdlog
    remoteview_config
    remoteview_protocol
    websockets
    nlohmann_json::nlohmann_json
)

target_include_directories(remoteview_rtc_channel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)