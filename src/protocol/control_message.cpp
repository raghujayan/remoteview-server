#include "control_message.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

namespace remoteview::protocol {

ControlMessage ControlMessage::from_json(const nlohmann::json& j) {
    ControlMessage msg;
    
    if (!j.contains("t")) {
        spdlog::warn("Control message missing type field");
        return msg; // type remains Unknown
    }
    
    std::string type_str = j["t"].get<std::string>();
    
    if (type_str == "hello") {
        msg.type = MessageType::Hello;
        HelloMessage hello_msg;
        
        if (j.contains("clientCaps")) {
            auto& caps = j["clientCaps"];
            hello_msg.client_caps.webgpu = caps.value("webgpu", false);
            hello_msg.client_caps.webgl2 = caps.value("webgl2", false);
            hello_msg.client_caps.max_texture = caps.value("maxTexture", 0u);
        }
        
        msg.hello = hello_msg;
        
    } else if (type_str == "set_slice") {
        msg.type = MessageType::SetSlice;
        SetSliceMessage slice_msg;
        
        slice_msg.inline_idx = j.value("inline", 0u);
        slice_msg.xline_idx = j.value("xline", 0u);
        slice_msg.z_idx = j.value("z", 0u);
        
        msg.set_slice = slice_msg;
        
    } else if (type_str == "set_view") {
        msg.type = MessageType::SetView;
        SetViewMessage view_msg;
        
        view_msg.plane = j.value("plane", std::string());
        view_msg.index = j.value("index", 0u);
        view_msg.drag = j.value("drag", false);
        view_msg.vx = j.value("vx", 0);
        
        msg.set_view = view_msg;
        
    } else if (type_str == "set_lut") {
        msg.type = MessageType::SetLut;
        SetLutMessage lut_msg;
        
        lut_msg.name = j.value("name", std::string());
        lut_msg.clip_pct = j.value("clipPct", 98.0f);
        lut_msg.gain = j.value("gain", 1.0f);
        lut_msg.agc = j.value("agc", 0u);
        
        msg.set_lut = lut_msg;
        
    } else if (type_str == "quality") {
        msg.type = MessageType::Quality;
        QualityMessage quality_msg;
        
        if (j.contains("prefer")) {
            auto& prefer = j["prefer"];
            quality_msg.prefer.dtype = prefer.value("dtype", std::string("u16"));
            quality_msg.prefer.downsample = prefer.value("downsample", 1u);
        }
        
        msg.quality = quality_msg;
        
    } else if (type_str == "ping") {
        msg.type = MessageType::Ping;
        PingMessage ping_msg;
        
        ping_msg.id = j.value("id", 0u);
        
        msg.ping = ping_msg;
        
    } else {
        spdlog::warn("Unknown control message type: {}", type_str);
        msg.type = MessageType::Unknown;
    }
    
    return msg;
}

nlohmann::json ControlMessage::to_json() const {
    nlohmann::json j;
    
    switch (type) {
        case MessageType::Hello:
            j["t"] = "hello";
            if (hello) {
                j["clientCaps"] = {
                    {"webgpu", hello->client_caps.webgpu},
                    {"webgl2", hello->client_caps.webgl2},
                    {"maxTexture", hello->client_caps.max_texture}
                };
            }
            break;
            
        case MessageType::SetSlice:
            j["t"] = "set_slice";
            if (set_slice) {
                j["inline"] = set_slice->inline_idx;
                j["xline"] = set_slice->xline_idx;
                j["z"] = set_slice->z_idx;
            }
            break;
            
        case MessageType::SetView:
            j["t"] = "set_view";
            if (set_view) {
                j["plane"] = set_view->plane;
                j["index"] = set_view->index;
                j["drag"] = set_view->drag;
                j["vx"] = set_view->vx;
            }
            break;
            
        case MessageType::SetLut:
            j["t"] = "set_lut";
            if (set_lut) {
                j["name"] = set_lut->name;
                j["clipPct"] = set_lut->clip_pct;
                j["gain"] = set_lut->gain;
                j["agc"] = set_lut->agc;
            }
            break;
            
        case MessageType::Quality:
            j["t"] = "quality";
            if (quality) {
                j["prefer"] = {
                    {"dtype", quality->prefer.dtype},
                    {"downsample", quality->prefer.downsample}
                };
            }
            break;
            
        case MessageType::Ping:
            j["t"] = "ping";
            if (ping) {
                j["id"] = ping->id;
            }
            break;
            
        case MessageType::Unknown:
        default:
            j["t"] = "unknown";
            break;
    }
    
    return j;
}

nlohmann::json ControlResponse::to_json() const {
    nlohmann::json j;
    
    switch (type) {
        case Type::Pong:
            j["t"] = "pong";
            if (pong) {
                j["id"] = pong->id;
                j["serverTime"] = pong->server_time_us;
            }
            break;
            
        case Type::Error:
            j["t"] = "error";
            if (error_msg) {
                j["message"] = *error_msg;
            }
            break;
            
        case Type::Status:
            j["t"] = "status";
            if (status) {
                j["data"] = *status;
            }
            break;
    }
    
    return j;
}

} // namespace remoteview::protocol