/**
 * RemoteView Server - Main Application Entry Point
 * 
 * This is the main entry point for the RemoteView seismic data streaming server.
 * The server provides real-time streaming of seismic tiles to web clients using:
 * 
 * - HueSpace/OpenVDS for reading seismic data from VDS files
 * - WebSocket connections for client communication
 * - WebRTC DataChannels for low-latency binary tile streaming
 * - LRU caching for performance optimization
 * - LZ4/Zstd compression for bandwidth efficiency
 * 
 * The server supports three orthogonal viewing planes (inline, crossline, time/depth)
 * with multi-resolution tiling for smooth pan/zoom interactions.
 */

#include "server.hpp"
#include "config/config.hpp"
#include <spdlog/spdlog.h>
#include <memory>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <vector>

// TestingHooks struct is now defined in server.hpp
using remoteview::TestingHooks;

void print_usage(const char* program_name) {
    std::cout << "RemoteView Server - Seismic Data Streaming Server\n";
    std::cout << "Usage: " << program_name << " [OPTIONS] [config_file.json]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help                    Show this help message\n";
    std::cout << "  -v, --version                 Show version information\n";
    std::cout << "  -c, --config FILE             Use configuration file\n";
    std::cout << "  --vds-file PATH               Override VDS file path\n";
    std::cout << "  --port PORT                   Override server port\n";
    std::cout << "  --metrics-port PORT           Override metrics port\n";
    std::cout << "  --log-level LEVEL             Set log level (debug,info,warn,error)\n\n";
    std::cout << "Testing Hooks:\n";
    std::cout << "  --enable-testing              Enable testing mode\n";
    std::cout << "  --roi INLINE_MIN,INLINE_MAX,XLINE_MIN,XLINE_MAX,Z_MIN,Z_MAX\n";
    std::cout << "                                Set region of interest for testing\n";
    std::cout << "  --dump-tiles DIR              Enable tile dumping to directory\n";
    std::cout << "  --profile                     Enable performance profiling\n";
    std::cout << "  --max-test-tiles N            Limit number of tiles for testing\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program_name << " --config myconfig.json\n";
    std::cout << "  " << program_name << " --enable-testing --roi 100,200,300,400,0,1000\n";
    std::cout << "  " << program_name << " --dump-tiles ./output --profile\n";
}

bool parse_roi(const std::string& roi_str, TestingHooks& hooks) {
    std::istringstream ss(roi_str);
    std::string token;
    std::vector<uint32_t> values;
    
    while (std::getline(ss, token, ',')) {
        try {
            values.push_back(std::stoul(token));
        } catch (const std::exception&) {
            return false;
        }
    }
    
    if (values.size() != 6) {
        return false;
    }
    
    hooks.roi_inline_start = values[0];
    hooks.roi_inline_end = values[1];
    hooks.roi_xline_start = values[2];
    hooks.roi_xline_end = values[3];
    hooks.roi_z_start = values[4];
    hooks.roi_z_end = values[5];
    
    return true;
}

/**
 * Main application entry point
 * 
 * Command line usage:
 *   remoteview_server [OPTIONS] [config_file.json]
 * 
 * Supports extensive command-line options for configuration override
 * and testing hooks for development and QA purposes.
 * 
 * The server runs indefinitely until terminated with Ctrl+C or SIGTERM.
 * 
 * @param argc Command line argument count
 * @param argv Command line arguments
 * @return 0 on success, 1 on error
 */
