#pragma once

#include "config/config.hpp"
#include "vds_metadata.hpp"
#include <unordered_set>
#include <mutex>
#include <atomic>

/**
 * HueSpace/OpenVDS API Headers
 * 
 * This includes the complete HueSpace SDK for reading VDS (Volume Data Store) files.
 * The VDS format is Bluware's proprietary seismic data format that provides:
 * - High-performance random access to 3D seismic volumes
 * - Compressed storage with multiple levels of detail (LOD)
 * - Metadata storage for coordinate systems, units, acquisition parameters
 * - Cloud and local storage backends
 * 
 * Header structure follows the reference StandAloneLoad.cpp example
 * to ensure compatibility with HueSpace SDK patterns.
 */
#include <HueSpace3/ProxyInterfaceFactory.h>
#include <HueSpace3/ProxyInterface.h>
#include <HueSpace3/ConfigMemoryManagement.h>
#include <HueSpace3/CacheConfigurer.h>
#include <HueSpace3/ConfigThreads.h>
#include <HueSpace3/VolumeData.h>
#include <HueSpace3/VolumeDataAccess.h>
#include <HueSpace3/BLOB.h>
#include <HueSpace3/KnownMetadata.h>
#include <HueSpace3/Workspace.h>
#include <HueSpace3/SceneManager.h>
#include <HueSpace3/Scene.h>
#include <HueSpace3/ProjectManager.h>
#include <HueSpace3/Project.h>
#include <HueSpace3/VDS.h>
#include <HueSpace3/VolumeData.h>
#include <HueSpace3/VolumeDataAccess.h>
#include <HueSpace3/VDSFilter.h>
#include <HueSpace3/VDSManager.h>
#include <HueSpace3/PluginManager.h>
#include <HueSpace3/RootObject.h>
#include <HueSpace3/PluginParameter.h>
#include <HueSpace3/PluginParameterInt.h>
#include <HueSpace3/VDSDirect.h>
#include <HueSpace3/VolumeDataAccessor.h>

#include <memory>
#include <vector>
#include <cstdint>
#include <mutex>
#include <chrono>

#ifdef CUDA_AVAILABLE
#include <cuda_runtime.h>
#endif

namespace remoteview {

struct TileRequest {
    uint32_t plane_index;      // 0=inline, 1=xline, 2=time/depth
    uint32_t slice_index;      // which slice along the plane
    uint32_t x_offset;
    uint32_t y_offset;
    uint32_t width;
    uint32_t height;
    uint32_t downsample_level;
    
    bool operator==(const TileRequest& other) const {
        return plane_index == other.plane_index &&
               slice_index == other.slice_index &&
               x_offset == other.x_offset &&
               y_offset == other.y_offset &&
               width == other.width &&
               height == other.height &&
               downsample_level == other.downsample_level;
    }
};

struct TileData {
    std::vector<uint8_t> data;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_sample;
    Hue::HueSpaceLib::DataBlock::Format format;
    std::chrono::steady_clock::time_point timestamp;
};

class VdsReader {
public:
    explicit VdsReader(std::shared_ptr<Config> config);
    ~VdsReader();
    
    void initialize();
    void shutdown();
    
    const VdsMetadata& get_metadata() const { return metadata_; }
    
    // Main tile reading function - follows HueSpace async pattern
    std::unique_ptr<TileData> read_tile(const TileRequest& request);
    
    /**
     * Prefetch cancellation system for slice changes
     */
    struct PrefetchContext {
        uint32_t current_slice_inline = 0;
        uint32_t current_slice_xline = 0; 
        uint32_t current_slice_z = 0;
        
        // Track active requests that should be cancelled on slice change
        std::unordered_set<int64_t> active_requests;
        std::mutex requests_mutex;
        
        // Atomic flag to signal cancellation to all active requests
        std::atomic<bool> cancel_prefetch{false};
        std::atomic<uint64_t> slice_change_generation{0}; // Increment on each slice change
    };
    
    /**
     * Cancel all pending prefetch requests for current slice
     * Called when user changes to a different slice
     */
    void cancel_prefetch_requests();
    
    /**
     * Update current slice indices and cancel obsolete prefetch
     * @param inline_idx New inline slice index
     * @param xline_idx New crossline slice index 
     * @param z_idx New time/depth slice index
     */
    void set_current_slice(uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx);
    
    /**
     * Check if a request should be cancelled due to slice change
     * @param generation Generation number when request started
     * @return true if request should be cancelled
     */
    bool should_cancel_request(uint64_t generation) const;
    
    bool is_valid_tile_request(const TileRequest& request) const;
    
    std::vector<uint32_t> get_available_downsample_levels() const;
    uint32_t get_optimal_tile_size(uint32_t requested_size) const;
    
    // Performance settings following StandAloneLoad patterns
    void set_compression_ratio(float ratio);
    void set_cuda_enabled(bool enabled);
    void set_cache_policy(bool immediate_timeout = true);

private:
    void initialize_huespace();
    void configure_memory_management();
    void load_vds_file();
    void extract_layout_info();
    void setup_request_buffers();
    void cleanup_buffers();
    
    // HueSpace objects following the established pattern
    Hue::ProxyLib::IProxyInterface* hue_proxy_interface_ = nullptr;
    Hue::ProxyLib::Project* project_ = nullptr;
    Hue::ProxyLib::VDS* vds_file_ = nullptr;
    const Hue::HueSpaceLib::VolumeDataLayout* vds_layout_ = nullptr;
    
    std::shared_ptr<Config> config_;
    VdsMetadata metadata_;
    std::mutex access_mutex_;
    bool initialized_ = false;
    bool cuda_available_ = false;
    
    // Prefetch cancellation context
    PrefetchContext prefetch_context_;
    
    // Request management following StandAloneLoad pattern
    static constexpr int MAX_REQUESTS_IN_FLIGHT = 2;
    struct RequestBuffer {
        void* buffer = nullptr;
        bool in_use = false;
        size_t size = 0;
    };
    
    std::vector<RequestBuffer> request_buffers_;
    int current_buffer_index_ = 0;
    
    // Performance timing (like StandAloneLoad Timer_c)
    struct RequestStats {
        size_t total_requests = 0;
        size_t total_bytes = 0;
        double total_time_ms = 0.0;
        
        double avg_throughput_mbps() const {
            return total_time_ms > 0 ? 
                   (total_bytes / (1024.0 * 1024.0)) / (total_time_ms / 1000.0) : 0.0;
        }
    };
    
    mutable RequestStats stats_;
};

} // namespace remoteview

namespace std {
    template<>
    struct hash<remoteview::TileRequest> {
        size_t operator()(const remoteview::TileRequest& req) const {
            return hash<uint32_t>()(req.plane_index) ^
                   (hash<uint32_t>()(req.slice_index) << 1) ^
                   (hash<uint32_t>()(req.x_offset) << 2) ^
                   (hash<uint32_t>()(req.y_offset) << 3) ^
                   (hash<uint32_t>()(req.width) << 4) ^
                   (hash<uint32_t>()(req.height) << 5) ^
                   (hash<uint32_t>()(req.downsample_level) << 6);
        }
    };
}