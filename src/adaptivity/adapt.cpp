#include "adapt.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace remoteview {

AdaptiveQualityController::AdaptiveQualityController(std::shared_ptr<Config> config) 
    : config_(std::move(config)) {
    // Initialize timestamps
    auto now = std::chrono::steady_clock::now();
    last_degradation_ = now;
    last_improvement_ = now;
    network_metrics_.last_update = now;
}

void AdaptiveQualityController::initialize() {
    // Start with best quality settings
    current_quality_.tile_size = 256;
    current_quality_.data_type = protocol::DataType::U16;
    current_quality_.compression = protocol::CompressionType::LZ4;
    current_quality_.downsample_level = 0;
    current_degradation_level_ = 0;
    
    spdlog::info("Adaptive quality controller initialized:");
    spdlog::info("  - Initial tile size: {}×{}", current_quality_.tile_size, current_quality_.tile_size);
    spdlog::info("  - Initial data type: u16");
    spdlog::info("  - Initial compression: LZ4");
    spdlog::info("  - Initial LOD: {}", current_quality_.downsample_level);
    spdlog::info("  - RTT thresholds: {}ms spike, {}ms recovery", 
                 RTT_SPIKE_THRESHOLD_MS, RTT_RECOVERY_THRESHOLD_MS);
    spdlog::info("  - Buffer thresholds: {}KB congestion, {}KB recovery",
                 BUFFER_CONGESTION_KB, BUFFER_RECOVERY_KB);
}

void AdaptiveQualityController::update_network_metrics(uint32_t rtt_ms, size_t buffered_bytes, uint64_t throughput_bps) {
    network_metrics_.rtt_ms = rtt_ms;
    network_metrics_.buffered_bytes = buffered_bytes;
    network_metrics_.bytes_per_second = throughput_bps;
    network_metrics_.last_update = std::chrono::steady_clock::now();
    
    // Update congestion detection
    bool was_congested = congestion_detected_.load();
    bool is_congested_now = (rtt_ms > RTT_SPIKE_THRESHOLD_MS) || 
                           (buffered_bytes > BUFFER_CONGESTION_KB * 1024);
    
    congestion_detected_ = is_congested_now;
    
    if (is_congested_now && !was_congested) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.congestion_events++;
        spdlog::debug("Congestion detected: RTT={}ms, buffered={}KB", rtt_ms, buffered_bytes / 1024);
    }
}

bool AdaptiveQualityController::adapt_quality() {
    auto now = std::chrono::steady_clock::now();
    bool quality_changed = false;
    
    if (should_degrade_quality()) {
        // Check if enough time has passed since last degradation
        if (now - last_degradation_ >= DEGRADE_DELAY) {
            degrade_quality();
            last_degradation_ = now;
            quality_changed = true;
            
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.degradations++;
        }
    } else if (should_improve_quality()) {
        // Check if enough time has passed since last improvement (hysteresis)
        if (now - last_improvement_ >= RECOVERY_DELAY && 
            now - last_degradation_ >= RECOVERY_DELAY) {
            improve_quality();
            last_improvement_ = now;
            quality_changed = true;
            
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.improvements++;
        }
    }
    
    return quality_changed;
}

bool AdaptiveQualityController::should_degrade_quality() const {
    // Don't degrade if already at worst quality
    if (current_degradation_level_ >= MAX_DEGRADATION_LEVEL) {
        return false;
    }
    
    // Degrade if congestion detected
    return congestion_detected_.load();
}

bool AdaptiveQualityController::should_improve_quality() const {
    // Can't improve if already at best quality
    if (current_degradation_level_ == 0) {
        return false;
    }
    
    // Only improve if no congestion and network is stable
    if (congestion_detected_.load()) {
        return false;
    }
    
    // Check for stable good conditions
    uint32_t rtt = network_metrics_.rtt_ms;
    size_t buffered = network_metrics_.buffered_bytes;
    
    return (rtt < RTT_RECOVERY_THRESHOLD_MS) && 
           (buffered < BUFFER_RECOVERY_KB * 1024);
}

