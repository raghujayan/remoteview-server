#include "webrtc_server.hpp"
#include <spdlog/spdlog.h>
#include <random>
#include <sstream>
#include <iomanip>

namespace remoteview {

// Static instance for callback access
WebRtcServer* WebRtcServer::instance_ = nullptr;

// libwebsockets protocol definition
static struct lws_protocols protocols[] = {
    {
        "remoteview-protocol", // protocol name
        WebRtcServer::websocket_callback,
        sizeof(void*), // per session data size
        4096, // rx buffer size
        0, // id
        nullptr, // user pointer
        4096 // tx packet size
    },
    { NULL, NULL, 0, 0 } // terminator
};

WebRtcServer::WebRtcServer(std::shared_ptr<Config> config)
    : config_(std::move(config)) {
    instance_ = this; // Set singleton instance
}

WebRtcServer::~WebRtcServer() {
    stop();
    instance_ = nullptr;
}

void WebRtcServer::start() {
    if (running_.load()) {
        spdlog::warn("WebRTC server is already running");
        return;
    }
    
    spdlog::info("Starting WebSocket server for WebRTC signaling");
    
    try {
        setup_websocket_server();
        running_.store(true);
        
        // Start server thread
        server_thread_ = std::make_unique<std::thread>(&WebRtcServer::server_loop, this);
        
        spdlog::info("WebSocket server started on port {}", config_->get_server_port());
    } catch (const std::exception& e) {
        spdlog::error("Failed to start WebSocket server: {}", e.what());
        cleanup_websocket_server();
        throw;
    }
}

void WebRtcServer::stop() {
    if (!running_.load()) {
        return;
    }
    
    spdlog::info("Stopping WebSocket server");
    running_.store(false);
    
    // Wait for server thread to finish
    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }
    
    cleanup_websocket_server();
    
    // Clear sessions
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }
    
    spdlog::info("WebSocket server stopped");
}

void WebRtcServer::setup_websocket_server() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    
    info.port = config_->get_server_port();
    info.iface = config_->get_bind_address().c_str(); // Bind to configured address (0.0.0.0)
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
    
    ws_context_ = lws_create_context(&info);
    if (!ws_context_) {
        throw std::runtime_error("Failed to create libwebsockets context");
    }
}

void WebRtcServer::cleanup_websocket_server() {
    if (ws_context_) {
        lws_context_destroy(ws_context_);
        ws_context_ = nullptr;
    }
}

void WebRtcServer::server_loop() {
    while (running_.load()) {
        lws_service(ws_context_, 50); // 50ms timeout
    }
}

std::string WebRtcServer::create_session(struct lws* wsi) {
    // Generate session ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    for (int i = 0; i < 32; ++i) {
        ss << std::hex << dis(gen);
    }
    std::string session_id = ss.str();
    
    auto session = std::make_unique<WebSocketSession>();
    session->wsi = wsi;
    session->session_id = session_id;
    session->last_ping = std::chrono::steady_clock::now();
    
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[session_id] = std::move(session);
    }
    
    // Store session ID in user data
    void** user_data = (void**)lws_wsi_user(wsi);
    *user_data = new std::string(session_id);
    
    spdlog::info("Created session {}", session_id);
    return session_id;
}

void WebRtcServer::remove_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session_id);
    spdlog::info("Removed session {}", session_id);
}

