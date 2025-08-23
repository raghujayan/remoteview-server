/**
 * Adaptive Quality Control for Real-time Streaming
 * 
 * Implements automatic quality adjustment based on network conditions:
 * - RTT-based adaptation (ping/pong measurements)
 * - Buffer-based back-pressure detection
 * - Multi-stage degradation: tile size → data type → compression → downsample
 * - Hysteresis for stable recovery
 * 
 * Quality levels automatically adapt to maintain smooth streaming.
 */

#pragma once

#include "config/config.hpp"
#include "protocol/tile_message.hpp"
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>

namespace remoteview {

/**
 * Adaptive quality controller with congestion-based triggers
 * 
 * Monitors network performance and automatically adjusts streaming quality:
 * 
 * Degradation sequence when congestion detected:
 * 1. Tile size: 256×256 → 192×192 → 128×128
 * 2. Data type: u16 → u8 (reduces bandwidth by 50%)
 * 3. Compression: LZ4 → Zstd-1 (better ratio, slight CPU cost)
 * 4. Downsample: Level 0 → 1 → 2 (quarter resolution)
 * 
 * Recovery happens in reverse order with hysteresis.
 */
class AdaptiveQualityController {
public:
    /**
     * Current quality settings for tile generation
     */
    struct QualitySettings {
        uint16_t tile_size = 256;                           // Tile dimensions (256×256, 192×192, 128×128)
        protocol::DataType data_type = protocol::DataType::U16;  // u16/u8 precision
        protocol::CompressionType compression = protocol::CompressionType::LZ4; // LZ4/Zstd
        uint32_t downsample_level = 0;                      // LOD level (0=full, 1=half, 2=quarter)
        
        /**
         * Calculate effective bandwidth multiplier vs baseline
         * @return Bandwidth factor (1.0 = baseline, 0.25 = quarter bandwidth)
         */
        double bandwidth_factor() const {
            double type_factor = (data_type == protocol::DataType::U8) ? 0.5 : 1.0;
            double tile_factor = (tile_size * tile_size) / (256.0 * 256.0);
            double downsample_factor = 1.0 / (1 << (downsample_level * 2)); // 4^level reduction
            return type_factor * tile_factor * downsample_factor;
        }
    };
    
    /**
     * Network performance metrics for adaptation decisions
     */
    struct NetworkMetrics {
        uint32_t rtt_ms = 0;                    // Round-trip time from ping/pong
        size_t buffered_bytes = 0;              // DataChannel bufferedAmount
        double packet_loss_rate = 0.0;          // Estimated loss rate from retransmissions
        uint64_t bytes_per_second = 0;          // Throughput estimate
        std::chrono::steady_clock::time_point last_update;
    };

private:
    std::shared_ptr<Config> config_;
    
    // Current quality state
    QualitySettings current_quality_;
    mutable std::mutex quality_mutex_;
    
    // Network monitoring
    NetworkMetrics network_metrics_;
    std::atomic<bool> congestion_detected_{false};
    
    // Adaptation thresholds and hysteresis
    static constexpr uint32_t RTT_SPIKE_THRESHOLD_MS = 250;     // High latency trigger
    static constexpr uint32_t RTT_RECOVERY_THRESHOLD_MS = 100;  // Recovery threshold (hysteresis)
    static constexpr size_t BUFFER_CONGESTION_KB = 64;          // Buffer congestion trigger
    static constexpr size_t BUFFER_RECOVERY_KB = 32;            // Buffer recovery threshold
    
    // Timing for stable recovery
    std::chrono::steady_clock::time_point last_degradation_;
    std::chrono::steady_clock::time_point last_improvement_;
    static constexpr auto RECOVERY_DELAY = std::chrono::seconds(5);   // Wait before improving
    static constexpr auto DEGRADE_DELAY = std::chrono::seconds(1);    // Quick degradation

public:
    explicit AdaptiveQualityController(std::shared_ptr<Config> config);
    ~AdaptiveQualityController() = default;
    
    /**
     * Initialize adaptive controller with baseline quality
     */
    void initialize();
    
    /**
     * Update network performance metrics
     * @param rtt_ms Current round-trip time from ping/pong
     * @param buffered_bytes DataChannel bufferedAmount
     * @param throughput_bps Estimated throughput in bytes/second
     */
    void update_network_metrics(uint32_t rtt_ms, size_t buffered_bytes, uint64_t throughput_bps = 0);
    
    /**
     * Check if quality should be adapted based on current metrics
     * Call this periodically (e.g., every 100ms) to trigger adaptations
     * @return true if quality was changed
     */
    bool adapt_quality();
    
    /**
     * Get current quality settings for tile generation
     * @return Thread-safe copy of current quality
     */
    QualitySettings get_current_quality() const {
        std::lock_guard<std::mutex> lock(quality_mutex_);
        return current_quality_;
    }
    
    /**
     * Force quality to specific settings (for testing)
     * @param quality New quality settings
     */
    void set_quality(const QualitySettings& quality) {
        std::lock_guard<std::mutex> lock(quality_mutex_);
        current_quality_ = quality;
    }
    
    /**
     * Get current network metrics
     * @return Current network performance data
     */
    NetworkMetrics get_network_metrics() const {
        return network_metrics_;
    }
    
    /**
     * Check if currently in congestion state
     * @return true if congestion detected and quality should be reduced
     */
    bool is_congested() const {
        return congestion_detected_.load();
    }
    
    /**
     * Get adaptation statistics for monitoring
     */
    struct AdaptationStats {
        uint64_t degradations = 0;      // Number of quality reductions
        uint64_t improvements = 0;      // Number of quality increases
        uint64_t congestion_events = 0; // Congestion detection count
        double avg_bandwidth_factor = 1.0; // Average bandwidth usage
    };
    
    AdaptationStats get_stats() const;

private:
    // Internal adaptation logic
    bool should_degrade_quality() const;
    bool should_improve_quality() const;
    void degrade_quality();
    void improve_quality();
    
    // Adaptation statistics
    mutable std::mutex stats_mutex_;
    AdaptationStats stats_;
    
    // Quality level tracking
    uint8_t current_degradation_level_ = 0;  // 0=best, higher=more degraded
    static constexpr uint8_t MAX_DEGRADATION_LEVEL = 8;
};

// Legacy class for compatibility
class Adapt {
public:
    explicit Adapt(std::shared_ptr<Config> config) 
        : controller_(std::make_unique<AdaptiveQualityController>(config)) {}
    ~Adapt() = default;
    
    void initialize() { controller_->initialize(); }
    void shutdown() {}
    
    AdaptiveQualityController* get_controller() { return controller_.get(); }

private:
    std::unique_ptr<AdaptiveQualityController> controller_;
};

} // namespace remoteview