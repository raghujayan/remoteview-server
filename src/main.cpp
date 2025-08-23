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

/**
 * Main application entry point
 * 
 * Command line usage:
 *   remoteview_server [config_file.json]
 * 
 * If no config file is provided, uses default configuration with:
 * - Default VDS file path
 * - Default WebSocket port (8080)
 * - Default cache limits
 * 
 * The server runs indefinitely until terminated with Ctrl+C or SIGTERM.
 * 
 * @param argc Command line argument count
 * @param argv Command line arguments (optional config file path)
 * @return 0 on success, 1 on error
 */
int main(int argc, char* argv[]) {
    try {
        // CRITICAL: Set HueSpace license environment variable
        // This must be set before any HueSpace API calls or the server will fail
        // The license server validates access to HueSpace/OpenVDS functionality
        setenv("HUE_LICENSE_FILE", "5053@license.cloud.bluware.com", 1);
        
        // Load configuration from file or use defaults
        auto config = std::make_shared<remoteview::Config>();
        if (argc > 1) {
            // Load configuration from JSON file specified on command line
            config->load_from_file(argv[1]);
            spdlog::info("Loaded configuration from: {}", argv[1]);
        } else {
            // Use built-in default configuration
            config->load_default();
            spdlog::info("Using default configuration");
        }
        
        // Configure logging level from config (DEBUG, INFO, WARN, ERROR)
        spdlog::set_level(config->get_log_level());
        
        // Log startup information for debugging/monitoring
        spdlog::info("RemoteView Server starting with VDS file: {}", 
                     config->get_vds_path());
        
        // Create and start the main server instance
        // This initializes all components: VDS reader, cache, WebSocket, WebRTC, etc.
        remoteview::Server server(config);
        server.start();  // Blocks until server is terminated
        
        return 0;  // Clean shutdown
        
    } catch (const std::exception& e) {
        // Catch and log any startup errors
        // Common failures: VDS file not found, license issues, port conflicts
        spdlog::error("Failed to start server: {}", e.what());
        return 1;  // Error exit code
    }
}