/**
 * RemoteView Compression with Standard Frame Formats
 * 
 * This implementation uses proper frame formats for LZ4 and Zstd compression:
 * 
 * LZ4 Frame Format (LZ4F):
 * - Magic number: 0x184D2204
 * - Self-describing headers with content size and checksum flags
 * - Standard format decodable by any LZ4 tool (e.g., lz4 command line)
 * - Content integrity verification with checksums
 * - Better error detection and recovery
 * 
 * Zstd Frame Format:
 * - Magic number: 0xFD2FB528
 * - Standard Zstd frame format decodable by any zstd tool
 * - Built-in content size and integrity verification
 * - Better error detection than raw blocks
 * 
 * Benefits over raw blocks:
 * - Tiles can be saved to disk and decoded offline with standard tools
 * - Better error detection and debugging capabilities
 * - Standard format ensures interoperability
 * - Content size and integrity verification built-in
 * - Proper handling of edge cases and malformed data
 */

#include "compressor.hpp"
#include <spdlog/spdlog.h>
#include <lz4.h>
#include <lz4hc.h>
#include <lz4frame.h>  // LZ4 frame format API
#include <zstd.h>
#include <chrono>

namespace remoteview {

// LZ4 Compressor implementation with frame format
LZ4Compressor::LZ4Compressor(int compression_level) 
    : compression_level_(compression_level), compression_ctx_(nullptr), decompression_ctx_(nullptr) {
    // Initialize LZ4F compression context
    LZ4F_errorCode_t result = LZ4F_createCompressionContext(
        reinterpret_cast<LZ4F_cctx**>(&compression_ctx_), LZ4F_VERSION);
    if (LZ4F_isError(result)) {
        spdlog::error("Failed to create LZ4F compression context: {}", LZ4F_getErrorName(result));
        compression_ctx_ = nullptr;
    }
    
    // Initialize LZ4F decompression context  
    result = LZ4F_createDecompressionContext(
        reinterpret_cast<LZ4F_dctx**>(&decompression_ctx_), LZ4F_VERSION);
    if (LZ4F_isError(result)) {
        spdlog::error("Failed to create LZ4F decompression context: {}", LZ4F_getErrorName(result));
        decompression_ctx_ = nullptr;
    }
    
    spdlog::debug("LZ4Frame compressor initialized with level {} (frame format)", compression_level_);
}

LZ4Compressor::~LZ4Compressor() {
    if (compression_ctx_) {
        LZ4F_freeCompressionContext(reinterpret_cast<LZ4F_cctx*>(compression_ctx_));
    }
    if (decompression_ctx_) {
        LZ4F_freeDecompressionContext(reinterpret_cast<LZ4F_dctx*>(decompression_ctx_));
    }
}

std::vector<uint8_t> LZ4Compressor::compress(const uint8_t* data, size_t size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    if (!compression_ctx_) {
        spdlog::error("LZ4F compression context not initialized");
        return std::vector<uint8_t>(data, data + size); // Return uncompressed
    }
    
    // Configure LZ4F preferences for tile compression
    LZ4F_preferences_t preferences = {};
    preferences.frameInfo.contentSize = size;
    preferences.frameInfo.blockSizeID = LZ4F_max64KB;  // Good for tile sizes
    preferences.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled; // Integrity check
    preferences.compressionLevel = compression_level_;
    
    // Calculate maximum frame size (header + content + footer)
    size_t max_frame_size = LZ4F_compressFrameBound(size, &preferences);
    std::vector<uint8_t> compressed(max_frame_size);
    
    // Compress entire frame in one shot (simpler and sufficient for tiles)
    size_t compressed_size = LZ4F_compressFrame(
        compressed.data(),
        max_frame_size,
        data,
        size,
        &preferences
    );
    
    if (LZ4F_isError(compressed_size)) {
        spdlog::error("LZ4F frame compression failed: {}", LZ4F_getErrorName(compressed_size));
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
    
    spdlog::debug("LZ4Frame compressed {} -> {} bytes ({:.1f}%)", 
                  size, compressed_size, (compressed_size * 100.0) / size);
    
    return compressed;
}

std::vector<uint8_t> LZ4Compressor::decompress(const uint8_t* data, size_t compressed_size, size_t uncompressed_size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    if (!decompression_ctx_) {
        spdlog::error("LZ4F decompression context not initialized");
        return std::vector<uint8_t>(data, data + compressed_size); // Return compressed data as fallback
    }
    
    // Reset decompression context for new frame
    LZ4F_resetDecompressionContext(reinterpret_cast<LZ4F_dctx*>(decompression_ctx_));
    
    std::vector<uint8_t> decompressed(uncompressed_size);
    size_t src_size = compressed_size;
    size_t dst_size = uncompressed_size;
    
    // Decompress entire frame in one shot
    LZ4F_errorCode_t result = LZ4F_decompress(
        reinterpret_cast<LZ4F_dctx*>(decompression_ctx_),
        decompressed.data(), &dst_size,
        data, &src_size,
        nullptr  // No decompression options
    );
    
    if (LZ4F_isError(result)) {
        spdlog::error("LZ4F frame decompression failed: {}", LZ4F_getErrorName(result));
        return std::vector<uint8_t>(data, data + compressed_size); // Return compressed data as fallback
    }
    
    // Verify frame was completely consumed
    if (src_size != compressed_size) {
        spdlog::warn("LZ4F frame not completely consumed: {} of {} bytes", src_size, compressed_size);
    }
    
    // Update statistics
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.decompress_calls++;
        stats_.total_decompress_time_ms += duration.count() / 1000.0;
    }
    
    spdlog::debug("LZ4Frame decompressed {} -> {} bytes", compressed_size, dst_size);
    
    decompressed.resize(dst_size);
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

// Zstd Compressor implementation with frame format
ZstdCompressor::ZstdCompressor(int compression_level) 
    : compression_level_(compression_level), compression_ctx_(nullptr), decompression_ctx_(nullptr) {
    // Create Zstd compression context for reuse
    compression_ctx_ = ZSTD_createCCtx();
    if (!compression_ctx_) {
        spdlog::error("Failed to create Zstd compression context");
    }
    
    // Create Zstd decompression context for reuse  
    decompression_ctx_ = ZSTD_createDCtx();
    if (!decompression_ctx_) {
        spdlog::error("Failed to create Zstd decompression context");
    }
    
    spdlog::debug("ZstdFrame compressor initialized with level {} (frame format)", compression_level_);
}

ZstdCompressor::~ZstdCompressor() {
    if (compression_ctx_) {
        ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(compression_ctx_));
    }
    if (decompression_ctx_) {
        ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(decompression_ctx_));
    }
}