WebSocketSession* WebRtcServer::get_session(struct lws* wsi) {
    void** user_data = (void**)lws_wsi_user(wsi);
    if (!user_data || !*user_data) {
        return nullptr;
    }
    
    std::string* session_id = static_cast<std::string*>(*user_data);
    
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(*session_id);
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

int WebRtcServer::websocket_callback(struct lws* wsi, enum lws_callback_reasons reason,
                                   void* user, void* in, size_t len) {
    if (!instance_) {
        return -1;
    }
    
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
        {
            std::string session_id = instance_->create_session(wsi);
            spdlog::info("WebSocket connection established for session {}", session_id);
            
            // Send initial handshake
            nlohmann::json handshake = {
                {"type", "handshake"},
                {"session_id", session_id},
                {"server_version", "1.0.0"},
                {"protocols", {"binary-tiles", "json-control"}}
            };
            instance_->send_control_response(session_id, handshake);
            break;
        }
        
        case LWS_CALLBACK_RECEIVE:
        {
            WebSocketSession* session = instance_->get_session(wsi);
            if (!session) {
                spdlog::warn("Received data for unknown session");
                return -1;
            }
            
            try {
                std::string message(static_cast<char*>(in), len);
                nlohmann::json json_msg = nlohmann::json::parse(message);
                instance_->handle_control_message(session->session_id, json_msg);
            } catch (const std::exception& e) {
                spdlog::error("Failed to parse JSON message: {}", e.what());
                return -1;
            }
            break;
        }
        
        case LWS_CALLBACK_CLOSED:
        {
            void** user_data = (void**)lws_wsi_user(wsi);
            if (user_data && *user_data) {
                std::string* session_id = static_cast<std::string*>(*user_data);
                spdlog::info("WebSocket connection closed for session {}", *session_id);
                instance_->remove_session(*session_id);
                delete session_id;
                *user_data = nullptr;
            }
            break;
        }
        
        default:
            break;
    }
    
    return 0;
}

void WebRtcServer::handle_control_message(const std::string& session_id, const nlohmann::json& msg) {
    try {
        // Handle both "type" and "t" fields for message type
        std::string type;
        if (msg.contains("type")) {
            type = msg.at("type");
        } else if (msg.contains("t")) {
            type = msg.at("t");
        } else {
            spdlog::warn("Message missing type field: {}", msg.dump());
            return;
        }
        
        if (type == "ping") {
            handle_ping(session_id, msg);
        } else if (type == "handshake_response") {
            handle_handshake(session_id, msg);
        } else if (type == "tile_request") {
            handle_tile_request(session_id, msg);
        } else if (type == "set_slice") {
            handle_set_slice(session_id, msg);
        } else if (type == "set_view") {
            handle_set_view(session_id, msg);
        } else if (type == "set_lut") {
            handle_set_lut(session_id, msg);
        } else if (type == "offer") {
            handle_webrtc_offer(session_id, msg);
        } else if (type == "answer") {
            handle_webrtc_answer(session_id, msg);
        } else if (type == "ice-candidate") {
            handle_ice_candidate(session_id, msg);
        } else {
            spdlog::warn("Unknown message type: {}", type);
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to handle control message: {}", e.what());
    }
}

void WebRtcServer::handle_handshake(const std::string& session_id, const nlohmann::json& msg) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->authenticated = true;
        it->second->datachannel_ready = true; // Simulate DataChannel ready
        
        nlohmann::json response = {
            {"type", "handshake_complete"},
            {"session_id", session_id},
            {"datachannel_ready", true}
        };
        send_control_response(session_id, response);
        
        spdlog::info("Handshake completed for session {}", session_id);
    }
}

void WebRtcServer::handle_ping(const std::string& session_id, const nlohmann::json& msg) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->last_ping = std::chrono::steady_clock::now();
        
        nlohmann::json pong = {
            {"type", "pong"},
            {"timestamp", msg.value("timestamp", 0)}
        };
        send_control_response(session_id, pong);
    }
}

void WebRtcServer::handle_tile_request(const std::string& session_id, const nlohmann::json& msg) {
    // This will integrate with VdsReader later
    spdlog::info("Received tile request from session {}: {}", session_id, msg.dump());
    
    // For now, send an acknowledgment
    nlohmann::json ack = {
        {"type", "tile_request_ack"},
        {"request_id", msg.value("request_id", "")}
    };
    send_control_response(session_id, ack);
}

void WebRtcServer::send_control_response(const std::string& session_id, const nlohmann::json& response) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        spdlog::warn("Cannot send response to unknown session {}", session_id);
        return;
    }
    
    std::string json_str = response.dump();
    
    // Prepare LWS write
    size_t msg_len = json_str.length();
    std::vector<unsigned char> buf(LWS_PRE + msg_len);
    memcpy(buf.data() + LWS_PRE, json_str.c_str(), msg_len);
    
    int result = lws_write(it->second->wsi, buf.data() + LWS_PRE, msg_len, LWS_WRITE_TEXT);
    if (result < 0) {
        spdlog::error("Failed to send WebSocket message to session {}", session_id);
    }
}

