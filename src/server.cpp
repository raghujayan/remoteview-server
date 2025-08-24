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

} // namespace remoteview