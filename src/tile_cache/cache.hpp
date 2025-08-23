#pragma once

#include "config/config.hpp"
#include "protocol/tile_message.hpp"
#include <memory>
#include <unordered_map>
#include <list>
#include <mutex>
#include <atomic>
#include <chrono>

namespace remoteview {

// Forward declarations to avoid circular dependencies
// TileKey: Unique identifier for cached tiles
// TileData: Raw seismic data from VDS reader (defined in vds_access/vds_reader.hpp)
struct TileKey;
struct TileData;

} // namespace remoteview

/**
 * Hash function specialization for TileKey to enable use in std::unordered_map
 * Must be defined in std namespace before TileKey is used as a map key
 * Combines all key fields with bit shifts to create unique hash values
 */
namespace std {
    template<>
    struct hash<remoteview::TileKey> {
        size_t operator()(const remoteview::TileKey& key) const;
    };
}

namespace remoteview {

/**
 * TileKey: Unique identifier for seismic data tiles
 * 
 * This structure defines all parameters needed to uniquely identify a tile of seismic data:
 * - plane_index: Which orthogonal plane (inline=0, crossline=1, time/depth=2)  
 * - slice_index: Position within that plane (e.g., inline 1000, crossline 500)
 * - x_offset, y_offset: Pixel coordinates within the slice where tile starts
 * - width, height: Tile dimensions in pixels (typically 256x256)
 * - downsample_level: Level of detail (0=full res, 1=half res, 2=quarter res, etc.)
 * 
 * These 7 parameters completely specify any tile in the 3D seismic volume.
 */
struct TileKey {
    uint32_t plane_index;      // Which orthogonal plane (0=inline, 1=crossline, 2=time/depth)
    uint32_t slice_index;      // Position within that plane (e.g. inline number 1000)
    uint32_t x_offset;         // X pixel coordinate where tile starts within slice  
    uint32_t y_offset;         // Y pixel coordinate where tile starts within slice
    uint32_t width;            // Tile width in pixels (usually 256)
    uint32_t height;           // Tile height in pixels (usually 256)
    uint32_t downsample_level; // Level of detail (0=full resolution, 1=half, etc.)
    
    /**
     * Equality operator required for use in hash maps
     * Two tiles are considered equal if ALL their parameters match
     */
    bool operator==(const TileKey& other) const {
        return plane_index == other.plane_index &&
               slice_index == other.slice_index &&
               x_offset == other.x_offset &&
               y_offset == other.y_offset &&
               width == other.width &&
               height == other.height &&
               downsample_level == other.downsample_level;
    }
};

/**
 * TileEntry: Individual cache entry containing tile data and metadata
 * 
 * Each cached tile consists of:
 * - key: The TileKey that uniquely identifies this tile
 * - data: The actual seismic pixel data from VDS reader  
 * - last_access: Timestamp for LRU (Least Recently Used) eviction policy
 * - size_bytes: Memory footprint for cache size management
 * 
 * Constructor moved to .cpp file to avoid incomplete type issues with TileData
 */
struct TileEntry {
    TileKey key;                                          // Unique identifier for this tile
    std::unique_ptr<remoteview::TileData> data;          // Raw seismic pixel data from VDS
    std::chrono::steady_clock::time_point last_access;   // When tile was last accessed (for LRU)
    size_t size_bytes;                                    // Memory footprint of this entry
    
    // Constructor declaration - implementation in cache.cpp to avoid forward declaration issues
    TileEntry(TileKey k, std::unique_ptr<remoteview::TileData> d);
};

/**
 * TileCache: High-performance LRU cache for seismic data tiles
 * 
 * This cache implements a Least Recently Used (LRU) eviction policy with the following features:
 * - Thread-safe access with mutex protection
 * - O(1) get/put operations using hash map + doubly-linked list
 * - Memory and entry count limits to prevent unbounded growth
 * - Detailed statistics for cache hit rates and performance monitoring
 * - Automatic eviction when size/count limits are exceeded
 * 
 * The cache uses a classic LRU implementation:
 * - unordered_map for O(1) key lookup (TileKey -> list iterator)
 * - doubly-linked list for O(1) insertion/deletion and LRU ordering
 * - Most recently used items at front, least recently used at back
 * 
 * This design is critical for real-time seismic viewing where the same tiles
 * are frequently re-accessed as users pan/zoom through the data.
 */
class TileCache {
public:
    explicit TileCache(std::shared_ptr<Config> config);
    ~TileCache();
    
    // Lifecycle management
    void initialize();    // Load configuration, allocate resources
    void shutdown();      // Clean shutdown, free all cached data
    
