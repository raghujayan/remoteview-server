#include <gtest/gtest.h>
#include "compressor/compressor.hpp"
#include <vector>
#include <string>
#include <random>

using namespace remoteview;

class CompressorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate test data
        std::mt19937 rng(42); // Fixed seed for reproducible tests
        std::uniform_int_distribution<uint8_t> dist(0, 255);
        
        test_data_.resize(1024);
        for (auto& byte : test_data_) {
            byte = dist(rng);
        }
        
        // Create repeated data for better compression
        repeated_data_.resize(1024);
        std::fill(repeated_data_.begin(), repeated_data_.end(), 0x42);
    }
    
    std::vector<uint8_t> test_data_;
    std::vector<uint8_t> repeated_data_;
};

TEST_F(CompressorTest, LZ4CompressDecompress) {
    LZ4Compressor compressor;
    
    // Compress data
    auto compressed = compressor.compress(test_data_.data(), test_data_.size());
    EXPECT_GT(compressed.size(), 0);
    
    // Decompress data
    auto decompressed = compressor.decompress(compressed.data(), compressed.size(), test_data_.size());
    EXPECT_EQ(decompressed.size(), test_data_.size());
    EXPECT_EQ(decompressed, test_data_);
    
    // Check statistics
    auto stats = compressor.get_stats();
    EXPECT_EQ(stats.compress_calls, 1);
    EXPECT_EQ(stats.decompress_calls, 1);
    EXPECT_GT(stats.total_compress_time_ms, 0);
}

TEST_F(CompressorTest, ZstdCompressDecompress) {
    ZstdCompressor compressor;
    
    // Compress data
    auto compressed = compressor.compress(repeated_data_.data(), repeated_data_.size());
    EXPECT_GT(compressed.size(), 0);
    EXPECT_LT(compressed.size(), repeated_data_.size()); // Should compress well
    
    // Decompress data
    auto decompressed = compressor.decompress(compressed.data(), compressed.size(), repeated_data_.size());
    EXPECT_EQ(decompressed.size(), repeated_data_.size());
    EXPECT_EQ(decompressed, repeated_data_);
}

TEST_F(CompressorTest, NoCompressor) {
    NoCompressor compressor;
    
    // "Compress" data (should be unchanged)
    auto compressed = compressor.compress(test_data_.data(), test_data_.size());
    EXPECT_EQ(compressed, test_data_);
    
    // "Decompress" data (should be unchanged)
    auto decompressed = compressor.decompress(compressed.data(), compressed.size(), test_data_.size());
    EXPECT_EQ(decompressed, test_data_);
}

TEST_F(CompressorTest, CompressorFactory) {
    // Test LZ4 creation
    auto lz4_compressor = CompressorFactory::create(protocol::CompressionType::LZ4);
    EXPECT_NE(lz4_compressor, nullptr);
    EXPECT_EQ(lz4_compressor->type(), protocol::CompressionType::LZ4);
    
    // Test Zstd creation
    auto zstd_compressor = CompressorFactory::create(protocol::CompressionType::Zstd);
    EXPECT_NE(zstd_compressor, nullptr);
    EXPECT_EQ(zstd_compressor->type(), protocol::CompressionType::Zstd);
    
    // Test None creation
    auto none_compressor = CompressorFactory::create(protocol::CompressionType::None);
    EXPECT_NE(none_compressor, nullptr);
    EXPECT_EQ(none_compressor->type(), protocol::CompressionType::None);
}

TEST_F(CompressorTest, EstimateRatio) {
    LZ4Compressor compressor;
    
    // Random data should not compress well
    float random_ratio = compressor.estimate_ratio(test_data_.data(), test_data_.size());
    EXPECT_GT(random_ratio, 0.5f);
    
    // Repeated data should compress very well
    float repeated_ratio = compressor.estimate_ratio(repeated_data_.data(), repeated_data_.size());
    EXPECT_LT(repeated_ratio, 0.5f);
}

TEST_F(CompressorTest, RecommendForData) {
    // Speed preference should return LZ4
    auto speed_type = CompressorFactory::recommend_for_data(test_data_.data(), test_data_.size(), true);
    EXPECT_EQ(speed_type, protocol::CompressionType::LZ4);
    
    // Large data with quality preference should return Zstd
    auto quality_type = CompressorFactory::recommend_for_data(test_data_.data(), 16384, false);
    EXPECT_EQ(quality_type, protocol::CompressionType::Zstd);
    
    // Small data with quality preference should return LZ4
    auto small_type = CompressorFactory::recommend_for_data(test_data_.data(), 1024, false);
    EXPECT_EQ(small_type, protocol::CompressionType::LZ4);
}