void AdaptiveQualityController::degrade_quality() {
    std::lock_guard<std::mutex> lock(quality_mutex_);
    
    // Multi-stage degradation sequence:
    switch (current_degradation_level_) {
        case 0: // Reduce tile size: 256 → 192
            current_quality_.tile_size = 192;
            spdlog::info("Quality degraded: tile size 256→192");
            break;
            
        case 1: // Further reduce tile size: 192 → 128  
            current_quality_.tile_size = 128;
            spdlog::info("Quality degraded: tile size 192→128");
            break;
            
        case 2: // Switch to u8 data type (50% bandwidth reduction)
            current_quality_.data_type = protocol::DataType::U8;
            spdlog::info("Quality degraded: data type u16→u8");
            break;
            
        case 3: // Switch to Zstd compression (better ratio)
            current_quality_.compression = protocol::CompressionType::Zstd;
            spdlog::info("Quality degraded: compression LZ4→Zstd");
            break;
            
        case 4: // First downsample level (quarter pixels)
            current_quality_.downsample_level = 1;
            spdlog::info("Quality degraded: downsample 0→1 (half resolution)");
            break;
            
        case 5: // Second downsample level (sixteenth pixels)  
            current_quality_.downsample_level = 2;
            spdlog::info("Quality degraded: downsample 1→2 (quarter resolution)");
            break;
            
        case 6: // Smallest tile size with downsample
            current_quality_.tile_size = 64;
            spdlog::info("Quality degraded: tile size 128→64");
            break;
            
        case 7: // Maximum degradation
            current_quality_.downsample_level = 3;
            spdlog::info("Quality degraded: downsample 2→3 (eighth resolution)");
            break;
            
        default:
            spdlog::warn("Already at maximum degradation level");
            return;
    }
    
    current_degradation_level_++;
    
    // Update average bandwidth factor for statistics
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_.avg_bandwidth_factor = 
        (stats_.avg_bandwidth_factor + current_quality_.bandwidth_factor()) / 2.0;
        
    spdlog::info("Quality degraded to level {}, bandwidth factor: {:.3f}", 
                 current_degradation_level_, current_quality_.bandwidth_factor());
}

void AdaptiveQualityController::improve_quality() {
    std::lock_guard<std::mutex> lock(quality_mutex_);
    
    if (current_degradation_level_ == 0) {
        return; // Already at best quality
    }
    
    // Reverse degradation sequence:
    switch (current_degradation_level_ - 1) {
        case 7: // Restore downsample level
            current_quality_.downsample_level = 2;
            spdlog::info("Quality improved: downsample 3→2");
            break;
            
        case 6: // Restore tile size  
            current_quality_.tile_size = 128;
            spdlog::info("Quality improved: tile size 64→128");
            break;
            
        case 5: // Reduce downsample level
            current_quality_.downsample_level = 1;
            spdlog::info("Quality improved: downsample 2→1");
            break;
            
        case 4: // Remove downsample
            current_quality_.downsample_level = 0;
            spdlog::info("Quality improved: downsample 1→0 (full resolution)");
            break;
            
        case 3: // Restore LZ4 compression
            current_quality_.compression = protocol::CompressionType::LZ4;
            spdlog::info("Quality improved: compression Zstd→LZ4");
            break;
            
        case 2: // Restore u16 data type
            current_quality_.data_type = protocol::DataType::U16;
            spdlog::info("Quality improved: data type u8→u16");
            break;
            
        case 1: // Restore tile size: 128 → 192
            current_quality_.tile_size = 192;
            spdlog::info("Quality improved: tile size 128→192");
            break;
            
        case 0: // Restore full tile size: 192 → 256
            current_quality_.tile_size = 256;
            spdlog::info("Quality improved: tile size 192→256 (baseline)");
            break;
    }
    
    current_degradation_level_--;
    
    // Update average bandwidth factor for statistics  
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_.avg_bandwidth_factor = 
        (stats_.avg_bandwidth_factor + current_quality_.bandwidth_factor()) / 2.0;
        
    spdlog::info("Quality improved to level {}, bandwidth factor: {:.3f}", 
                 current_degradation_level_, current_quality_.bandwidth_factor());
}

AdaptiveQualityController::AdaptationStats AdaptiveQualityController::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

} // namespace remoteview