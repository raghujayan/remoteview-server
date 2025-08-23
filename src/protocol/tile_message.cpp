#include "tile_message.hpp"
#include <spdlog/spdlog.h>
#include <cstring>

namespace remoteview::protocol {

TileMessage::TileMessage(PlaneType plane, uint32_t slice_idx,
                        uint32_t tile_x, uint32_t tile_y,
                        uint16_t tile_w, uint16_t tile_h,
                        DataType dtype, CompressionType compression,
                        std::vector<uint8_t> payload) {
    
    header_.msg_type = 0x01;
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
    if (size < TileHeader::SIZE) {
        spdlog::error("TileMessage data too small for header: {} bytes", size);
        return nullptr;
    }
    
    auto message = std::make_unique<TileMessage>();
    
    // Copy header
    memcpy(&message->header_, data, TileHeader::SIZE);
    
    // Validate header
    if (message->header_.msg_type != 0x01) {
        spdlog::error("Invalid tile message type: {}", message->header_.msg_type);
        return nullptr;
    }
    
    // Check payload size
    if (size < TileHeader::SIZE + message->header_.payload_bytes) {
        spdlog::error("Insufficient data for payload: {} < {}", 
                     size, TileHeader::SIZE + message->header_.payload_bytes);
        return nullptr;
    }
    
    // Copy payload
    message->payload_.resize(message->header_.payload_bytes);
    if (message->header_.payload_bytes > 0) {
        memcpy(message->payload_.data(), data + TileHeader::SIZE, message->header_.payload_bytes);
    }
    
    return message;
}

bool TileMessage::is_valid() const {
    return header_.msg_type == 0x01 && 
           header_.payload_bytes == payload_.size();
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
    return data.size() == expected_data_size();
}

size_t TileData::expected_data_size() const {
    size_t sample_size = TileMessage::bytes_per_sample(dtype);
    return tile_w * tile_h * sample_size;
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