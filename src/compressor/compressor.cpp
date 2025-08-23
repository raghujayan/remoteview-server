#include "compressor.hpp"
#include <spdlog/spdlog.h>
#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>
#include <chrono>

namespace remoteview {

// LZ4 Compressor implementation
LZ4Compressor::LZ4Compressor(int compression_level) 
    : compression_level_(compression_level) {
    spdlog::debug("LZ4 compressor initialized with level {}", compression_level_);
}

std::vector<uint8_t> LZ4Compressor::compress(const uint8_t* data, size_t size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Calculate maximum compressed size
    int max_compressed_size = LZ4_compressBound(static_cast<int>(size));
    if (max_compressed_size <= 0) {
        spdlog::error("LZ4_compressBound failed for size {}", size);
        return std::vector<uint8_t>(data, data + size); // Return uncompressed
    }
    
    std::vector<uint8_t> compressed(max_compressed_size);
    
    int compressed_size;
    if (compression_level_ <= 1) {
        // Fast compression
        compressed_size = LZ4_compress_default(
            reinterpret_cast<const char*>(data),
            reinterpret_cast<char*>(compressed.data()),
            static_cast<int>(size),
            max_compressed_size
        );
    } else {
        // High compression
        compressed_size = LZ4_compress_HC(
            reinterpret_cast<const char*>(data),
            reinterpret_cast<char*>(compressed.data()),
            static_cast<int>(size),
            max_compressed_size,
            compression_level_
        );
    }
    
    if (compressed_size <= 0) {
        spdlog::error("LZ4 compression failed");
        return std::vector<uint8_t>(data, data + size); // Return uncompressed
    }
    
    compressed.resize(compressed_size);
    
    // Update statistics
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.compress_calls++;
        stats_.total_input_bytes += size;
        stats_.total_output_bytes += compressed_size;
        stats_.total_compress_time_ms += duration.count() / 1000.0;
    }
    
    spdlog::debug("LZ4 compressed {} -> {} bytes ({:.1f}%)", 
                  size, compressed_size, (compressed_size * 100.0) / size);
    
    return compressed;
}

std::vector<uint8_t> LZ4Compressor::decompress(const uint8_t* data, size_t compressed_size, size_t uncompressed_size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::vector<uint8_t> decompressed(uncompressed_size);
    
    int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(data),
        reinterpret_cast<char*>(decompressed.data()),
        static_cast<int>(compressed_size),
        static_cast<int>(uncompressed_size)
    );
    
    if (result < 0) {
        spdlog::error("LZ4 decompression failed");
        return std::vector<uint8_t>(data, data + compressed_size); // Return compressed data as fallback
    }
    
    // Update statistics
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.decompress_calls++;
        stats_.total_decompress_time_ms += duration.count() / 1000.0;
    }
    
    spdlog::debug("LZ4 decompressed {} -> {} bytes", compressed_size, result);
    
    decompressed.resize(result);
    return decompressed;
}

float LZ4Compressor::estimate_ratio(const uint8_t* data, size_t size) {
    // Sample-based estimation - compress a small portion to estimate ratio
    size_t sample_size = std::min(size, static_cast<size_t>(4096));
    auto compressed = compress(data, sample_size);
    
    if (compressed.size() >= sample_size) {
        return 1.0f; // No compression benefit
    }
    
    return static_cast<float>(compressed.size()) / sample_size;
}

Compressor::Stats LZ4Compressor::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void LZ4Compressor::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Stats{};
}

// Zstd Compressor implementation
ZstdCompressor::ZstdCompressor(int compression_level) 
    : compression_level_(compression_level) {
    spdlog::debug("Zstd compressor initialized with level {}", compression_level_);
}

