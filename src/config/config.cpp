#include "config.hpp"
#include <fstream>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace remoteview {

void Config::load_from_file(const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open config file: " + filepath);
        }
        
        nlohmann::json j;
        file >> j;
        from_json(j);
        
        spdlog::info("Loaded configuration from: {}", filepath);
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config from {}: {}", filepath, e.what());
        throw;
    }
}

void Config::load_default() {
    apply_defaults();
    spdlog::info("Using default configuration");
}

void Config::save_to_file(const std::string& filepath) const {
    try {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot create config file: " + filepath);
        }
        
        auto j = to_json();
        file << j.dump(2);
        
        spdlog::info("Saved configuration to: {}", filepath);
    } catch (const std::exception& e) {
        spdlog::error("Failed to save config to {}: {}", filepath, e.what());
        throw;
    }
}

void Config::apply_defaults() {
    // All defaults are set in the header file member initializers
}

nlohmann::json Config::to_json() const {
    nlohmann::json j;
    
    j["vds"] = {
        {"path", vds_path_}
    };
    
    j["server"] = {
        {"port", server_port_},
        {"bind_address", bind_address_}
    };
    
    j["tiles"] = {
        {"default_size", default_tile_size_},
        {"max_in_flight", max_tiles_in_flight_}
    };
    
    j["cache"] = {
        {"size_mb", cache_size_mb_}
    };
    
    j["compression"] = {
        {"algorithm", compression_algorithm_},
        {"level", compression_level_}
    };
    
    j["adaptivity"] = {
        {"adaptive_tile_size", adaptive_tile_size_},
        {"adaptive_compression", adaptive_compression_}
    };
    
    j["security"] = {
        {"enable_dtls", enable_dtls_},
        {"cert_file", cert_file_},
        {"key_file", key_file_}
    };
    
    j["logging"] = {
        {"level", static_cast<int>(log_level_)},
        {"file", log_file_}
    };
    
    j["metrics"] = {
        {"enabled", enable_metrics_},
        {"port", metrics_port_}
    };
    
    return j;
}

void Config::from_json(const nlohmann::json& j) {
    if (j.contains("vds")) {
        auto& vds = j["vds"];
        if (vds.contains("path")) {
            vds_path_ = vds["path"].get<std::string>();
        }
    }
    
    if (j.contains("server")) {
        auto& server = j["server"];
        if (server.contains("port")) {
            server_port_ = server["port"].get<uint16_t>();
        }
        if (server.contains("bind_address")) {
            bind_address_ = server["bind_address"].get<std::string>();
        }
    }
    
    if (j.contains("tiles")) {
        auto& tiles = j["tiles"];
        if (tiles.contains("default_size")) {
            default_tile_size_ = tiles["default_size"].get<uint32_t>();
        }
        if (tiles.contains("max_in_flight")) {
            max_tiles_in_flight_ = tiles["max_in_flight"].get<uint32_t>();
        }
    }
    
    if (j.contains("cache")) {
        auto& cache = j["cache"];
        if (cache.contains("size_mb")) {
            cache_size_mb_ = cache["size_mb"].get<size_t>();
        }
    }
    
    if (j.contains("compression")) {
        auto& compression = j["compression"];
        if (compression.contains("algorithm")) {
            compression_algorithm_ = compression["algorithm"].get<std::string>();
        }
        if (compression.contains("level")) {
            compression_level_ = compression["level"].get<int>();
        }
    }
    
    if (j.contains("adaptivity")) {
        auto& adaptivity = j["adaptivity"];
        if (adaptivity.contains("adaptive_tile_size")) {
            adaptive_tile_size_ = adaptivity["adaptive_tile_size"].get<bool>();
        }
        if (adaptivity.contains("adaptive_compression")) {
            adaptive_compression_ = adaptivity["adaptive_compression"].get<bool>();
        }
    }
    
    if (j.contains("security")) {
        auto& security = j["security"];
        if (security.contains("enable_dtls")) {
            enable_dtls_ = security["enable_dtls"].get<bool>();
        }
        if (security.contains("cert_file")) {
            cert_file_ = security["cert_file"].get<std::string>();
        }
        if (security.contains("key_file")) {
            key_file_ = security["key_file"].get<std::string>();
        }
    }
    
    if (j.contains("logging")) {
        auto& logging = j["logging"];
        if (logging.contains("level")) {
            log_level_ = static_cast<spdlog::level::level_enum>(
                logging["level"].get<int>());
        }
        if (logging.contains("file")) {
            log_file_ = logging["file"].get<std::string>();
        }
    }
    
    if (j.contains("metrics")) {
        auto& metrics = j["metrics"];
        if (metrics.contains("enabled")) {
            enable_metrics_ = metrics["enabled"].get<bool>();
        }
        if (metrics.contains("port")) {
            metrics_port_ = metrics["port"].get<uint16_t>();
        }
    }
}

} // namespace remoteview