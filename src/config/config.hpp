#pragma once

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace remoteview {

class Config {
public:
    Config() = default;
    
    void load_from_file(const std::string& filepath);
    void load_default();
    void save_to_file(const std::string& filepath) const;
    
    // VDS Configuration
    const std::string& get_vds_path() const { return vds_path_; }
    void set_vds_path(const std::string& path) { vds_path_ = path; }
    
    // Server Configuration
    uint16_t get_server_port() const { return server_port_; }
    void set_server_port(uint16_t port) { server_port_ = port; }
    
    const std::string& get_bind_address() const { return bind_address_; }
    void set_bind_address(const std::string& address) { bind_address_ = address; }
    
    // Tile Configuration
    uint32_t get_default_tile_size() const { return default_tile_size_; }
    void set_default_tile_size(uint32_t size) { default_tile_size_ = size; }
    
    uint32_t get_max_tiles_in_flight() const { return max_tiles_in_flight_; }
    void set_max_tiles_in_flight(uint32_t count) { max_tiles_in_flight_ = count; }
    
    // Cache Configuration
    size_t get_cache_size_mb() const { return cache_size_mb_; }
    void set_cache_size_mb(size_t size_mb) { cache_size_mb_ = size_mb; }
    
    // Compression Configuration
    const std::string& get_compression_algorithm() const { return compression_algorithm_; }
    void set_compression_algorithm(const std::string& algorithm) { compression_algorithm_ = algorithm; }
    
    int get_compression_level() const { return compression_level_; }
    void set_compression_level(int level) { compression_level_ = level; }
    
    // Adaptivity Configuration
    bool is_adaptive_tile_size_enabled() const { return adaptive_tile_size_; }
    void set_adaptive_tile_size(bool enabled) { adaptive_tile_size_ = enabled; }
    
    bool is_adaptive_compression_enabled() const { return adaptive_compression_; }
    void set_adaptive_compression(bool enabled) { adaptive_compression_ = enabled; }
    
    // Security Configuration
    bool is_dtls_enabled() const { return enable_dtls_; }
    void set_dtls_enabled(bool enabled) { enable_dtls_ = enabled; }
    
    const std::string& get_cert_file() const { return cert_file_; }
    void set_cert_file(const std::string& path) { cert_file_ = path; }
    
    const std::string& get_key_file() const { return key_file_; }
    void set_key_file(const std::string& path) { key_file_ = path; }
    
    // Logging Configuration
    spdlog::level::level_enum get_log_level() const { return log_level_; }
    void set_log_level(spdlog::level::level_enum level) { log_level_ = level; }
    
    const std::string& get_log_file() const { return log_file_; }
    void set_log_file(const std::string& path) { log_file_ = path; }
    
    // Metrics Configuration
    bool is_metrics_enabled() const { return enable_metrics_; }
    void set_metrics_enabled(bool enabled) { enable_metrics_ = enabled; }
    
    uint16_t get_metrics_port() const { return metrics_port_; }
    void set_metrics_port(uint16_t port) { metrics_port_ = port; }

private:
    void apply_defaults();
    nlohmann::json to_json() const;
    void from_json(const nlohmann::json& j);
    
    // VDS - exact path from project plan
    std::string vds_path_ = "/home/rocky/onnia2x3d_mig_Time.vds";
    
    // Server
    uint16_t server_port_ = 8080;
    std::string bind_address_ = "0.0.0.0";
    
    // Tiles
    uint32_t default_tile_size_ = 256;
    uint32_t max_tiles_in_flight_ = 64;
    
    // Cache
    size_t cache_size_mb_ = 1024;
    
    // Compression
    std::string compression_algorithm_ = "lz4";
    int compression_level_ = 1;
    
    // Adaptivity
    bool adaptive_tile_size_ = true;
    bool adaptive_compression_ = true;
    
    // Security
    bool enable_dtls_ = true;
    std::string cert_file_ = "";
    std::string key_file_ = "";
    
    // Logging
    spdlog::level::level_enum log_level_ = spdlog::level::info;
    std::string log_file_ = "";
    
    // Metrics
    bool enable_metrics_ = true;
    uint16_t metrics_port_ = 9090;
};

} // namespace remoteview