std::vector<uint8_t> ZstdCompressor::compress(const uint8_t* data, size_t size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    size_t max_compressed_size = ZSTD_compressBound(size);
    std::vector<uint8_t> compressed(max_compressed_size);
    
    size_t compressed_size = ZSTD_compress(
        compressed.data(),
        max_compressed_size,
        data,
        size,
        compression_level_
    );
    
    if (ZSTD_isError(compressed_size)) {
        spdlog::error("Zstd compression failed: {}", ZSTD_getErrorName(compressed_size));
        return std::vector<uint8_t>(data, data + size); // Return uncompressed
    }
    
    compressed.resize(compressed_size);
    
    // Update statistics
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.compress_calls++;
        stats_.total_input_bytes += size;
        stats_.total_output_bytes += compressed_size;
        stats_.total_compress_time_ms += duration.count() / 1000.0;
    }
    
    spdlog::debug("Zstd compressed {} -> {} bytes ({:.1f}%)", 
                  size, compressed_size, (compressed_size * 100.0) / size);
    
    return compressed;
}

std::vector<uint8_t> ZstdCompressor::decompress(const uint8_t* data, size_t compressed_size, size_t uncompressed_size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::vector<uint8_t> decompressed(uncompressed_size);
    
    size_t result = ZSTD_decompress(
        decompressed.data(),
        uncompressed_size,
        data,
        compressed_size
    );
    
    if (ZSTD_isError(result)) {
        spdlog::error("Zstd decompression failed: {}", ZSTD_getErrorName(result));
        return std::vector<uint8_t>(data, data + compressed_size); // Return compressed data as fallback
    }
    
    // Update statistics
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.decompress_calls++;
        stats_.total_decompress_time_ms += duration.count() / 1000.0;
    }
    
    spdlog::debug("Zstd decompressed {} -> {} bytes", compressed_size, result);
    
    decompressed.resize(result);
    return decompressed;
}

float ZstdCompressor::estimate_ratio(const uint8_t* data, size_t size) {
    // Sample-based estimation
    size_t sample_size = std::min(size, static_cast<size_t>(4096));
    auto compressed = compress(data, sample_size);
    
    if (compressed.size() >= sample_size) {
        return 1.0f; // No compression benefit
    }
    
    return static_cast<float>(compressed.size()) / sample_size;
}

Compressor::Stats ZstdCompressor::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void ZstdCompressor::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Stats{};
}

// Factory implementation
std::unique_ptr<Compressor> CompressorFactory::create(protocol::CompressionType type, int compression_level) {
    switch (type) {
        case protocol::CompressionType::LZ4:
            return std::make_unique<LZ4Compressor>(compression_level);
        case protocol::CompressionType::Zstd:
            return std::make_unique<ZstdCompressor>(compression_level);
        case protocol::CompressionType::None:
            return std::make_unique<NoCompressor>();
        default:
            spdlog::warn("Unsupported compression type, using LZ4");
            return std::make_unique<LZ4Compressor>(compression_level);
    }
}

std::unique_ptr<Compressor> CompressorFactory::create(const std::string& name, int compression_level) {
    if (name == "lz4") {
        return std::make_unique<LZ4Compressor>(compression_level);
    } else if (name == "zstd") {
        return std::make_unique<ZstdCompressor>(compression_level);
    } else if (name == "none") {
        return std::make_unique<NoCompressor>();
    } else {
        spdlog::warn("Unknown compression name '{}', using LZ4", name);
        return std::make_unique<LZ4Compressor>(compression_level);
    }
}

protocol::CompressionType CompressorFactory::recommend_for_data(const uint8_t* data, size_t size, bool prefer_speed) {
    if (prefer_speed) {
        return protocol::CompressionType::LZ4;
    } else {
        // For higher compression ratio, use Zstd for larger data
        return size > 8192 ? protocol::CompressionType::Zstd : protocol::CompressionType::LZ4;
    }
}

// No compressor implementation  
std::vector<uint8_t> NoCompressor::compress(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.compress_calls++;
    stats_.total_input_bytes += size;
    stats_.total_output_bytes += size;
    
    return std::vector<uint8_t>(data, data + size);
}

std::vector<uint8_t> NoCompressor::decompress(const uint8_t* data, size_t compressed_size, size_t uncompressed_size) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.decompress_calls++;
    
    return std::vector<uint8_t>(data, data + compressed_size);
}

Compressor::Stats NoCompressor::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void NoCompressor::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Stats{};
}

} // namespace remoteview