int main(int argc, char* argv[]) {
    try {
        // CRITICAL: Set HueSpace license environment variable
        // This must be set before any HueSpace API calls or the server will fail
        // The license server validates access to HueSpace/OpenVDS functionality
        setenv("HUE_LICENSE_FILE", "5053@license.cloud.bluware.com", 1);
        
        // Parse command line options
        TestingHooks testing_hooks;
        std::string config_file;
        std::string vds_override;
        std::string log_level_str;
        uint16_t port_override = 0;
        uint16_t metrics_port_override = 0;
        
        static struct option long_options[] = {
            {"help", no_argument, 0, 'h'},
            {"version", no_argument, 0, 'v'},
            {"config", required_argument, 0, 'c'},
            {"vds-file", required_argument, 0, 1001},
            {"port", required_argument, 0, 1002},
            {"metrics-port", required_argument, 0, 1003},
            {"log-level", required_argument, 0, 1004},
            {"enable-testing", no_argument, 0, 2001},
            {"roi", required_argument, 0, 2002},
            {"dump-tiles", required_argument, 0, 2003},
            {"profile", no_argument, 0, 2004},
            {"max-test-tiles", required_argument, 0, 2005},
            {0, 0, 0, 0}
        };
        
        int c;
        int option_index = 0;
        
        while ((c = getopt_long(argc, argv, "hvc:", long_options, &option_index)) != -1) {
            switch (c) {
                case 'h':
                    print_usage(argv[0]);
                    return 0;
                case 'v':
                    std::cout << "RemoteView Server v1.0.0\n";
                    return 0;
                case 'c':
                case 1001: // --vds-file
                    if (c == 'c') config_file = optarg;
                    else vds_override = optarg;
                    break;
                case 1002: // --port
                    port_override = static_cast<uint16_t>(std::stoul(optarg));
                    break;
                case 1003: // --metrics-port
                    metrics_port_override = static_cast<uint16_t>(std::stoul(optarg));
                    break;
                case 1004: // --log-level
                    log_level_str = optarg;
                    break;
                case 2001: // --enable-testing
                    testing_hooks.enabled = true;
                    break;
                case 2002: // --roi
                    if (!parse_roi(optarg, testing_hooks)) {
                        spdlog::error("Invalid ROI format. Expected: inline_min,inline_max,xline_min,xline_max,z_min,z_max");
                        return 1;
                    }
                    testing_hooks.enabled = true;
                    break;
                case 2003: // --dump-tiles
                    testing_hooks.enable_tile_dump = true;
                    testing_hooks.dump_directory = optarg;
                    testing_hooks.enabled = true;
                    break;
                case 2004: // --profile
                    testing_hooks.enable_performance_profiling = true;
                    testing_hooks.enabled = true;
                    break;
                case 2005: // --max-test-tiles
                    testing_hooks.max_test_tiles = static_cast<uint32_t>(std::stoul(optarg));
                    testing_hooks.enabled = true;
                    break;
                case '?':
                    std::cerr << "Use --help for usage information\n";
                    return 1;
                default:
                    break;
            }
        }
        
        // Handle positional config file argument
        if (optind < argc && config_file.empty()) {
            config_file = argv[optind];
        }
        
        // Load configuration from file or use defaults
        auto config = std::make_shared<remoteview::Config>();
        if (!config_file.empty()) {
            config->load_from_file(config_file);
            spdlog::info("Loaded configuration from: {}", config_file);
        } else {
            config->load_default();
            spdlog::info("Using default configuration");
        }
        
        // Apply command-line overrides
        if (!vds_override.empty()) {
            config->set_vds_path(vds_override);
            spdlog::info("VDS file overridden: {}", vds_override);
        }
        if (port_override > 0) {
            config->set_server_port(port_override);
            spdlog::info("Server port overridden: {}", port_override);
        }
        if (metrics_port_override > 0) {
            config->set_metrics_port(metrics_port_override);
            spdlog::info("Metrics port overridden: {}", metrics_port_override);
        }
        if (!log_level_str.empty()) {
            if (log_level_str == "debug") config->set_log_level(spdlog::level::debug);
            else if (log_level_str == "info") config->set_log_level(spdlog::level::info);
            else if (log_level_str == "warn") config->set_log_level(spdlog::level::warn);
            else if (log_level_str == "error") config->set_log_level(spdlog::level::err);
            else {
                spdlog::error("Invalid log level: {}", log_level_str);
                return 1;
            }
        }
        
        // Configure logging level from config
        spdlog::set_level(config->get_log_level());
        
        // Log testing hooks if enabled
        if (testing_hooks.enabled) {
            spdlog::info("Testing hooks enabled:");
            if (testing_hooks.roi_inline_end > 0) {
                spdlog::info("  - ROI: inline({}-{}), xline({}-{}), z({}-{})",
                    testing_hooks.roi_inline_start, testing_hooks.roi_inline_end,
                    testing_hooks.roi_xline_start, testing_hooks.roi_xline_end,
                    testing_hooks.roi_z_start, testing_hooks.roi_z_end);
            }
            if (testing_hooks.enable_tile_dump) {
                spdlog::info("  - Tile dumping: {}", testing_hooks.dump_directory);
            }
            if (testing_hooks.enable_performance_profiling) {
                spdlog::info("  - Performance profiling enabled");
            }
            spdlog::info("  - Max test tiles: {}", testing_hooks.max_test_tiles);
        }
        
        // Log startup information
        spdlog::info("RemoteView Server starting with VDS file: {}", config->get_vds_path());
        spdlog::info("Server port: {}, Metrics port: {}", config->get_server_port(), config->get_metrics_port());
        
        // Create and configure the main server instance
        remoteview::Server server(config);
        
        // Configure testing hooks if enabled
        if (testing_hooks.enabled) {
            server.configure_testing_hooks(testing_hooks);
        }
        
        server.start();  // Blocks until server is terminated
        
        return 0;  // Clean shutdown
        
    } catch (const std::exception& e) {
        // Catch and log any startup errors
        spdlog::error("Failed to start server: {}", e.what());
        return 1;  // Error exit code
    }
}