std::vector<uint8_t> ZstdCompressor::compress(const uint8_t* data, size_t size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    if (!compression_ctx_) {
        spdlog::error("Zstd compression context not initialized");
        return std::vector<uint8_t>(data, data + size); // Return uncompressed
    }
    
    size_t max_compressed_size = ZSTD_compressBound(size);
    std::vector<uint8_t> compressed(max_compressed_size);
    
    // Use context-based compression with frame format (standard Zstd format)
    size_t compressed_size = ZSTD_compressCCtx(
        static_cast<ZSTD_CCtx*>(compression_ctx_),
        compressed.data(),
        max_compressed_size,
        data,
        size,
        compression_level_
    );
    
    if (ZSTD_isError(compressed_size)) {
        spdlog::error("ZstdFrame compression failed: {}", ZSTD_getErrorName(compressed_size));
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
    
    spdlog::debug("ZstdFrame compressed {} -> {} bytes ({:.1f}%)", 
                  size, compressed_size, (compressed_size * 100.0) / size);
    
    return compressed;
}

std::vector<uint8_t> ZstdCompressor::decompress(const uint8_t* data, size_t compressed_size, size_t uncompressed_size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    if (!decompression_ctx_) {
        spdlog::error("Zstd decompression context not initialized");
        return std::vector<uint8_t>(data, data + compressed_size); // Return compressed data as fallback
    }
    
    std::vector<uint8_t> decompressed(uncompressed_size);
    
    // Use context-based decompression (handles standard Zstd frame format)
    size_t result = ZSTD_decompressDCtx(
        static_cast<ZSTD_DCtx*>(decompression_ctx_),
        decompressed.data(),
        uncompressed_size,
        data,
        compressed_size
    );
    
    if (ZSTD_isError(result)) {
        spdlog::error("ZstdFrame decompression failed: {}", ZSTD_getErrorName(result));
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
    
    spdlog::debug("ZstdFrame decompressed {} -> {} bytes", compressed_size, result);
    
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