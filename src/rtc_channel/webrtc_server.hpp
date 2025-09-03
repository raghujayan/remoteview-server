#pragma once

#include "config/config.hpp"
#include "protocol/tile_message.hpp"
#include <libwebsockets.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace remoteview {

// Forward declarations
class Server;

// WebSocket connection session
struct WebSocketSession {
    struct lws* wsi = nullptr;
    std::string session_id;
    bool authenticated = false;
    
    // Connection state for DataChannel simulation
    bool datachannel_ready = false;
    std::chrono::steady_clock::time_point last_ping;
};

class WebRtcServer {
public:
    explicit WebRtcServer(std::shared_ptr<Config> config);
    ~WebRtcServer();
    
    // Set reference to main server for tile generation
    void set_server(Server* server) { server_ = server; }
    
    // Callback type for tile generation  
    using TileGenerationCallback = std::function<void(const std::string&, uint32_t, uint32_t, uint32_t)>;
    
    // Set callback for tile generation instead of direct server access
    void set_tile_generation_callback(TileGenerationCallback callback) {
        tile_generation_callback_ = callback;
    }
    
    void start();
    void stop();
    bool is_running() const { return running_.load(); }
    
    // Send binary tile data to client
    bool send_tile_data(const std::string& session_id, const std::vector<uint8_t>& tile_data);
    
    // JSON control protocol handlers
    void handle_control_message(const std::string& session_id, const nlohmann::json& msg);
    void send_control_response(const std::string& session_id, const nlohmann::json& response);
    
private:
    std::shared_ptr<Config> config_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> server_thread_;
    
    // libwebsockets context
    struct lws_context* ws_context_ = nullptr;
    
    // Active sessions
    std::unordered_map<std::string, std::unique_ptr<WebSocketSession>> sessions_;
    std::mutex sessions_mutex_;
    
    // Server implementation
    void server_loop();
    void setup_websocket_server();
    void cleanup_websocket_server();
    
    // Session management
    std::string create_session(struct lws* wsi);
    void remove_session(const std::string& session_id);
    WebSocketSession* get_session(struct lws* wsi);
    
public:
    // libwebsockets callbacks
    static int websocket_callback(struct lws* wsi, enum lws_callback_reasons reason,
                                void* user, void* in, size_t len);

    // Reference to main server for VDS access
    Server* server_ = nullptr;
    
    // Callback for tile generation
    TileGenerationCallback tile_generation_callback_;
    
    // Protocol handlers
    void handle_handshake(const std::string& session_id, const nlohmann::json& msg);
    void handle_ping(const std::string& session_id, const nlohmann::json& msg);
    void handle_tile_request(const std::string& session_id, const nlohmann::json& msg);
    
    // Client control message handlers
    void handle_set_slice(const std::string& session_id, const nlohmann::json& msg);
    void handle_set_view(const std::string& session_id, const nlohmann::json& msg);
    void handle_set_lut(const std::string& session_id, const nlohmann::json& msg);
    
    // WebRTC signaling handlers
    void handle_webrtc_offer(const std::string& session_id, const nlohmann::json& msg);
    void handle_webrtc_answer(const std::string& session_id, const nlohmann::json& msg);
    void handle_ice_candidate(const std::string& session_id, const nlohmann::json& msg);
    
    // Tile generation for seismic data
    void generate_tiles_for_slice(const std::string& session_id, uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx);
    
    // Singleton instance for callback access
    static WebRtcServer* instance_;
};

} // namespace remoteview