#pragma once

#include "config/config.hpp"
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <string>
#include <nlohmann/json.hpp>
#include <httplib.h>

namespace remoteview {

struct MetricsData {
    // Connection metrics
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> total_connections{0};
    std::atomic<uint64_t> disconnections{0};
    
    // Tile metrics
    std::atomic<uint64_t> tiles_served{0};
    std::atomic<uint64_t> tiles_cached{0};
    std::atomic<uint64_t> tiles_generated{0};
    std::atomic<uint64_t> tile_cache_hits{0};
    std::atomic<uint64_t> tile_cache_misses{0};
    
    // Data transfer metrics
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> compressed_bytes{0};
    std::atomic<uint64_t> uncompressed_bytes{0};
    
    // Performance metrics
    std::atomic<double> avg_tile_generation_ms{0.0};
    std::atomic<double> avg_compression_ratio{0.0};
    std::atomic<uint64_t> prefetch_cancellations{0};
    
    // Error metrics
    std::atomic<uint64_t> protocol_errors{0};
    std::atomic<uint64_t> compression_errors{0};
    std::atomic<uint64_t> vds_read_errors{0};
    std::atomic<uint64_t> validation_errors{0};
    
    // System metrics
    std::atomic<double> cpu_usage_percent{0.0};
    std::atomic<uint64_t> memory_usage_bytes{0};
    std::atomic<uint64_t> cache_size_bytes{0};
    
    // Reset counters
    void reset() {
        active_connections = 0;
        total_connections = 0;
        disconnections = 0;
        tiles_served = 0;
        tiles_cached = 0;
        tiles_generated = 0;
        tile_cache_hits = 0;
        tile_cache_misses = 0;
        bytes_sent = 0;
        bytes_received = 0;
        compressed_bytes = 0;
        uncompressed_bytes = 0;
        avg_tile_generation_ms = 0.0;
        avg_compression_ratio = 0.0;
        prefetch_cancellations = 0;
        protocol_errors = 0;
        compression_errors = 0;
        vds_read_errors = 0;
        validation_errors = 0;
        cpu_usage_percent = 0.0;
        memory_usage_bytes = 0;
        cache_size_bytes = 0;
    }
};

class MetricsCollector {
public:
    explicit MetricsCollector(std::shared_ptr<Config> config);
    ~MetricsCollector();
    
    void start();
    void stop();
    void collect();
    bool is_running() const { return running_.load(); }
    
    // Metrics update methods
    void increment_connections() { metrics_.total_connections++; metrics_.active_connections++; }
    void decrement_connections() { metrics_.active_connections--; metrics_.disconnections++; }
    void increment_tiles_served() { metrics_.tiles_served++; }
    void increment_tiles_cached() { metrics_.tiles_cached++; }
    void increment_tiles_generated() { metrics_.tiles_generated++; }
    void record_cache_hit() { metrics_.tile_cache_hits++; }
    void record_cache_miss() { metrics_.tile_cache_misses++; }
    void add_bytes_sent(uint64_t bytes) { metrics_.bytes_sent += bytes; }
    void add_bytes_received(uint64_t bytes) { metrics_.bytes_received += bytes; }
    void record_compression(uint64_t compressed, uint64_t uncompressed);
    void record_tile_generation_time(double ms);
    void increment_prefetch_cancellations() { metrics_.prefetch_cancellations++; }
    void increment_protocol_errors() { metrics_.protocol_errors++; }
    void increment_compression_errors() { metrics_.compression_errors++; }
    void increment_vds_read_errors() { metrics_.vds_read_errors++; }
    void increment_validation_errors() { metrics_.validation_errors++; }
    void update_cache_size(uint64_t bytes) { metrics_.cache_size_bytes = bytes; }
    
    // Get metrics as JSON
    nlohmann::json get_metrics_json() const;
    
static MetricsCollector* instance() { return instance_; }
    
private:
    void http_server_thread();
    void metrics_collection_thread();
    void collect_system_metrics();
    double get_cpu_usage();
    uint64_t get_memory_usage();
    
    std::shared_ptr<Config> config_;
    std::atomic<bool> running_{false};
    
    MetricsData metrics_;
    
    // HTTP server for metrics endpoint
    std::unique_ptr<httplib::Server> http_server_;
    std::thread http_thread_;
    std::thread collection_thread_;
    
    // For averaging calculations
    mutable std::mutex avg_mutex_;
    std::vector<double> tile_generation_times_;
    std::vector<double> compression_ratios_;
    
    // Singleton for global access
    static MetricsCollector* instance_;
    
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace remoteview