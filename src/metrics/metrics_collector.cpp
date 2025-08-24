#include "metrics_collector.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <thread>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/mach_host.h>
#elif __linux__
#include <sys/sysinfo.h>
#include <sys/stat.h>
#endif

namespace remoteview {

// Static singleton instance
MetricsCollector* MetricsCollector::instance_ = nullptr;

MetricsCollector::MetricsCollector(std::shared_ptr<Config> config)
    : config_(std::move(config))
    , start_time_(std::chrono::steady_clock::now()) {
    instance_ = this;
}

MetricsCollector::~MetricsCollector() {
    stop();
    instance_ = nullptr;
}

void MetricsCollector::start() {
    if (running_.load()) {
        spdlog::warn("Metrics collector is already running");
        return;
    }
    
    if (!config_->is_metrics_enabled()) {
        spdlog::info("Metrics collection disabled in config");
        return;
    }
    
    spdlog::info("Starting metrics collector on port {}", config_->get_metrics_port());
    
    running_.store(true);
    
    // Start HTTP server for metrics endpoint
    http_server_ = std::make_unique<httplib::Server>();
    
    // Setup metrics endpoint
    http_server_->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto json = get_metrics_json();
            res.set_content(json.dump(2), "application/json");
        } catch (const std::exception& e) {
            spdlog::error("Error generating metrics JSON: {}", e.what());
            res.status = 500;
            res.set_content("Internal server error", "text/plain");
        }
    });
    
    // Setup health check endpoint
    http_server_->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\": \"healthy\"}", "application/json");
    });
    
    // Start HTTP server thread
    http_thread_ = std::thread(&MetricsCollector::http_server_thread, this);
    
    // Start metrics collection thread
    collection_thread_ = std::thread(&MetricsCollector::metrics_collection_thread, this);
    
    spdlog::info("Metrics collector started");
}

void MetricsCollector::stop() {
    if (!running_.load()) {
        return;
    }
    
    spdlog::info("Stopping metrics collector");
    running_.store(false);
    
    // Stop HTTP server
    if (http_server_) {
        http_server_->stop();
    }
    
    // Join threads
    if (http_thread_.joinable()) {
        http_thread_.join();
    }
    if (collection_thread_.joinable()) {
        collection_thread_.join();
    }
    
    http_server_.reset();
    
    spdlog::info("Metrics collector stopped");
}

void MetricsCollector::collect() {
    if (!running_.load()) {
        return;
    }
    
    collect_system_metrics();
}

void MetricsCollector::record_compression(uint64_t compressed, uint64_t uncompressed) {
    metrics_.compressed_bytes += compressed;
    metrics_.uncompressed_bytes += uncompressed;
    
    if (uncompressed > 0) {
        double ratio = static_cast<double>(compressed) / static_cast<double>(uncompressed);
        
        std::lock_guard<std::mutex> lock(avg_mutex_);
        compression_ratios_.push_back(ratio);
        
        // Keep only last 1000 ratios for averaging
        if (compression_ratios_.size() > 1000) {
            compression_ratios_.erase(compression_ratios_.begin());
        }
        
        // Calculate average
        double sum = 0.0;
        for (double r : compression_ratios_) {
            sum += r;
        }
        metrics_.avg_compression_ratio = sum / compression_ratios_.size();
    }
}

void MetricsCollector::record_tile_generation_time(double ms) {
    std::lock_guard<std::mutex> lock(avg_mutex_);
    tile_generation_times_.push_back(ms);
    
    // Keep only last 1000 times for averaging
    if (tile_generation_times_.size() > 1000) {
        tile_generation_times_.erase(tile_generation_times_.begin());
    }
    
    // Calculate average
    double sum = 0.0;
    for (double t : tile_generation_times_) {
        sum += t;
    }
    metrics_.avg_tile_generation_ms = sum / tile_generation_times_.size();
}

