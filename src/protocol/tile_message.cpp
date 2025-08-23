#include "tile_message.hpp"
#include <spdlog/spdlog.h>
#include <cstring>

namespace remoteview::protocol {

TileMessage::TileMessage(PlaneType plane, uint32_t slice_idx,
                        uint32_t tile_x, uint32_t tile_y,
                        uint16_t tile_w, uint16_t tile_h,
                        DataType dtype, CompressionType compression,
                        std::vector<uint8_t> payload) {
    
    header_.protocol_version = TileHeader::CURRENT_VERSION;
    header_.plane = static_cast<uint8_t>(plane);
    header_.tile_w = tile_w;
    header_.tile_h = tile_h;
    header_.tile_x = tile_x;
    header_.tile_y = tile_y;
    header_.slice_index = slice_idx;
    header_.dtype = static_cast<uint8_t>(dtype);
    header_.compression = static_cast<uint8_t>(compression);
    header_.payload_bytes = static_cast<uint32_t>(payload.size());
    
    payload_ = std::move(payload);
    
    // Note: uncompressed size can be calculated from tile dimensions and dtype
    // No need to store in header for 24-byte limit
}

std::vector<uint8_t> TileMessage::serialize() const {
    std::vector<uint8_t> result(TileHeader::SIZE + payload_.size());
    
    // Copy header
    memcpy(result.data(), &header_, TileHeader::SIZE);
    
    // Copy payload
    if (!payload_.empty()) {
        memcpy(result.data() + TileHeader::SIZE, payload_.data(), payload_.size());
    }
    
    return result;
}

std::unique_ptr<TileMessage> TileMessage::deserialize(const uint8_t* data, size_t size) {
    // Basic size validation
    if (size < TileHeader::SIZE) {
        spdlog::error("TileMessage data too small for header: {} bytes", size);
        return nullptr;
    }
    
    auto message = std::make_unique<TileMessage>();
    
    // Copy header with strict alignment
    memcpy(&message->header_, data, TileHeader::SIZE);
    
    // CRITICAL: Validate protocol version and frame size BEFORE allocation
    if (!message->header_.is_valid()) {
        spdlog::error("Invalid tile header: version=0x{:02x}, payload_bytes={}, plane={}, dtype={}", 
                     message->header_.protocol_version, 
                     message->header_.payload_bytes,
                     message->header_.plane,
                     message->header_.dtype);
        return nullptr;
    }
    
    // Validate endianness on first connection (static check)
    static bool endian_checked = false;
    if (!endian_checked) {
        if (!TileHeader::validate_endianness()) {
            spdlog::error("System endianness mismatch - protocol requires little-endian");
            return nullptr;
        }
        endian_checked = true;
        spdlog::info("Protocol endianness validated: little-endian OK");
    }
    
    // Validate total message size with overflow protection
    const size_t required_size = TileHeader::SIZE + message->header_.payload_bytes;
    if (required_size < TileHeader::SIZE || // Overflow check
        size < required_size) {             // Underflow check
        spdlog::error("Invalid message size: have={}, need={}, payload={}", 
                     size, required_size, message->header_.payload_bytes);
        return nullptr;
    }
    
    // Safe payload allocation (size already validated)
    message->payload_.resize(message->header_.payload_bytes);
    if (message->header_.payload_bytes > 0) {
        memcpy(message->payload_.data(), data + TileHeader::SIZE, message->header_.payload_bytes);
    }
    
    return message;
}

bool TileMessage::is_valid() const {
    if (!header_.is_valid()) {
        return false;
    }
    
    // Payload size must match header
    if (header_.payload_bytes != payload_.size()) {
        return false;
    }
    
    // Validate payload size makes sense for uncompressed data
    size_t bytes_per_pixel = bytes_per_sample(static_cast<DataType>(header_.dtype));
    size_t expected_uncompressed_size = static_cast<size_t>(header_.tile_w) * 
                                       header_.tile_h * bytes_per_pixel;
    
    // For uncompressed data, sizes should match exactly
    if (header_.compression == static_cast<uint8_t>(CompressionType::None)) {
        if (payload_.size() != expected_uncompressed_size) {
            spdlog::warn("Uncompressed tile payload size mismatch: expected={}, got={}", 
                        expected_uncompressed_size, payload_.size());
            return false;
        }
    } else {
        // For compressed data, payload should be smaller than uncompressed
        // but not ridiculously small (min 10% compression)
        if (payload_.size() > expected_uncompressed_size) {
            spdlog::warn("Compressed tile larger than uncompressed: compressed={}, uncompressed={}", 
                        payload_.size(), expected_uncompressed_size);
            return false;
        }
        
        // Minimum reasonable compression - allow very high compression ratios
        // (at least 1 byte per 1024 pixels to allow for very redundant data)
        size_t min_compressed_size = std::max(static_cast<size_t>(1), 
                                             (header_.tile_w * header_.tile_h + 1023) / 1024);
        if (payload_.size() < min_compressed_size && payload_.size() > 0) {
            spdlog::warn("Compressed tile suspiciously small: size={}, min_expected={}", 
                        payload_.size(), min_compressed_size);
            return false;
        }
    }
    
    // Check for empty payload when it should have data
    if (payload_.size() == 0 && expected_uncompressed_size > 0) {
        spdlog::warn("Empty tile payload for non-zero dimensions: {}x{}", 
                    header_.tile_w, header_.tile_h);
        return false;
    }
    
    return true;
}

size_t TileMessage::bytes_per_sample(DataType dtype) {
    switch (dtype) {
        case DataType::U8:
        case DataType::MuLawU8:
            return 1;
        case DataType::U16:
            return 2;
        case DataType::F32:
            return 4;
        default:
            return 1;
    }
}

const char* TileMessage::dtype_name(DataType dtype) {
    switch (dtype) {
        case DataType::U8: return "u8";
        case DataType::U16: return "u16";
        case DataType::F32: return "f32";
        case DataType::MuLawU8: return "mu-law-u8";
        default: return "unknown";
    }
}

const char* TileMessage::compression_name(CompressionType comp) {
    switch (comp) {
        case CompressionType::None: return "none";
        case CompressionType::LZ4: return "lz4";
        case CompressionType::Zstd: return "zstd";
        default: return "unknown";
    }
}

const char* TileMessage::plane_name(PlaneType plane) {
    switch (plane) {
        case PlaneType::Inline: return "inline";
        case PlaneType::Crossline: return "crossline";
        case PlaneType::TimeDepth: return "time-depth";
        default: return "unknown";
    }
}

// TileData implementation
bool TileData::is_valid() const {
    // Validate tile dimensions
    if (tile_w == 0 || tile_h == 0) {
        return false;
    }
    
    // Reasonable dimension limits (same as TileHeader)
    if (tile_w > 2048 || tile_h > 2048) {
        return false;
    }
    
    // Coordinate bounds check  
    if (tile_x > 1000000 || tile_y > 1000000) {
        return false;
    }
    
    // Slice bounds check
    if (slice_index > 100000) {
        return false;
    }
    
    // Validate enum values
    if (static_cast<uint8_t>(plane) > 2 || 
        static_cast<uint8_t>(dtype) > 3) {
        return false;
    }
    
    // Data size must match expected size
    size_t expected_size = expected_data_size();
    if (data.size() != expected_size) {
        spdlog::warn("TileData size mismatch: expected={}, got={} for {}x{} {}", 
                    expected_size, data.size(), tile_w, tile_h, 
                    TileMessage::dtype_name(dtype));
        return false;
    }
    
    // Check for reasonable data size (prevent huge allocations)
    uint64_t pixel_count = static_cast<uint64_t>(tile_w) * tile_h;
    if (pixel_count > 4 * 1024 * 1024) {
        spdlog::warn("TileData pixel count too large: {} pixels ({}x{})", 
                    pixel_count, tile_w, tile_h);
        return false;
    }
    
    return true;
}

size_t TileData::expected_data_size() const {
    size_t sample_size = TileMessage::bytes_per_sample(dtype);
    return tile_w * tile_h * sample_size;
}

// TileMessage validation utilities implementation
bool TileMessage::validate_tile_request(PlaneType plane, uint32_t slice_idx,
                                       uint32_t tile_x, uint32_t tile_y,
                                       uint16_t tile_w, uint16_t tile_h) {
    // Validate plane type
    if (static_cast<uint8_t>(plane) > 2) {
        spdlog::warn("Invalid plane type: {}", static_cast<uint8_t>(plane));
        return false;
    }
    
    // Validate slice index
    if (slice_idx > ValidationLimits::MAX_SLICE_INDEX) {
        spdlog::warn("Slice index too large: {} > {}", slice_idx, ValidationLimits::MAX_SLICE_INDEX);
        return false;
    }
    
    // Validate tile coordinates
    if (tile_x > ValidationLimits::MAX_COORDINATE || tile_y > ValidationLimits::MAX_COORDINATE) {
        spdlog::warn("Tile coordinates out of range: ({}, {}) > {}", 
                    tile_x, tile_y, ValidationLimits::MAX_COORDINATE);
        return false;
    }
    
    // Validate tile dimensions
    if (tile_w == 0 || tile_h == 0) {
        spdlog::warn("Zero tile dimensions: {}x{}", tile_w, tile_h);
        return false;
    }
    
    if (tile_w > ValidationLimits::MAX_TILE_DIMENSION || tile_h > ValidationLimits::MAX_TILE_DIMENSION) {
        spdlog::warn("Tile dimensions too large: {}x{} > {}", 
                    tile_w, tile_h, ValidationLimits::MAX_TILE_DIMENSION);
        return false;
    }
    
    // Validate pixel count doesn't cause huge allocations
    uint64_t pixel_count = static_cast<uint64_t>(tile_w) * tile_h;
    if (pixel_count > ValidationLimits::MAX_PIXELS_PER_TILE) {
        spdlog::warn("Too many pixels per tile: {} > {}", 
                    pixel_count, ValidationLimits::MAX_PIXELS_PER_TILE);
        return false;
    }
    
    return true;
}

bool TileMessage::validate_format_combination(DataType dtype, CompressionType compression) {
    // Validate data type enum
    if (static_cast<uint8_t>(dtype) > 3) {
        spdlog::warn("Invalid data type: {}", static_cast<uint8_t>(dtype));
        return false;
    }
    
    // Validate compression enum
    if (static_cast<uint8_t>(compression) > 2) {
        spdlog::warn("Invalid compression type: {}", static_cast<uint8_t>(compression));
        return false;
    }
    
    // All combinations currently supported
    // Future: could add restrictions like "F32 doesn't benefit from LZ4"
    return true;
}

std::unique_ptr<TileMessage> TileData::compress(CompressionType compression) const {
    spdlog::debug("Compressing tile data: {} bytes with {}", data.size(), TileMessage::compression_name(compression));
    
    // For now, return uncompressed data - this will be integrated with the compressor module
    // when we connect the tile serving pipeline
    return std::make_unique<TileMessage>(
        plane, slice_index, tile_x, tile_y, tile_w, tile_h,
        dtype, compression, data);
}

} // namespace remoteview::protocol