#include "cache.hpp"
#include "vds_access/vds_reader.hpp"  // For TileData definition
#include <spdlog/spdlog.h>
#include <algorithm>

/**
 * Hash function implementation for TileKey
 * 
 * This creates a unique hash value by combining all 7 fields of TileKey.
 * Each field is shifted by a different amount to minimize hash collisions.
 * The XOR operation combines all fields into a single hash value.
 * 
 * Good hash distribution is critical for unordered_map performance.
 * This ensures tiles with similar parameters get different hash values.
 */
namespace std {
    size_t hash<remoteview::TileKey>::operator()(const remoteview::TileKey& key) const {
        return hash<uint32_t>()(key.plane_index) ^                    // Plane: inline/crossline/time-depth
               (hash<uint32_t>()(key.slice_index) << 1) ^             // Which slice number (e.g. inline 1000)
               (hash<uint32_t>()(key.x_offset) << 2) ^                // X pixel offset in slice
               (hash<uint32_t>()(key.y_offset) << 3) ^                // Y pixel offset in slice  
               (hash<uint32_t>()(key.width) << 4) ^                   // Tile width (usually 256)
               (hash<uint32_t>()(key.height) << 5) ^                  // Tile height (usually 256)
               (hash<uint32_t>()(key.downsample_level) << 6);         // Level of detail (0=full, 1=half, etc.)
    }
}

namespace remoteview {

/**
 * TileEntry constructor
 * 
 * Creates a new cache entry with:
 * - key: Moved (not copied) for efficiency 
 * - data: Moved (not copied) to transfer ownership to cache
 * - last_access: Set to current time for LRU tracking
 * - size_bytes: Calculated from actual data size for memory management
 * 
 * The size calculation uses data->data.size() which gives the raw pixel buffer size.
 * This is used to enforce cache memory limits and evict tiles when needed.
 */
TileEntry::TileEntry(TileKey k, std::unique_ptr<remoteview::TileData> d)
    : key(std::move(k)),                                    // Transfer ownership of key
      data(std::move(d)),                                   // Transfer ownership of data
      last_access(std::chrono::steady_clock::now()) {      // Current time for LRU
    // Calculate memory footprint for cache size management
    // Uses the raw pixel buffer size from VDS reader
    size_bytes = data ? data->data.size() : 0;
}

/**
 * TileCache constructor
 * 
 * Initializes cache with default limits:
 * - max_entries_: 1000 tiles (prevents unbounded growth)
 * - max_size_bytes_: 512MB memory limit (for typical seismic workloads)
 * - All statistics counters start at 0
 * 
 * TODO: These limits should be loaded from the Config object
 * to allow runtime configuration via config files.
 */
TileCache::TileCache(std::shared_ptr<Config> config)
    : config_(std::move(config)),                         // Store config for future use
      max_entries_(1000),                                 // Maximum number of tiles to cache
      max_size_bytes_(512 * 1024 * 1024),                // Maximum memory usage (512MB)
      current_size_bytes_(0),                             // Start with empty cache
      hits_(0), misses_(0), evictions_(0) {               // Initialize all statistics to 0
}

/**
 * TileCache destructor
 * 
 * Ensures clean shutdown by calling shutdown() which frees all cached data.
 * This is important to prevent memory leaks when the cache is destroyed.
 */
TileCache::~TileCache() {
    shutdown();
}

/**
 * Initialize the cache system
 * 
 * Currently uses default values, but in the future will load configuration
 * from the Config object (cache sizes, eviction policies, etc.)
 * 
 * Logs the cache configuration for debugging and monitoring.
 */
void TileCache::initialize() {
    // TODO: Read cache configuration from config object
    // This should load max_entries_, max_size_bytes_, and other parameters
    // from a configuration file or environment variables
    
    spdlog::info("Tile cache initialized: max_entries={}, max_size={}MB", 
                 max_entries_, 
                 max_size_bytes_ / (1024 * 1024));  // Convert bytes to MB for logging
}

/**
 * Shutdown the cache system
 * 
 * Thread-safe cleanup of all cached data:
 * 1. Acquire mutex lock to prevent concurrent access
 * 2. Clear hash map (breaks TileKey -> iterator associations)
 * 3. Clear linked list (destroys all TileEntry objects and their data)
 * 4. Reset memory usage counter
 * 
 * This ensures no memory leaks when the cache is destroyed or restarted.
 */
void TileCache::shutdown() {
    std::lock_guard<std::mutex> lock(cache_mutex_);  // Thread-safe shutdown
    
    cache_map_.clear();        // Clear all TileKey -> iterator mappings
    cache_list_.clear();       // Destroy all TileEntry objects (frees pixel data)
    current_size_bytes_ = 0;   // Reset memory usage tracking
    
    spdlog::info("Tile cache shutdown complete");
}

/**
 * Get tile data from cache
 * 
 * This is the most critical cache operation, called on every tile request.
 * It implements the LRU cache lookup with the following steps:
 * 
 * 1. Thread-safe lookup in hash map (O(1) average case)
 * 2. If found (cache hit):
 *    - Move tile to front of LRU list (mark as most recently used)
 *    - Update access timestamp for debugging/monitoring
 *    - Increment hit counter
 *    - Create and return a COPY of the tile data
 * 3. If not found (cache miss):
 *    - Increment miss counter
 *    - Return nullptr (caller must load from VDS)
 * 
 * IMPORTANT: Returns a COPY of the cached data, not the original.
 * This prevents the caller from accidentally modifying cached data
 * and ensures thread safety when multiple requests access the same tile.
 * 
 * @param key Unique identifier for the requested tile
 * @return Copy of tile data if cached, nullptr if not found
 */
std::unique_ptr<remoteview::TileData> TileCache::get(const TileKey& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);  // Thread-safe access
    
