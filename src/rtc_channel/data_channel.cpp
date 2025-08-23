#include "data_channel.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace remoteview {

bool PlaneQueue::enqueue(std::unique_ptr<protocol::TileMessage> tile) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    enqueued_++;
    
    // Drop-oldest policy: if queue is full, remove oldest tile
    if (tile_queue_.size() >= MAX_DEPTH) {
        tile_queue_.pop();  // Drop oldest
        dropped_++;
        spdlog::debug("Dropped oldest tile from plane {} queue (depth={})", 
                     static_cast<int>(plane_type_), MAX_DEPTH);
    }
    
    // Enqueue new tile
    tile_queue_.push(std::move(tile));
    
    // Update statistics
    size_t current_depth = tile_queue_.size();
    current_depth_ = current_depth;
    
    // Update max depth seen (atomic compare-and-swap)
    size_t current_max = max_depth_seen_.load();
    while (current_depth > current_max && 
           !max_depth_seen_.compare_exchange_weak(current_max, current_depth)) {
        // Keep trying until we successfully update or someone else sets a higher value
    }
    
    return true;  // Always succeeds (drop-oldest ensures space)
}

std::unique_ptr<protocol::TileMessage> PlaneQueue::dequeue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    if (tile_queue_.empty()) {
        return nullptr;
    }
    
    auto tile = std::move(tile_queue_.front());
    tile_queue_.pop();
    
    sent_++;
    current_depth_ = tile_queue_.size();
    
    return tile;
}

PlaneQueue::QueueStats PlaneQueue::get_stats() const {
    QueueStats stats;
    stats.enqueued = enqueued_.load();
    stats.dropped = dropped_.load(); 
    stats.sent = sent_.load();
    stats.current_depth = current_depth_.load();
    stats.max_depth_seen = max_depth_seen_.load();
    return stats;
}

void PlaneQueue::clear() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    size_t cleared_count = tile_queue_.size();
    while (!tile_queue_.empty()) {
        tile_queue_.pop();
    }
    
    current_depth_ = 0;
    
    if (cleared_count > 0) {
        spdlog::debug("Cleared {} tiles from plane {} queue", 
                     cleared_count, static_cast<int>(plane_type_));
    }
}

DataChannelManager::DataChannelManager() {
    // Initialize per-plane queues
    plane_queues_[0] = std::make_unique<PlaneQueue>(protocol::PlaneType::Inline);
    plane_queues_[1] = std::make_unique<PlaneQueue>(protocol::PlaneType::Crossline);
    plane_queues_[2] = std::make_unique<PlaneQueue>(protocol::PlaneType::TimeDepth);
    
    // Log critical DataChannel configuration
    spdlog::info("DataChannel back-pressure configuration:");
    spdlog::info("  - unordered: {}", config_.unordered);
    spdlog::info("  - maxRetransmits: {}", config_.max_retransmits);
    spdlog::info("  - maxBuffered: {}KB", config_.max_buffered / 1024);
    spdlog::info("  - queue depth per plane: {}", PlaneQueue::MAX_DEPTH);
}

bool DataChannelManager::send_tile(std::unique_ptr<protocol::TileMessage> tile) {
    if (!tile) {
        return false;
    }
    
    // Extract plane type from tile header
    auto plane_idx = static_cast<size_t>(tile->header().plane);
    if (plane_idx >= 3) {
        spdlog::error("Invalid plane index in tile: {}", plane_idx);
        return false;
    }
    
    // Enqueue to appropriate plane queue
    return plane_queues_[plane_idx]->enqueue(std::move(tile));
}

std::unique_ptr<protocol::TileMessage> DataChannelManager::get_next_tile() {
    if (!is_channel_ready()) {
        return nullptr;
    }
    
    // Round-robin through planes to ensure fairness
    static size_t last_plane = 0;
    
    for (size_t i = 0; i < 3; ++i) {
        size_t plane_idx = (last_plane + i) % 3;
        auto tile = plane_queues_[plane_idx]->dequeue();
        
        if (tile) {
            last_plane = (plane_idx + 1) % 3;  // Next plane for fairness
            return tile;
        }
    }
    
    return nullptr;  // No tiles ready
}

void DataChannelManager::update_congestion_metrics(size_t buffered_bytes, uint32_t rtt_ms) {
    metrics_.buffered_bytes = buffered_bytes;
    metrics_.rtt_ms = rtt_ms;
    metrics_.last_rtt_update = std::chrono::steady_clock::now();
    
    // Track congestion events
    if (buffered_bytes > config_.max_buffered) {
        metrics_.congestion_events++;
        spdlog::debug("DataChannel congestion detected: buffered={}KB, limit={}KB", 
                     buffered_bytes / 1024, config_.max_buffered / 1024);
    }
}

bool DataChannelManager::is_congested() const {
    size_t buffered = metrics_.buffered_bytes.load();
    uint32_t rtt = metrics_.rtt_ms.load();
    
    // Congestion indicators:
    // 1. Buffer approaching limit
    // 2. RTT spike (> 250ms indicates network stress)
    // 3. Any queue at maximum depth
    
    bool buffer_congested = buffered > (config_.max_buffered * 3 / 4);  // 75% threshold
    bool rtt_spike = rtt > 250;  // High latency threshold
    
    bool queue_congested = false;
    for (const auto& queue : plane_queues_) {
        if (queue->is_full()) {
            queue_congested = true;
            break;
        }
    }
    
    if (buffer_congested || rtt_spike || queue_congested) {
        spdlog::debug("Congestion detected: buffer={}KB rtt={}ms queue_full={}", 
                     buffered / 1024, rtt, queue_congested);
        return true;
    }
    
    return false;
}

DataChannelManager::AllMetrics DataChannelManager::get_all_metrics() const {
    AllMetrics metrics;
    
    // Manually copy congestion metrics (atomic members can't be copy-assigned)
    metrics.congestion.buffered_bytes.store(metrics_.buffered_bytes.load());
    metrics.congestion.bytes_sent.store(metrics_.bytes_sent.load());
    metrics.congestion.congestion_events.store(metrics_.congestion_events.load());
    metrics.congestion.rtt_ms.store(metrics_.rtt_ms.load());
    metrics.congestion.last_rtt_update = metrics_.last_rtt_update;
    
    for (size_t i = 0; i < 3; ++i) {
        metrics.plane_stats[i] = plane_queues_[i]->get_stats();
    }
    
    return metrics;
}

void DataChannelManager::clear_all_queues() {
    spdlog::info("Clearing all plane queues due to slice change");
    
    for (auto& queue : plane_queues_) {
        queue->clear();
    }
}

} // namespace remoteview