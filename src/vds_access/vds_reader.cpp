#include "vds_reader.hpp"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <thread>

#ifdef CUDA_AVAILABLE
#include <cuda_runtime.h>
#endif

namespace remoteview {

VdsReader::VdsReader(std::shared_ptr<Config> config)
    : config_(std::move(config)) {
    request_buffers_.resize(MAX_REQUESTS_IN_FLIGHT);
}

VdsReader::~VdsReader() {
    shutdown();
}

void VdsReader::initialize() {
    std::lock_guard<std::mutex> lock(access_mutex_);
    
    if (initialized_) {
        spdlog::warn("VdsReader already initialized");
        return;
    }
    
    const auto& vds_path = config_->get_vds_path();
    spdlog::info("Initializing VDS reader for file: {}", vds_path);
    
    try {
        initialize_huespace();
        configure_memory_management();
        load_vds_file();
        extract_layout_info();
        setup_request_buffers();
        
        initialized_ = true;
        spdlog::info("VDS reader initialized successfully");
        spdlog::info("CUDA support: {}", cuda_available_ ? "enabled" : "disabled");
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize VDS reader: {}", e.what());
        shutdown();
        throw;
    }
}

void VdsReader::shutdown() {
    std::lock_guard<std::mutex> lock(access_mutex_);
    
    if (!initialized_) {
        return;
    }
    
    cleanup_buffers();
    
    // Release HueSpace objects in reverse order (following StandAloneLoad pattern)
    vds_layout_ = nullptr;
    vds_file_ = nullptr;
    project_ = nullptr;
    
    if (hue_proxy_interface_) {
        hue_proxy_interface_->Release();
        hue_proxy_interface_ = nullptr;
    }
    
    initialized_ = false;
    spdlog::info("VDS reader shutdown complete");
}

void VdsReader::initialize_huespace() {
    // Follow exact pattern from StandAloneLoad.cpp
    hue_proxy_interface_ = Hue::ProxyLib::ProxyInterfaceFactory::CreateProxyInterface();
    project_ = Hue::ProxyLib::Workspace::Instance()->Scenes().Create()->Projects().Create();
    
    if (!hue_proxy_interface_ || !project_) {
        throw std::runtime_error("Failed to initialize HueSpace proxy interface or project");
    }
}

void VdsReader::configure_memory_management() {
    // Follow StandAloneLoad.cpp memory management configuration
    Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetRegisterCUDACallback(false);
    Hue::ProxyLib::ConfigMemoryManagement::Instance()->CacheConfigurer()->SetEnabled(false);
    Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetProcessingGPUCacheMax0(4096);
    Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetProcessingCPUCacheMax(512);
    
    // Check CUDA availability
    cuda_available_ = Hue::ProxyLib::ConfigMemoryManagement::Instance()->CUDASupported();
    
    // Configure threading based on CUDA availability
    if (!cuda_available_) {
        // Not CUDA, use 4 CPU worker threads (from StandAloneLoad pattern)
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetEnableProcessingThread0(true);
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetEnableProcessingThread1(true);
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetEnableProcessingThread2(true);
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetEnableProcessingThread3(true);
    }
}

void VdsReader::load_vds_file() {
    const auto& vds_path = config_->get_vds_path();
    
    // Follow exact pattern from StandAloneLoad.cpp
    vds_file_ = project_->VDSs().RestoreVDSFromFileName(vds_path);
    
    if (!vds_file_) {
        throw std::runtime_error("Failed to load VDS file: " + vds_path);
    }
    
    // Set cache policy for single-read optimization (from StandAloneLoad)
    set_cache_policy(true);
    
    spdlog::info("VDS file loaded: {}", vds_path);
}

void VdsReader::extract_layout_info() {
    // Get thread-safe VolumeDataLayout (from StandAloneLoad comment)
    vds_layout_ = Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->GetVolumeDataLayout(*vds_file_->GetHueObj());
    
    if (!vds_layout_) {
        throw std::runtime_error("Failed to get VolumeDataLayout from VDS file");
    }
    
    // Extract metadata using the layout
    metadata_ = VdsMetadataExtractor::extract_from_layout(vds_layout_);
    
    spdlog::info("VDS layout extracted: {}x{}x{} samples", 
                 metadata_.inline_size, 
                 metadata_.crossline_size, 
                 metadata_.time_samples);
}

void VdsReader::setup_request_buffers() {
    // Calculate maximum tile buffer size
    const size_t max_tile_size = config_->get_default_tile_size();
    const size_t max_buffer_size = max_tile_size * max_tile_size * 4; // 4 bytes per sample (f32)
    
    for (int i = 0; i < MAX_REQUESTS_IN_FLIGHT; ++i) {
        auto& buffer = request_buffers_[i];
        buffer.size = max_buffer_size;
        
#ifdef CUDA_AVAILABLE
        if (cuda_available_) {
            // Allocate CUDA pinned memory (from StandAloneLoad pattern)
            cudaError_t cuda_result = cudaHostAlloc(&buffer.buffer, buffer.size, 
                                                   cudaHostAllocPortable | cudaHostAllocMapped);
            if (cuda_result != cudaSuccess) {
                spdlog::warn("Failed to allocate CUDA pinned memory, falling back to regular malloc");
                buffer.buffer = malloc(buffer.size);
            }
        } else
#endif
        {
            buffer.buffer = malloc(buffer.size);
        }
        
        if (!buffer.buffer) {
            throw std::runtime_error("Failed to allocate request buffer");
        }
        
        buffer.in_use = false;
    }
    
    spdlog::debug("Allocated {} request buffers of {} KB each", 
                  MAX_REQUESTS_IN_FLIGHT, max_buffer_size / 1024);
}

void VdsReader::cleanup_buffers() {
    for (auto& buffer : request_buffers_) {
        if (buffer.buffer) {
#ifdef CUDA_AVAILABLE
            if (cuda_available_) {
                cudaFreeHost(buffer.buffer);
            } else
#endif
            {
                free(buffer.buffer);
            }
            buffer.buffer = nullptr;
        }
        buffer.in_use = false;
        buffer.size = 0;
    }
}

std::unique_ptr<TileData> VdsReader::read_tile(const TileRequest& request) {
    std::lock_guard<std::mutex> lock(access_mutex_);
    
    if (!initialized_) {
        throw std::runtime_error("VDS reader not initialized");
    }
    
    if (!is_valid_tile_request(request)) {
        throw std::invalid_argument("Invalid tile request parameters");
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        auto tile_data = std::make_unique<TileData>();
        
        // Get request buffer
        auto& buffer = request_buffers_[current_buffer_index_ % MAX_REQUESTS_IN_FLIGHT];
        current_buffer_index_++;
        
        // Calculate tile dimensions and format
        uint32_t downsample_factor = 1 << request.downsample_level;
        tile_data->width = request.width / downsample_factor;
        tile_data->height = request.height / downsample_factor;
        
        // Determine format based on config (following StandAloneLoad sample size pattern)
        Hue::HueSpaceLib::DataBlock::Format format;
        uint32_t bytes_per_sample;
        
        // Map from config compression algorithm hint to sample format
        const auto& compression_alg = config_->get_compression_algorithm();
        if (compression_alg == "u8" || tile_data->width * tile_data->height > 65536) {
            format = Hue::HueSpaceLib::DataBlock::Format_U8;
            bytes_per_sample = 1;
        } else if (compression_alg == "f32") {
            format = Hue::HueSpaceLib::DataBlock::Format_R32;
            bytes_per_sample = 4;
        } else {
            format = Hue::HueSpaceLib::DataBlock::Format_U16; // Default
            bytes_per_sample = 2;
        }
        
        tile_data->format = format;
        tile_data->bytes_per_sample = bytes_per_sample;
        
        // Calculate buffer requirements
        size_t required_size = tile_data->width * tile_data->height * bytes_per_sample;
        if (required_size > buffer.size) {
            throw std::runtime_error("Tile too large for allocated buffer");
        }
        
        // Set up coordinate ranges following StandAloneLoad pattern
        int startRead[Hue::HueSpaceLib::VolumeDataLayout::Dimensionality_Max] = {0,0,0,0,0,0};
        int endRead[Hue::HueSpaceLib::VolumeDataLayout::Dimensionality_Max] = {0,0,0,0,0,0};
        
        // Map tile request to HueSpace coordinates based on plane
        if (request.plane_index == 0) { // Inline slice
            startRead[0] = request.slice_index;
            endRead[0] = request.slice_index + 1;
            startRead[1] = request.x_offset / downsample_factor;
            endRead[1] = startRead[1] + tile_data->width;
            startRead[2] = request.y_offset / downsample_factor;  
            endRead[2] = startRead[2] + tile_data->height;
        } else if (request.plane_index == 1) { // Crossline slice
            startRead[0] = request.x_offset / downsample_factor;
            endRead[0] = startRead[0] + tile_data->width;
            startRead[1] = request.slice_index;
            endRead[1] = request.slice_index + 1;
            startRead[2] = request.y_offset / downsample_factor;
            endRead[2] = startRead[2] + tile_data->height;
        } else if (request.plane_index == 2) { // Time/depth slice  
            startRead[0] = request.x_offset / downsample_factor;
            endRead[0] = startRead[0] + tile_data->width;
            startRead[1] = request.y_offset / downsample_factor;
            endRead[1] = startRead[1] + tile_data->height;
            startRead[2] = request.slice_index;
            endRead[2] = request.slice_index + 1;
        }
        
        // Set remaining dimensions to 1 (from StandAloneLoad pattern)
        for (int i = 3; i < Hue::HueSpaceLib::VolumeDataLayout::Dimensionality_Max; ++i) {
            startRead[i] = 0;
            endRead[i] = 1;
        }
        
        // Capture generation number before starting request to detect slice changes
        uint64_t request_generation = prefetch_context_.slice_change_generation.load();
        
        // Check if we should cancel before starting the expensive operation
        if (should_cancel_request(request_generation)) {
            spdlog::debug("Request cancelled before starting due to slice change");
            return nullptr; // Request cancelled
        }
        
        // Make async request following StandAloneLoad pattern
        Hue::ProxyLib::int64 requestID;
        
        // Use optimal dimension group for time slices (from StandAloneLoad optimization)
        if (request.plane_index == 2 && tile_data->width == 1 && tile_data->height == 1) {
            requestID = Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->RequestVolumeSubset(
                buffer.buffer, vds_layout_, Hue::HueSpaceLib::DimensionGroup12, 
                request.downsample_level, 0, startRead, endRead, format);
        } else {
            requestID = Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->RequestVolumeSubset(
                buffer.buffer, vds_layout_, Hue::HueSpaceLib::DimensionGroup012,
                request.downsample_level, 0, startRead, endRead, format);
        }
        
        // Track active request for potential cancellation
        {
            std::lock_guard<std::mutex> lock(prefetch_context_.requests_mutex);
            prefetch_context_.active_requests.insert(requestID);
        }
        
        // For now, we'll use blocking wait and check cancellation before/after
        // TODO: Investigate if HueSpace API provides non-blocking completion check
        // or request cancellation mechanisms in future versions
        
        // Check cancellation once more before blocking wait
        if (should_cancel_request(request_generation)) {
            spdlog::debug("Request {} cancelled before wait due to slice change", requestID);
            {
                std::lock_guard<std::mutex> lock(prefetch_context_.requests_mutex);
                prefetch_context_.active_requests.erase(requestID);
            }
            return nullptr; // Request cancelled
        }
        
        // Block until completion (no cancellation during wait for now)
        bool success = Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->WaitForCompletion(requestID);
        
        // Remove from active requests
        {
            std::lock_guard<std::mutex> lock(prefetch_context_.requests_mutex);
            prefetch_context_.active_requests.erase(requestID);
        }
        
        if (!success) {
            throw std::runtime_error("VDS volume subset request failed");
        }
        
        // Final cancellation check before expensive memcpy
        if (should_cancel_request(request_generation)) {
            spdlog::debug("Request cancelled after completion but before data copy");
            return nullptr; // Request cancelled
        }
        
        // Copy data from buffer to tile data
        tile_data->data.resize(required_size);
        memcpy(tile_data->data.data(), buffer.buffer, required_size);
        tile_data->timestamp = std::chrono::steady_clock::now();
        
        // Update statistics
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        
        stats_.total_requests++;
        stats_.total_bytes += required_size;
        stats_.total_time_ms += duration.count() / 1000.0;
        
        spdlog::debug("Read tile {}x{} from plane {} slice {} in {:.2f}ms",
                     tile_data->width, tile_data->height, request.plane_index, 
                     request.slice_index, duration.count() / 1000.0);
        
        return tile_data;
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to read tile: {}", e.what());
        throw;
    }
}

bool VdsReader::is_valid_tile_request(const TileRequest& request) const {
    if (!initialized_ || !vds_layout_) {
        return false;
    }
    
    // Check plane bounds
    if (request.plane_index > 2) {
        return false;
    }
    
    // Check slice index bounds based on plane
    uint32_t max_slice;
    if (request.plane_index == 0) {
        max_slice = metadata_.inline_size;
    } else if (request.plane_index == 1) {
        max_slice = metadata_.crossline_size;
    } else {
        max_slice = metadata_.time_samples;
    }
    
    if (request.slice_index >= max_slice) {
        return false;
    }
    
    // Check spatial bounds
    if (request.x_offset >= metadata_.inline_size ||
        request.y_offset >= metadata_.crossline_size) {
        return false;
    }
    
    // Check downsample level
    if (request.downsample_level > metadata_.max_downsample_level) {
        return false;
    }
    
    return true;
}

std::vector<uint32_t> VdsReader::get_available_downsample_levels() const {
    std::vector<uint32_t> levels;
    for (uint32_t i = 0; i <= metadata_.max_downsample_level; ++i) {
        levels.push_back(i);
    }
    return levels;
}

uint32_t VdsReader::get_optimal_tile_size(uint32_t requested_size) const {
    // Round to nearest power of 2 for optimal VDS access
    uint32_t power = static_cast<uint32_t>(std::round(std::log2(requested_size)));
    uint32_t optimal = 1 << power;
    
    // Clamp to reasonable bounds
    constexpr uint32_t MIN_TILE_SIZE = 64;
    constexpr uint32_t MAX_TILE_SIZE = 1024;
    
    return std::clamp(optimal, MIN_TILE_SIZE, MAX_TILE_SIZE);
}

void VdsReader::set_compression_ratio(float ratio) {
    if (!vds_file_) return;
    
    // Follow StandAloneLoad compression setting pattern
    if (ratio > 1.0f) {
        spdlog::info("Setting VDS compression ratio to {}", ratio);
        vds_file_->SetWaveletAdaptiveMode(Hue::ProxyLib::WaveletAdaptiveModeInclDefault(
            static_cast<int>(Hue::ProxyLib::WaveletAdaptiveModeInclDefault::Ratio)));
        vds_file_->SetWaveletAdaptiveRatio(ratio);
    }
}

void VdsReader::set_cuda_enabled(bool enabled) {
    if (!enabled && cuda_available_) {
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetDisableCUDASupport(true);
        cuda_available_ = false;
        spdlog::info("CUDA support disabled by request");
    }
}

void VdsReader::set_cache_policy(bool immediate_timeout) {
    if (!vds_file_) return;
    
    // Follow StandAloneLoad cache policy pattern
    if (immediate_timeout) {
        vds_file_->SetCachePolicy(Hue::ProxyLib::VDSCachePolicy::TimeoutImmediately);
    }
}

// Prefetch cancellation implementation
void VdsReader::cancel_prefetch_requests() {
    std::lock_guard<std::mutex> lock(prefetch_context_.requests_mutex);
    
    spdlog::info("Cancelling {} active prefetch requests due to slice change", 
                prefetch_context_.active_requests.size());
    
    // Signal all active requests to cancel
    prefetch_context_.cancel_prefetch = true;
    
    // TODO: If HueSpace API supports request cancellation, cancel individual requests
    // For now we rely on early-exit checks in the reading loop
    for (auto request_id : prefetch_context_.active_requests) {
        spdlog::debug("Marking request {} for cancellation", request_id);
        // Future: Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->CancelRequest(request_id);
    }
    
    prefetch_context_.active_requests.clear();
    prefetch_context_.slice_change_generation++;
}

void VdsReader::set_current_slice(uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx) {
    std::lock_guard<std::mutex> lock(prefetch_context_.requests_mutex);
    
    bool slice_changed = (prefetch_context_.current_slice_inline != inline_idx ||
                         prefetch_context_.current_slice_xline != xline_idx ||
                         prefetch_context_.current_slice_z != z_idx);
    
    if (slice_changed) {
        spdlog::info("Slice changed from ({},{},{}) to ({},{},{}) - cancelling prefetch",
                    prefetch_context_.current_slice_inline, prefetch_context_.current_slice_xline, 
                    prefetch_context_.current_slice_z, inline_idx, xline_idx, z_idx);
        
        // Update slice indices
        prefetch_context_.current_slice_inline = inline_idx;
        prefetch_context_.current_slice_xline = xline_idx; 
        prefetch_context_.current_slice_z = z_idx;
        
        // Cancel all active prefetch requests for the old slice
        if (!prefetch_context_.active_requests.empty()) {
            spdlog::debug("Cancelling {} active requests", prefetch_context_.active_requests.size());
            prefetch_context_.cancel_prefetch = true;
            prefetch_context_.active_requests.clear();
            prefetch_context_.slice_change_generation++;
        }
        
        // Reset cancellation flag for new slice
        prefetch_context_.cancel_prefetch = false;
    }
}

bool VdsReader::should_cancel_request(uint64_t generation) const {
    return prefetch_context_.cancel_prefetch.load() || 
           generation < prefetch_context_.slice_change_generation.load();
}

} // namespace remoteview