    // O(1) lookup in hash map: TileKey -> iterator into linked list
    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) {
        // Cache miss - tile not found in cache
        misses_++;
        return nullptr;  // Caller must load from VDS
    }
    
    // Cache hit - tile found in cache
    
    // Move tile to front of LRU list (mark as most recently used)
    // This is O(1) because we have the iterator from the hash map
    move_to_front(it->second);
    
    // Update access timestamp for debugging and monitoring
    // Shows when this tile was last requested
    it->second->get()->last_access = std::chrono::steady_clock::now();
    
    hits_++;  // Update cache hit statistics
    
    // Create a COPY of the cached tile data to return
    // This prevents the caller from modifying the cached data
    // and ensures thread safety for concurrent access
    auto& entry_data = it->second->get()->data;
    if (!entry_data) {
        // Cached entry exists but has no data (shouldn't happen)
        return nullptr;
    }
    
    // Create new TileData object with copied values
    auto result = std::make_unique<remoteview::TileData>();
    result->width = entry_data->width;                    // Tile dimensions
    result->height = entry_data->height;
    result->bytes_per_sample = entry_data->bytes_per_sample;  // Data format info
    result->format = entry_data->format;                  // HueSpace data format
    result->data = entry_data->data;                      // COPY the pixel buffer (this is the expensive part)
    result->timestamp = entry_data->timestamp;           // When tile was originally loaded
    
    return result;  // Caller owns this copy
}

void TileCache::put(const TileKey& key, std::unique_ptr<remoteview::TileData> data) {
    if (!data) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    // Check if key already exists
    auto existing = cache_map_.find(key);
    if (existing != cache_map_.end()) {
        // Update existing entry
        current_size_bytes_ -= existing->second->get()->size_bytes;
        existing->second->get()->data = std::move(data);
        existing->second->get()->size_bytes = existing->second->get()->data->data.size();
        existing->second->get()->last_access = std::chrono::steady_clock::now();
        current_size_bytes_ += existing->second->get()->size_bytes;
        
        move_to_front(existing->second);
        return;
    }
    
    // Create new entry
    auto entry = std::make_unique<TileEntry>(key, std::move(data));
    size_t entry_size = entry->size_bytes;
    
    // Add to front of list
    cache_list_.push_front(std::move(entry));
    cache_map_[key] = cache_list_.begin();
    
    current_size_bytes_ += entry_size;
    
    // Evict if necessary
    while (should_evict()) {
        evict_lru();
    }
    
    spdlog::debug("Cached tile: plane={}, slice={}, pos=({},{}) size={}KB", 
                  key.plane_index, key.slice_index, key.x_offset, key.y_offset,
                  entry_size / 1024);
}

void TileCache::evict(const TileKey& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) {
        return;
    }
    
    current_size_bytes_ -= it->second->get()->size_bytes;
    cache_list_.erase(it->second);
    cache_map_.erase(it);
    
    evictions_++;
}

void TileCache::clear() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    cache_map_.clear();
    cache_list_.clear();
    current_size_bytes_ = 0;
    
    spdlog::info("Tile cache cleared");
}

void TileCache::reset_stats() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
}

void TileCache::evict_lru() {
    if (cache_list_.empty()) {
        return;
    }
    
    // Remove least recently used (back of list)
    auto& lru_entry = cache_list_.back();
    TileKey key = lru_entry->key;
    
    current_size_bytes_ -= lru_entry->size_bytes;
    cache_map_.erase(key);
    cache_list_.pop_back();
    
    evictions_++;
    
    spdlog::debug("Evicted LRU tile: plane={}, slice={}, pos=({},{})", 
                  key.plane_index, key.slice_index, key.x_offset, key.y_offset);
}

void TileCache::move_to_front(std::list<std::unique_ptr<TileEntry>>::iterator it) {
    if (it == cache_list_.begin()) {
        return; // Already at front
    }
    
    // Move to front
    cache_list_.splice(cache_list_.begin(), cache_list_, it);
}

bool TileCache::should_evict() const {
    return cache_list_.size() > max_entries_ || current_size_bytes_ > max_size_bytes_;
}

} // namespace remoteview