nlohmann::json MetricsCollector::get_metrics_json() const {
    auto now = std::chrono::steady_clock::now();
    auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    
    nlohmann::json j;
    
    // Basic info
    j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    j["uptime_seconds"] = uptime_seconds;
    
    // Connection metrics
    j["connections"]["active"] = metrics_.active_connections.load();
    j["connections"]["total"] = metrics_.total_connections.load();
    j["connections"]["disconnections"] = metrics_.disconnections.load();
    
    // Tile metrics
    j["tiles"]["served"] = metrics_.tiles_served.load();
    j["tiles"]["cached"] = metrics_.tiles_cached.load();
    j["tiles"]["generated"] = metrics_.tiles_generated.load();
    j["tiles"]["cache_hits"] = metrics_.tile_cache_hits.load();
    j["tiles"]["cache_misses"] = metrics_.tile_cache_misses.load();
    
    auto cache_requests = metrics_.tile_cache_hits.load() + metrics_.tile_cache_misses.load();
    if (cache_requests > 0) {
        j["tiles"]["cache_hit_ratio"] = static_cast<double>(metrics_.tile_cache_hits.load()) / cache_requests;
    } else {
        j["tiles"]["cache_hit_ratio"] = 0.0;
    }
    
    // Data transfer metrics  
    j["data_transfer"]["bytes_sent"] = metrics_.bytes_sent.load();
    j["data_transfer"]["bytes_received"] = metrics_.bytes_received.load();
    j["data_transfer"]["compressed_bytes"] = metrics_.compressed_bytes.load();
    j["data_transfer"]["uncompressed_bytes"] = metrics_.uncompressed_bytes.load();
    
    // Performance metrics
    j["performance"]["avg_tile_generation_ms"] = metrics_.avg_tile_generation_ms.load();
    j["performance"]["avg_compression_ratio"] = metrics_.avg_compression_ratio.load();
    j["performance"]["prefetch_cancellations"] = metrics_.prefetch_cancellations.load();
    
    // Error metrics
    j["errors"]["protocol_errors"] = metrics_.protocol_errors.load();
    j["errors"]["compression_errors"] = metrics_.compression_errors.load();
    j["errors"]["vds_read_errors"] = metrics_.vds_read_errors.load();
    j["errors"]["validation_errors"] = metrics_.validation_errors.load();
    
    // System metrics
    j["system"]["cpu_usage_percent"] = metrics_.cpu_usage_percent.load();
    j["system"]["memory_usage_bytes"] = metrics_.memory_usage_bytes.load();
    j["system"]["cache_size_bytes"] = metrics_.cache_size_bytes.load();
    
    return j;
}

void MetricsCollector::http_server_thread() {
    try {
        if (!http_server_->listen("0.0.0.0", config_->get_metrics_port())) {
            spdlog::error("Failed to start metrics HTTP server on port {}", config_->get_metrics_port());
        }
    } catch (const std::exception& e) {
        spdlog::error("Metrics HTTP server thread error: {}", e.what());
    }
}

void MetricsCollector::metrics_collection_thread() {
    try {
        while (running_.load()) {
            collect_system_metrics();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    } catch (const std::exception& e) {
        spdlog::error("Metrics collection thread error: {}", e.what());
    }
}

void MetricsCollector::collect_system_metrics() {
    try {
        metrics_.cpu_usage_percent = get_cpu_usage();
        metrics_.memory_usage_bytes = get_memory_usage();
    } catch (const std::exception& e) {
        spdlog::debug("Error collecting system metrics: {}", e.what());
    }
}

double MetricsCollector::get_cpu_usage() {
    // Simple CPU usage calculation - this is a basic implementation
    // In production, you might want to use a more sophisticated approach
    static clock_t last_cpu_time = 0;
    static clock_t last_system_time = 0;
    
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        clock_t current_cpu_time = usage.ru_utime.tv_sec * CLOCKS_PER_SEC + usage.ru_utime.tv_usec;
        clock_t current_system_time = clock();
        
        if (last_system_time != 0) {
            double cpu_percent = static_cast<double>(current_cpu_time - last_cpu_time) / 
                               static_cast<double>(current_system_time - last_system_time) * 100.0;
            
            last_cpu_time = current_cpu_time;
            last_system_time = current_system_time;
            
            return std::max(0.0, std::min(100.0, cpu_percent));
        }
        
        last_cpu_time = current_cpu_time;
        last_system_time = current_system_time;
    }
    
    return 0.0;
}

uint64_t MetricsCollector::get_memory_usage() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
        // ru_maxrss is in bytes on macOS
        return static_cast<uint64_t>(usage.ru_maxrss);
#elif __linux__
        // ru_maxrss is in kilobytes on Linux
        return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
    }
    return 0;
}

} // namespace remoteview