    /**
     * Cache operations - all thread-safe
     * 
     * get(): Retrieve tile data if cached, nullptr if cache miss
     *        Updates LRU position and access timestamp on hit
     *        Returns a COPY of the cached data (caller owns the memory)
     * 
     * put(): Store tile data in cache, may trigger eviction
     *        If key already exists, updates the existing entry
     *        Automatically evicts LRU entries if limits exceeded
     * 
     * evict(): Explicitly remove a tile from cache (used by prefetch logic)
     * 
     * clear(): Remove all cached tiles, reset statistics
     */
    std::unique_ptr<remoteview::TileData> get(const TileKey& key);
    void put(const TileKey& key, std::unique_ptr<remoteview::TileData> data);
    void evict(const TileKey& key);
    void clear();
    
    /**
     * Cache statistics for performance monitoring and debugging
     * 
     * hits: Number of successful cache lookups (data was cached)
     * misses: Number of failed lookups (data had to be loaded from VDS)
     * evictions: Number of tiles removed due to size/count limits
     * current_entries: Number of tiles currently cached
     * current_size_bytes: Total memory used by cached tiles
     * max_size_bytes: Maximum allowed cache size
     * hit_rate(): Percentage of lookups that hit the cache (higher is better)
     */
    struct Stats {
        size_t hits{0};               // Successful cache lookups
        size_t misses{0};             // Cache misses (had to load from VDS)
        size_t evictions{0};          // Tiles evicted due to size limits
        size_t current_entries{0};    // Number of tiles currently cached
        size_t current_size_bytes{0}; // Total memory used by cache
        size_t max_size_bytes{0};     // Maximum allowed cache size
        
        /**
         * Calculate cache hit rate as percentage
         * Higher hit rates indicate better cache performance
         * Typical good hit rates for seismic viewing are 70-90%
         */
        double hit_rate() const {
            size_t total = hits + misses;
            return total > 0 ? static_cast<double>(hits) / total : 0.0;
        }
    };
    
    /**
     * Get current cache statistics (thread-safe)
     * Returns a snapshot of cache performance metrics
     */
    Stats get_stats() const { 
        std::lock_guard<std::mutex> lock(cache_mutex_);
        Stats result;
        result.hits = hits_;
        result.misses = misses_;
        result.evictions = evictions_;
        result.current_entries = cache_list_.size();
        result.current_size_bytes = current_size_bytes_;
        result.max_size_bytes = max_size_bytes_;
        return result;
    }
    
    // Reset all statistics counters (useful for benchmarking)
    void reset_stats();

private:
    // Configuration and limits
    std::shared_ptr<Config> config_;  // Application configuration (cache sizes, etc.)
    
    /**
     * LRU cache implementation using hash map + doubly-linked list
     * 
     * This is a classic computer science pattern for O(1) LRU cache:
     * 
     * cache_map_: Hash table mapping TileKey -> iterator into cache_list_
     *            Provides O(1) lookup to find if a tile is cached
     *            Iterator points to the tile's position in the linked list
     * 
     * cache_list_: Doubly-linked list of TileEntry objects
     *             Front = most recently used, Back = least recently used  
     *             Allows O(1) insertion/deletion/reordering of tiles
     * 
     * When a tile is accessed:
     * 1. Look up iterator in cache_map_ - O(1)
     * 2. Move that list node to front - O(1) 
     * 3. Update access timestamp - O(1)
     * 
     * When eviction is needed:
     * 1. Remove back node from list - O(1)
     * 2. Remove corresponding map entry - O(1)
     */
    mutable std::mutex cache_mutex_;                                           // Thread safety for all operations
    std::unordered_map<TileKey, std::list<std::unique_ptr<TileEntry>>::iterator> cache_map_;  // Key -> List position
    std::list<std::unique_ptr<TileEntry>> cache_list_;                        // LRU ordered list (front=new, back=old)
    
    // Cache limits (TODO: load from config)
    size_t max_entries_;        // Maximum number of tiles to cache (default 1000)
    size_t max_size_bytes_;     // Maximum memory usage in bytes (default 512MB)
    size_t current_size_bytes_; // Current memory usage tracking
    
    // Performance statistics (protected by cache_mutex_)
    size_t hits_;       // Number of cache hits (found in cache)
    size_t misses_;     // Number of cache misses (had to load from VDS)  
    size_t evictions_;  // Number of tiles evicted due to size limits
    
    /**
     * Internal cache management methods
     * 
     * evict_lru(): Remove the least recently used tile (back of list)
     *             Called automatically when cache limits are exceeded
     * 
     * move_to_front(): Move an existing cache entry to front of list (most recently used)
     *                 Called on every cache hit to maintain LRU ordering
     * 
     * should_evict(): Check if cache has exceeded size or entry count limits
     *                Returns true if eviction is needed
     */
    void evict_lru();                                                         // Remove least recently used tile
    void move_to_front(std::list<std::unique_ptr<TileEntry>>::iterator it);  // Move tile to front (most recent)
    bool should_evict() const;                                               // Check if eviction needed
};

} // namespace remoteview