bool WebRtcServer::send_tile_data(const std::string& session_id, const std::vector<uint8_t>& tile_data) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }
    
    // For WebSocket fallback, we don't require datachannel_ready
    // In production, this would check DataChannel state
    
    // For now, send via WebSocket binary frame (will be replaced with actual DataChannel)
    std::vector<unsigned char> buf(LWS_PRE + tile_data.size());
    memcpy(buf.data() + LWS_PRE, tile_data.data(), tile_data.size());
    
    int result = lws_write(it->second->wsi, buf.data() + LWS_PRE, tile_data.size(), LWS_WRITE_BINARY);
    if (result < 0) {
        spdlog::error("Failed to send tile data to session {}", session_id);
        return false;
    }
    
    return true;
}

void WebRtcServer::handle_webrtc_offer(const std::string& session_id, const nlohmann::json& msg) {
    spdlog::info("Received WebRTC offer from session {}", session_id);
    
    // Instead of trying to implement WebRTC, acknowledge that we'll use WebSocket for control
    nlohmann::json websocket_fallback = {
        {"type", "websocket_fallback"},
        {"message", "Using WebSocket for control messages, DataChannel not available"}
    };
    
    send_control_response(session_id, websocket_fallback);
    
    // Mark session as ready for WebSocket-based control
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->datachannel_ready = false; // Explicitly no DataChannel
        spdlog::info("Session {} configured for WebSocket control messages", session_id);
    }
}

void WebRtcServer::handle_webrtc_answer(const std::string& session_id, const nlohmann::json& msg) {
    spdlog::info("Received WebRTC answer from session {}", session_id);
    // Answer received from client (not expected in our setup, but handled for completeness)
}

void WebRtcServer::handle_ice_candidate(const std::string& session_id, const nlohmann::json& msg) {
    spdlog::debug("Received ICE candidate from session {}", session_id);
    // ICE candidates would be handled by WebRTC library
    // For now, just acknowledge receipt
}

void WebRtcServer::handle_set_slice(const std::string& session_id, const nlohmann::json& msg) {
    uint32_t inline_idx = msg.value("inline", 0);
    uint32_t xline_idx = msg.value("xline", 0);
    uint32_t z_idx = msg.value("z", 0);
    
    spdlog::info("Received set_slice from session {}: inline={}, xline={}, z={}", 
                 session_id, inline_idx, xline_idx, z_idx);
    
    // Generate tiles for the new slice
    generate_tiles_for_slice(session_id, inline_idx, xline_idx, z_idx);
    
    // Acknowledge slice change
    nlohmann::json ack = {
        {"type", "slice_update_ack"},
        {"inline", inline_idx},
        {"xline", xline_idx},
        {"z", z_idx}
    };
    send_control_response(session_id, ack);
}

void WebRtcServer::handle_set_view(const std::string& session_id, const nlohmann::json& msg) {
    spdlog::info("Received set_view from session {}: plane={}, index={}, drag={}", 
                 session_id,
                 msg.value("plane", "unknown"),
                 msg.value("index", 0),
                 msg.value("drag", false));
    
    // TODO: Update view parameters and trigger tile generation
    // For now, just acknowledge
    nlohmann::json ack = {
        {"type", "view_update_ack"},
        {"plane", msg.value("plane", "unknown")},
        {"index", msg.value("index", 0)}
    };
    send_control_response(session_id, ack);
}

void WebRtcServer::handle_set_lut(const std::string& session_id, const nlohmann::json& msg) {
    spdlog::info("Received set_lut from session {}: name={}, clipPct={}, gain={}, agc={}", 
                 session_id,
                 msg.value("name", "unknown"),
                 msg.value("clipPct", 0.0),
                 msg.value("gain", 0.0),
                 msg.value("agc", 0));
    
    // TODO: Update LUT parameters and regenerate tiles
    // For now, just acknowledge
    nlohmann::json ack = {
        {"type", "lut_update_ack"},
        {"name", msg.value("name", "unknown")}
    };
    send_control_response(session_id, ack);
}

void WebRtcServer::generate_tiles_for_slice(const std::string& session_id, uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx) {
    if (tile_generation_callback_) {
        tile_generation_callback_(session_id, inline_idx, xline_idx, z_idx);
    } else {
        spdlog::error("Cannot generate tiles - tile generation callback not set");
    }
}

} // namespace remoteview