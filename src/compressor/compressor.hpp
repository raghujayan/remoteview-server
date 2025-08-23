#pragma once

#include "protocol/tile_message.hpp"
#include <vector>
#include <memory>
#include <string>
#include <mutex>

namespace remoteview {

/**
 * Abstract Compressor Interface
 * 
 * This defines the interface for all compression algorithms used in RemoteView.
 * Seismic tile data compression is critical for:
 * - Reducing network bandwidth usage (tiles can be MB in size)
 * - Faster transmission over WebRTC DataChannels
 * - Better user experience with lower latency
 * 
 * The system supports multiple compression algorithms:
 * - LZ4: Fast compression/decompression, moderate ratio
 * - Zstd: Slower but better compression ratio
 * - None: No compression (for comparison/debugging)
 * 
 * Each compressor tracks statistics for performance monitoring.
 */
class Compressor {
public:
    virtual ~Compressor() = default;
    
    /**
     * Compress raw data buffer
     * @param data Raw input data (e.g. seismic pixel values)
     * @param size Size of input data in bytes
     * @return Compressed data buffer, empty if compression failed
     */
    virtual std::vector<uint8_t> compress(const uint8_t* data, size_t size) = 0;
    
    /**
     * Decompress compressed data buffer
     * @param data Compressed input data
     * @param compressed_size Size of compressed data in bytes
     * @param uncompressed_size Expected size after decompression (for validation)
     * @return Decompressed data buffer, empty if decompression failed
     */
    virtual std::vector<uint8_t> decompress(const uint8_t* data, size_t compressed_size, 
                                           size_t uncompressed_size) = 0;
    
    /**
     * Estimate compression ratio without actually compressing
     * Used for adaptive compression selection based on data characteristics
     * @param data Sample data to analyze
     * @param size Size of sample data
     * @return Estimated compression ratio (1.0 = no compression, 2.0 = 50% size reduction)
     */
    virtual float estimate_ratio(const uint8_t* data, size_t size) = 0;
    
    /**
     * Get human-readable compressor name for logging/debugging
     * @return Name string (e.g. "LZ4", "Zstd", "None")
     */
    virtual const char* name() const = 0;
    
    /**
     * Get compression type enum for protocol messages
     * @return CompressionType enum value for wire protocol
     */
    virtual protocol::CompressionType type() const = 0;
    
    // Performance metrics
    struct Stats {
        size_t compress_calls = 0;
        size_t decompress_calls = 0;
        size_t total_input_bytes = 0;
        size_t total_output_bytes = 0;
        double total_compress_time_ms = 0.0;
        double total_decompress_time_ms = 0.0;
        
        double compression_ratio() const {
            return total_input_bytes > 0 ? 
                   static_cast<double>(total_output_bytes) / total_input_bytes : 0.0;
        }
        
        double avg_compress_time_ms() const {
            return compress_calls > 0 ? total_compress_time_ms / compress_calls : 0.0;
        }
    };
    
    virtual Stats get_stats() const = 0;
    virtual void reset_stats() = 0;
};

// LZ4 Compressor - default, fast compression with proper frame format
class LZ4Compressor : public Compressor {
public:
    explicit LZ4Compressor(int compression_level = 1);
    ~LZ4Compressor();
    
    std::vector<uint8_t> compress(const uint8_t* data, size_t size) override;
    std::vector<uint8_t> decompress(const uint8_t* data, size_t compressed_size,
                                   size_t uncompressed_size) override;
    
    float estimate_ratio(const uint8_t* data, size_t size) override;
    const char* name() const override { return "LZ4Frame"; }
    protocol::CompressionType type() const override { return protocol::CompressionType::LZ4; }
    
    Stats get_stats() const override;
    void reset_stats() override;

private:
    int compression_level_;
    mutable Stats stats_;
    mutable std::mutex stats_mutex_;
    
    // LZ4 frame context for reuse (performance optimization)
    void* compression_ctx_;
    void* decompression_ctx_;
};

// Zstd Compressor - high compression with standard frame format
class ZstdCompressor : public Compressor {
public:
    explicit ZstdCompressor(int compression_level = 1);
    ~ZstdCompressor();
    
    std::vector<uint8_t> compress(const uint8_t* data, size_t size) override;
    std::vector<uint8_t> decompress(const uint8_t* data, size_t compressed_size,
                                   size_t uncompressed_size) override;
    
    float estimate_ratio(const uint8_t* data, size_t size) override;
    const char* name() const override { return "ZstdFrame"; }
    protocol::CompressionType type() const override { return protocol::CompressionType::Zstd; }
    
    Stats get_stats() const override;
    void reset_stats() override;

private:
    int compression_level_;
    mutable Stats stats_;
    mutable std::mutex stats_mutex_;
    
    // Zstd context for reuse (performance optimization)
    void* compression_ctx_;
    void* decompression_ctx_;
};

// No compression - passthrough
class NoCompressor : public Compressor {
public:
    std::vector<uint8_t> compress(const uint8_t* data, size_t size) override;
    std::vector<uint8_t> decompress(const uint8_t* data, size_t compressed_size,
                                   size_t uncompressed_size) override;
    
    float estimate_ratio(const uint8_t* /*data*/, size_t /*size*/) override { return 1.0f; }
    const char* name() const override { return "None"; }
    protocol::CompressionType type() const override { return protocol::CompressionType::None; }
    
    Stats get_stats() const override;
    void reset_stats() override;

private:
    mutable Stats stats_;
    mutable std::mutex stats_mutex_;
};

// Factory for creating compressors
class CompressorFactory {
public:
    static std::unique_ptr<Compressor> create(protocol::CompressionType type, 
                                            int compression_level = 1);
    
    static std::unique_ptr<Compressor> create(const std::string& name, 
                                            int compression_level = 1);
    
    // Get best compressor for data characteristics
    static protocol::CompressionType recommend_for_data(const uint8_t* data, size_t size,
                                                       bool prefer_speed = true);
};

} // namespace remoteview