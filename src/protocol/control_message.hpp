#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <cstdint>

namespace remoteview::protocol {

// JSON Control Messages from client to server as per wire protocol spec

enum class MessageType {
    Hello,
    SetSlice,
    SetView, 
    SetLut,
    Quality,
    Ping,
    Unknown
};

struct ClientCapabilities {
    bool webgpu = false;
    bool webgl2 = false;
    uint32_t max_texture = 0;
};

struct HelloMessage {
    ClientCapabilities client_caps;
};

struct SetSliceMessage {
    uint32_t inline_idx;
    uint32_t xline_idx;
    uint32_t z_idx;
};

struct SetViewMessage {
    std::string plane; // "inline", "xline", "z"
    uint32_t index;
    bool drag = false;
    int vx = 0; // velocity hint for prefetch
};

struct SetLutMessage {
    std::string name; // "SeismicRWB", "Gray", etc.
    float clip_pct = 98.0f;
    float gain = 1.0f;
    uint32_t agc = 0; // AGC window size, 0 = disabled
};

struct QualityPreferences {
    std::string dtype = "u16"; // "u8", "u16", "f32"
    uint32_t downsample = 1;
};

struct QualityMessage {
    QualityPreferences prefer;
};

struct PingMessage {
    uint32_t id;
};

struct ControlMessage {
    MessageType type = MessageType::Unknown;
    
    // Message-specific data
    std::optional<HelloMessage> hello;
    std::optional<SetSliceMessage> set_slice;
    std::optional<SetViewMessage> set_view;
    std::optional<SetLutMessage> set_lut;
    std::optional<QualityMessage> quality;
    std::optional<PingMessage> ping;
    
    // Parsing
    static ControlMessage from_json(const nlohmann::json& j);
    nlohmann::json to_json() const;
};

// Response messages (server to client)

struct PongMessage {
    uint32_t id;
    uint64_t server_time_us; // server timestamp
};

struct ControlResponse {
    enum class Type {
        Pong,
        Error,
        Status
    } type;
    
    std::optional<PongMessage> pong;
    std::optional<std::string> error_msg;
    std::optional<nlohmann::json> status;
    
    nlohmann::json to_json() const;
};

} // namespace remoteview::protocol