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

} // namespace remoteview