#include "server.hpp"
#include <spdlog/spdlog.h>
#include <signal.h>
#include <thread>
#include <chrono>

namespace remoteview {

static Server* g_server_instance = nullptr;

static void signal_handler(int signal) {
    if (g_server_instance) {
        spdlog::info("Received signal {}, shutting down...", signal);
        g_server_instance->stop();
    }
}

Server::Server(std::shared_ptr<Config> config) 
    : config_(std::move(config)) {
    
    vds_reader_ = std::make_unique<VdsReader>(config_);
    tile_cache_ = std::make_unique<TileCache>(config_);
    webrtc_server_ = std::make_unique<WebRtcServer>(config_);
    metrics_ = std::make_unique<MetricsCollector>(config_);
    adaptivity_ = std::make_unique<Adapt>(config_);
    opengl_renderer_ = std::make_unique<HueSpaceRenderer>(config_);
}

Server::~Server() {
    stop();
}

void Server::start() {
    if (running_.load()) {
        spdlog::warn("Server is already running");
        return;
    }
    
    setup_signal_handlers();
    
    try {
        vds_reader_->initialize();
        tile_cache_->initialize();
        
        // Initialize HueSpace OpenGL renderer for high-quality frame generation
        spdlog::info("🎨 Initializing HueSpace OpenGL renderer...");
        HueSpaceRenderer::RenderConfig render_config;
        render_config.width = 1024;   // Higher resolution for better quality
        render_config.height = 768;   
        render_config.enable_msaa = true;
        render_config.msaa_samples = 4;
        render_config.enable_oit = true;
        
        if (!opengl_renderer_->initialize(render_config)) {
            throw std::runtime_error("Failed to initialize HueSpace OpenGL renderer");
        }
        
        spdlog::info("✅ HueSpace OpenGL renderer initialized (VDS will be loaded on-demand)");
        
        // Connect WebRTC server to main server for VDS access
        webrtc_server_->set_server(this);
        
        // Set up tile generation callback
        webrtc_server_->set_tile_generation_callback(
            [this](const std::string& session_id, uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx) {
                // Update VDS reader with new slice
                this->handle_slice_change(inline_idx, xline_idx, z_idx);
                // Generate and send real VDS tiles
                this->generate_and_send_vds_tiles(session_id, inline_idx, xline_idx, z_idx);
            }
        );
        
        webrtc_server_->start();
        
        metrics_->start();
        adaptivity_->initialize();
        
        running_.store(true);
        spdlog::info("RemoteView Server started successfully");
        
        main_loop();
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to start server components: {}", e.what());
        throw;
    }
}

void Server::stop() {
    if (!running_.load()) {
        return;
    }
    
    shutdown_requested_.store(true);
    
    spdlog::info("Stopping server components...");
    
    if (adaptivity_) adaptivity_->shutdown();
    if (metrics_) metrics_->stop();
    if (webrtc_server_) webrtc_server_->stop();
    if (tile_cache_) tile_cache_->shutdown();
    if (vds_reader_) vds_reader_->shutdown();
    
    running_.store(false);
    spdlog::info("RemoteView Server stopped");
}

void Server::setup_signal_handlers() {
    g_server_instance = this;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
}

void Server::main_loop() {
    while (!shutdown_requested_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (metrics_) {
            metrics_->collect();
        }
    }
}

void Server::handle_slice_change(uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx) {
    if (!vds_reader_) {
        spdlog::warn("Cannot handle slice change - VDS reader not initialized");
        return;
    }
    
    spdlog::info("Server handling slice change to ({},{},{})", inline_idx, xline_idx, z_idx);
    
    // Update VDS reader with new slice - this cancels prefetch requests
    vds_reader_->set_current_slice(inline_idx, xline_idx, z_idx);
    
    // Clear DataChannel queues as they contain tiles from old slice
    if (webrtc_server_) {
        // TODO: Get DataChannelManager from WebRtcServer and clear queues
        // webrtc_server_->get_data_channel_manager()->clear_all_queues();
        spdlog::debug("Clearing DataChannel queues due to slice change");
    }
    
    // Optionally clear tile cache for old slice (aggressive cache invalidation)
    // Note: This could be made smarter to only evict tiles from the old slice
    if (tile_cache_) {
        spdlog::debug("Clearing tile cache due to slice change");
        tile_cache_->clear();
    }
}

void Server::configure_testing_hooks(const TestingHooks& hooks) {
    testing_hooks_ = hooks;
    
    if (hooks.enabled) {
        spdlog::info("Testing hooks configured:");
        
        if (hooks.roi_inline_end > 0) {
            spdlog::info("  ROI enabled: inline({}-{}), xline({}-{}), z({}-{})",
                hooks.roi_inline_start, hooks.roi_inline_end,
                hooks.roi_xline_start, hooks.roi_xline_end,
                hooks.roi_z_start, hooks.roi_z_end);
        }
        
        if (hooks.enable_tile_dump) {
            spdlog::info("  Tile dumping enabled to: {}", hooks.dump_directory);
            
            // Create dump directory if it doesn't exist
            // TODO: Add filesystem include for directory creation
            spdlog::debug("  Dump directory: {}", hooks.dump_directory);
        }
        
        if (hooks.enable_performance_profiling) {
            spdlog::info("  Performance profiling enabled");
            
            // Enable detailed metrics collection
            if (metrics_) {
                // TODO: Enable performance profiling mode in metrics
                spdlog::debug("  Metrics profiling mode would be enabled");
            }
        }
        
        spdlog::info("  Max test tiles: {}", hooks.max_test_tiles);
    }
}

std::vector<uint8_t> Server::read_vds_tile_data(uint32_t plane_index, uint32_t slice_index,
                                               uint32_t tile_x, uint32_t tile_y, 
                                               uint32_t tile_w, uint32_t tile_h) {
    if (!vds_reader_) {
        spdlog::error("Cannot read VDS tile - VDS reader not initialized");
        return {};
    }
    
    try {
        // Create VDS tile request
        TileRequest request;
        request.plane_index = plane_index;
        request.slice_index = slice_index;
        request.x_offset = tile_x;
        request.y_offset = tile_y;
        request.width = tile_w;
        request.height = tile_h;
        request.downsample_level = 0; // Full resolution
        
        // Validate request
        if (!vds_reader_->is_valid_tile_request(request)) {
            spdlog::warn("Invalid VDS tile request: plane={}, slice={}, x={}, y={}, w={}, h={}", 
                        plane_index, slice_index, tile_x, tile_y, tile_w, tile_h);
            return {};
        }
        
        // Read actual VDS data
        auto vds_tile = vds_reader_->read_tile(request);
        if (!vds_tile) {
            spdlog::warn("VDS reader returned null tile: plane={}, slice={}, x={}, y={}", 
                        plane_index, slice_index, tile_x, tile_y);
            return {};
        }
        
        // Log some sample values to verify data content
        if (vds_tile->data.size() >= 20) {
            spdlog::info("VDS sample values (first 10 bytes): {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}", 
                        vds_tile->data[0], vds_tile->data[1], vds_tile->data[2], vds_tile->data[3], vds_tile->data[4],
                        vds_tile->data[5], vds_tile->data[6], vds_tile->data[7], vds_tile->data[8], vds_tile->data[9]);
            
            // Convert first few samples to see actual seismic values
            for (size_t i = 0; i < 5 && i * 2 + 1 < vds_tile->data.size(); i++) {
                int16_t sample = vds_tile->data[i*2] | (vds_tile->data[i*2 + 1] << 8);
                spdlog::info("Sample {}: {}", i, sample);
            }
        }
        
        spdlog::info("Successfully read VDS tile: plane={}, slice={}, size={}x{}, bytes={}", 
                    plane_index, slice_index, vds_tile->width, vds_tile->height, vds_tile->data.size());
        
        return vds_tile->data;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception reading VDS tile: {}", e.what());
        return {};
    }
}

void Server::generate_and_send_vds_tiles(const std::string& session_id, 
                                         uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx) {
    if (!opengl_renderer_ || !webrtc_server_) {
        spdlog::error("Cannot generate OpenGL frame - OpenGL renderer or WebRTC server not available");
        return;
    }
    
    spdlog::info("🎨 Generating HueSpace OpenGL frame for session {} at slice ({},{},{})", 
                 session_id, inline_idx, xline_idx, z_idx);
    
    try {
        // Share VDS object from VdsReader with OpenGL renderer on-demand
        static bool vds_shared = false;
        if (!vds_shared && vds_reader_) {
            spdlog::info("🔗 Sharing VDS object with OpenGL renderer");
            auto* vds_object = vds_reader_->get_vds_object();
            if (vds_object && opengl_renderer_->set_vds_object(vds_object)) {
                vds_shared = true;
                spdlog::info("✅ VDS object shared successfully");
            } else {
                spdlog::error("Failed to share VDS object with OpenGL renderer");
                return;
            }
        }
        
        // Set up slice parameters for HueSpace rendering
        HueSpaceRenderer::SliceParams slice_params;
        slice_params.inline_index = static_cast<int>(inline_idx);
        slice_params.crossline_index = static_cast<int>(xline_idx); 
        slice_params.time_index = static_cast<int>(z_idx);
        slice_params.show_inline = true;
        slice_params.show_crossline = true;
        slice_params.show_time = true;
        slice_params.opacity = 1.0f;
        slice_params.amplitude_scale = 1.0f;
        slice_params.contrast = 1.0f;
        slice_params.brightness = 0.0f;
        
        // Set up camera parameters
        HueSpaceRenderer::CameraParams camera_params;
        camera_params.position = {0, 0, 2};  // Camera position
        camera_params.target = {0, 0, 0};    // Look at origin
        camera_params.up = {0, 1, 0};        // Up vector
        camera_params.zoom = 1.0f;
        
        // Render frame using HueSpace OpenGL
        if (!opengl_renderer_->render_frame(slice_params, camera_params)) {
            spdlog::error("Failed to render HueSpace OpenGL frame");
            return;
        }
        
        // Get rendered frame as RGBA data
        std::vector<uint8_t> rgba_data;
        int width, height;
        if (!opengl_renderer_->get_frame_rgba(rgba_data, width, height)) {
            spdlog::error("Failed to get rendered frame data");
            return;
        }
        
        // Debug: Check if frame has actual content
        bool has_content = false;
        uint8_t max_value = 0;
        for (size_t i = 0; i < std::min(size_t(400), rgba_data.size()); i += 4) {
            uint8_t r = rgba_data[i];
            uint8_t g = rgba_data[i + 1];
            uint8_t b = rgba_data[i + 2];
            max_value = std::max({max_value, r, g, b});
            if (r > 0 || g > 0 || b > 0) {
                has_content = true;
            }
        }
        
        spdlog::info("✅ HueSpace frame rendered: {}x{} pixels, {} bytes, has_content={}, max_value={}", 
                     width, height, rgba_data.size(), has_content, (int)max_value);
        
        // Create a frame message using the existing TileMessage protocol
        // We'll send it as a single "tile" that covers the whole frame
        auto plane_type = protocol::PlaneType::Inline; // Use inline as default view
        auto data_type = protocol::DataType::U8;        // RGBA = 4 x U8
        auto compression = protocol::CompressionType::None;
        
        // Create frame message (treating as single large tile)
        protocol::TileMessage frame_msg(
            plane_type,
            inline_idx,  // Use requested slice index
            0, 0,        // x, y offset (full frame)
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), // frame dimensions
            data_type,
            compression,
            std::move(rgba_data)
        );
        
        // Serialize frame data
        auto binary_data = frame_msg.serialize();
        
        // Send via WebRTC server
        if (webrtc_server_->send_tile_data(session_id, binary_data)) {
            spdlog::info("✅ Sent HueSpace OpenGL frame: {}x{} pixels, {} total bytes", 
                        width, height, binary_data.size());
        } else {
            spdlog::error("❌ Failed to send HueSpace OpenGL frame");
        }
        
    } catch (const std::exception& e) {
        spdlog::error("Exception generating HueSpace OpenGL frame: {}", e.what());
    }
}

