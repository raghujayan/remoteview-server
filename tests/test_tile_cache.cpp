#include <gtest/gtest.h>
#include <memory>
#include <chrono>

// Create a minimal test-only version to avoid HueSpace dependencies
namespace remoteview {
    struct TileData {
        std::vector<uint8_t> data;
        uint32_t width;
        uint32_t height;
        uint32_t bytes_per_sample;
        std::chrono::steady_clock::time_point timestamp;
    };
}

#include "tile_cache/cache.hpp"
#include "config/config.hpp"

using namespace remoteview;

class TileCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = std::make_shared<Config>();
        config_->load_default();
        cache_ = std::make_unique<TileCache>(config_);
        cache_->initialize();
        
        // Create test data (minimal test version)
        test_data_ = std::make_unique<remoteview::TileData>();
        test_data_->width = 256;
        test_data_->height = 256;
        test_data_->bytes_per_sample = 1;
        test_data_->data.resize(256 * 256);
        std::fill(test_data_->data.begin(), test_data_->data.end(), 0x42);
        test_data_->timestamp = std::chrono::steady_clock::now();
        
        test_key_ = TileKey{0, 100, 0, 0, 256, 256, 0};
    }
    
    void TearDown() override {
        cache_->shutdown();
    }
    
    std::shared_ptr<Config> config_;
    std::unique_ptr<TileCache> cache_;
    std::unique_ptr<remoteview::TileData> test_data_;
    TileKey test_key_;
};

TEST_F(TileCacheTest, BasicPutGet) {
    // Cache miss initially
    auto result = cache_->get(test_key_);
    EXPECT_EQ(result, nullptr);
    
    // Put data in cache
    auto data_copy = std::make_unique<remoteview::TileData>(*test_data_);
    cache_->put(test_key_, std::move(data_copy));
    
    // Should hit now
    result = cache_->get(test_key_);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->width, test_data_->width);
    EXPECT_EQ(result->height, test_data_->height);
    EXPECT_EQ(result->data.size(), test_data_->data.size());
    
    // Check stats
    auto stats = cache_->get_stats();
    EXPECT_EQ(stats.misses, 1);
    EXPECT_EQ(stats.hits, 1);
    EXPECT_GT(stats.current_entries, 0);
    EXPECT_GT(stats.current_size_bytes, 0);
}

TEST_F(TileCacheTest, Eviction) {
    TileKey key1{0, 100, 0, 0, 256, 256, 0};
    TileKey key2{0, 101, 0, 0, 256, 256, 0};
    
    // Put first entry
    auto data1 = std::make_unique<remoteview::TileData>(*test_data_);
    cache_->put(key1, std::move(data1));
    
    // Put second entry  
    auto data2 = std::make_unique<remoteview::TileData>(*test_data_);
    cache_->put(key2, std::move(data2));
    
    // Both should be accessible
    EXPECT_NE(cache_->get(key1), nullptr);
    EXPECT_NE(cache_->get(key2), nullptr);
    
    // Explicit eviction
    cache_->evict(key1);
    EXPECT_EQ(cache_->get(key1), nullptr);
    EXPECT_NE(cache_->get(key2), nullptr);
}

TEST_F(TileCacheTest, Clear) {
    // Add some data
    cache_->put(test_key_, std::make_unique<remoteview::TileData>(*test_data_));
    EXPECT_NE(cache_->get(test_key_), nullptr);
    
    // Clear cache
    cache_->clear();
    EXPECT_EQ(cache_->get(test_key_), nullptr);
    
    // Stats should be reset
    auto stats = cache_->get_stats();
    EXPECT_EQ(stats.current_entries, 0);
    EXPECT_EQ(stats.current_size_bytes, 0);
}

TEST_F(TileCacheTest, UpdateExisting) {
    // Put initial data
    cache_->put(test_key_, std::make_unique<remoteview::TileData>(*test_data_));
    
    // Update with new data
    auto updated_data = std::make_unique<remoteview::TileData>(*test_data_);
    updated_data->data.resize(512);
    cache_->put(test_key_, std::move(updated_data));
    
    // Should get updated data
    auto result = cache_->get(test_key_);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->data.size(), 512);
}

TEST_F(TileCacheTest, StatsTracking) {
    cache_->reset_stats();
    
    // Test miss
    cache_->get(test_key_);
    auto stats = cache_->get_stats();
    EXPECT_EQ(stats.misses, 1);
    EXPECT_EQ(stats.hits, 0);
    
    // Test hit
    cache_->put(test_key_, std::make_unique<remoteview::TileData>(*test_data_));
    cache_->get(test_key_);
    stats = cache_->get_stats();
    EXPECT_EQ(stats.hits, 1);
    
    // Test hit rate calculation
    EXPECT_FLOAT_EQ(stats.hit_rate(), 0.5); // 1 hit out of 2 total
}

TEST_F(TileCacheTest, TileKeyEquality) {
    TileKey key1{0, 100, 0, 0, 256, 256, 0};
    TileKey key2{0, 100, 0, 0, 256, 256, 0};
    TileKey key3{0, 101, 0, 0, 256, 256, 0}; // Different slice
    
    EXPECT_TRUE(key1 == key2);
    EXPECT_FALSE(key1 == key3);
}