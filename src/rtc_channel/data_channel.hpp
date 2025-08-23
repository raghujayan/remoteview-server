/**
 * DataChannel Back-pressure Management
 * 
 * Implements explicit back-pressure control for WebRTC DataChannels:
 * - Unordered delivery with no retransmissions (maxRetransmits: 0)
 * - Per-plane ring buffers with depth <= 2
 * - Drop-oldest policy on overflow
 * - Comprehensive metrics for congestion monitoring
 * 
 * This ensures smooth real-time streaming without blocking on network congestion.
 */

#pragma once

#include "protocol/tile_message.hpp" 
#include <memory>
#include <queue>
#include <mutex>
#include <atomic>
#include <chrono>

namespace remoteview {

/**
 * Per-plane tile queue with back-pressure management
 * 
 * Each seismic plane (inline, crossline, time/depth) gets its own queue
 * to prevent one slow plane from blocking others.
 */
class PlaneQueue {
public:
    static constexpr size_t MAX_DEPTH = 2;  // Ring buffer depth limit
    
    struct QueueStats {
        uint64_t enqueued{0};     // Total tiles enqueued
        uint64_t dropped{0};      // Tiles dropped due to overflow
        uint64_t sent{0};         // Tiles successfully sent
        size_t current_depth{0};  // Current queue depth
        size_t max_depth_seen{0}; // Peak queue depth
        
        double drop_rate() const {
            return enqueued > 0 ? static_cast<double>(dropped) / enqueued : 0.0;
        }
    };

private:
    mutable std::mutex queue_mutex_;
    std::queue<std::unique_ptr<protocol::TileMessage>> tile_queue_;
    
    // Thread-safe statistics - use atomics for lock-free access
    std::atomic<uint64_t> enqueued_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> sent_{0};
    std::atomic<size_t> current_depth_{0};
    std::atomic<size_t> max_depth_seen_{0};
    
    protocol::PlaneType plane_type_;

public:
    explicit PlaneQueue(protocol::PlaneType plane) : plane_type_(plane) {}
    
    /**
     * Enqueue tile with drop-oldest policy
     * @param tile Tile to enqueue (ownership transferred)
     * @return true if enqueued, false if dropped
     */
    bool enqueue(std::unique_ptr<protocol::TileMessage> tile);
    
    /**
     * Dequeue next tile for transmission
     * @return Next tile or nullptr if queue empty
     */
    std::unique_ptr<protocol::TileMessage> dequeue();
    
    /**
     * Get current queue statistics (thread-safe snapshot)
     * @return Current statistics
     */
    QueueStats get_stats() const;
    
    /**
     * Clear all queued tiles (used on slice changes)
     */
    void clear();
    
    /**
     * Check if queue is at capacity
     * @return true if queue is full
     */
    bool is_full() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return tile_queue_.size() >= MAX_DEPTH;
    }
    
    /**
     * Get current queue depth
     * @return Number of tiles in queue
     */
    size_t depth() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return tile_queue_.size();
    }
};

/**
 * DataChannel manager with back-pressure control
 * 
 * Manages WebRTC DataChannel configuration and tile delivery:
 * - Ensures unordered: true, maxRetransmits: 0 configuration
 * - Maintains per-plane queues with drop-oldest policy
 * - Tracks bufferedAmount and congestion state
 * - Provides metrics for adaptive quality decisions
 */
class DataChannelManager {
public:
    struct ChannelConfig {
        bool unordered = true;           // Enable unordered delivery
        uint16_t max_retransmits = 0;    // No retransmissions for real-time
        size_t max_buffered = 64 * 1024; // 64KB buffer limit per channel
    };
    
    struct CongestionMetrics {
        std::atomic<size_t> buffered_bytes{0};        // Current buffered amount
        std::atomic<uint64_t> bytes_sent{0};          // Total bytes transmitted
        std::atomic<uint64_t> congestion_events{0};   // Times we hit buffer limits
        std::atomic<uint32_t> rtt_ms{0};             // Round-trip time from ping/pong
        std::chrono::steady_clock::time_point last_rtt_update;
    };
    
    // Snapshot struct with regular members (can be copied)
    struct CongestionSnapshot {
        size_t buffered_bytes = 0;        // Current buffered amount
        uint64_t bytes_sent = 0;          // Total bytes transmitted
        uint64_t congestion_events = 0;   // Times we hit buffer limits
        uint32_t rtt_ms = 0;             // Round-trip time from ping/pong
        std::chrono::steady_clock::time_point last_rtt_update;
    };

private:
    ChannelConfig config_;
    CongestionMetrics metrics_;
    
    // Per-plane queues for back-pressure management
    std::array<std::unique_ptr<PlaneQueue>, 3> plane_queues_;
    
    // Channel state
    bool channel_ready_ = false;
    std::mutex state_mutex_;

public:
    DataChannelManager();
    ~DataChannelManager() = default;
    
    /**
     * Configure DataChannel with proper back-pressure settings
     * Call this during WebRTC setup to ensure correct channel configuration
     */
    ChannelConfig get_channel_config() const { return config_; }
    
    /**
     * Enqueue tile for transmission with back-pressure
     * @param tile Tile to send (ownership transferred)
     * @return true if queued, false if dropped due to congestion
     */
    bool send_tile(std::unique_ptr<protocol::TileMessage> tile);
    
    /**
     * Process outbound queue and return next tile to transmit
     * @return Next tile or nullptr if no tiles ready
     */
    std::unique_ptr<protocol::TileMessage> get_next_tile();
    
    /**
     * Update network congestion metrics
     * @param buffered_bytes Current DataChannel bufferedAmount
     * @param rtt_ms Round-trip time from ping/pong
     */
    void update_congestion_metrics(size_t buffered_bytes, uint32_t rtt_ms);
    
    /**
     * Check if channel is experiencing congestion
     * @return true if back-pressure should be applied
     */
    bool is_congested() const;
    
    /**
     * Get comprehensive metrics for monitoring
     * @return All queue and congestion statistics
     */
    struct AllMetrics {
        CongestionSnapshot congestion;
        std::array<PlaneQueue::QueueStats, 3> plane_stats;
    };
    
    AllMetrics get_all_metrics() const;
    
    /**
     * Clear all queues (used on slice changes)
     */
    void clear_all_queues();
    
    /**
     * Mark channel as ready/not ready
     */
    void set_channel_ready(bool ready) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        channel_ready_ = ready;
    }
    
    bool is_channel_ready() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(state_mutex_));
        return channel_ready_;
    }
};

} // namespace remoteview