bool Server::test_opengl_rendering() {
    spdlog::info("🧪 Testing OpenGL rendering with HueSpace...");
    
    try {
        // Initialize OpenGL renderer
        HueSpaceRenderer::RenderConfig render_config;
        render_config.width = 512;
        render_config.height = 512;
        render_config.enable_msaa = false;  // Keep simple for test
        
        if (!opengl_renderer_->initialize(render_config)) {
            spdlog::error("Failed to initialize OpenGL renderer");
            return false;
        }
        
        // Load VDS file if configured
        if (!config_->get_vds_path().empty()) {
            if (!opengl_renderer_->load_vds(config_->get_vds_path())) {
                spdlog::error("Failed to load VDS file for OpenGL rendering");
                return false;
            }
            
            // Get VDS info
            int inline_count, crossline_count, time_count;
            if (opengl_renderer_->get_vds_dimensions(inline_count, crossline_count, time_count)) {
                spdlog::info("VDS dimensions: {}x{}x{}", inline_count, crossline_count, time_count);
            }
        }
        
        // Test rendering
        HueSpaceRenderer::SliceParams slice_params;
        slice_params.inline_index = 0;
        slice_params.crossline_index = 0;
        slice_params.time_index = 0;
        
        HueSpaceRenderer::CameraParams camera_params;
        camera_params.position = {0, 0, 2};
        camera_params.target = {0, 0, 0};
        
        if (!opengl_renderer_->render_frame(slice_params, camera_params)) {
            spdlog::error("Failed to render test frame");
            return false;
        }
        
        // Get rendered frame
        std::vector<uint8_t> rgba_data;
        int width, height;
        if (!opengl_renderer_->get_frame_rgba(rgba_data, width, height)) {
            spdlog::error("Failed to get rendered frame data");
            return false;
        }
        
        spdlog::info("✅ OpenGL rendering test successful!");
        spdlog::info("   Rendered frame: {}x{} pixels, {} bytes", width, height, rgba_data.size());
        
        // Save test frame to file (optional)
        std::string output_path = "/tmp/huespace_render_test.raw";
        std::ofstream file(output_path, std::ios::binary);
        if (file.is_open()) {
            file.write(reinterpret_cast<const char*>(rgba_data.data()), rgba_data.size());
            file.close();
            spdlog::info("   Test frame saved to: {}", output_path);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception during OpenGL rendering test: {}", e.what());
        return false;
    }
}

} // namespace remoteview