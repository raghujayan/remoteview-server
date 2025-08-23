#pragma once

#include <vector>
#include <cstdint>
#include <memory>

namespace remoteview::protocol {

/**
 * RemoteView Binary Tile Protocol
 * 
 * This implements the wire protocol for transmitting seismic tiles over WebRTC DataChannels.
 * The protocol is designed for efficiency and minimal parsing overhead:
 * 
 * Message Format: [TileHeader 24 bytes] + [Compressed Payload N bytes]
 * 
 * The 24-byte header contains all metadata needed to reconstruct and display the tile:
 * - Spatial location (plane, slice, x/y offsets)
 * - Tile dimensions (width, height)
 * - Data format (u8/u16/f32/mu-law)
 * - Compression type and payload size
 * 
 * This binary protocol is much more efficient than JSON for real-time streaming
 * as it minimizes bandwidth and parsing overhead.
 */

// Binary tile message header - exactly 24 bytes as per specification
// Uses packed structure to ensure consistent byte layout across platforms
#pragma pack(push, 1)
struct TileHeader {
    uint8_t msg_type = 0x01;    // Message type identifier (0x01 = tile data)
    uint8_t plane;              // Orthogonal plane: 0=inline, 1=crossline, 2=time/depth
    uint16_t tile_w;            // Tile width in pixels (typically 256)
    uint16_t tile_h;            // Tile height in pixels (typically 256)
    uint32_t tile_x;            // X origin in slice pixel coordinates
    uint32_t tile_y;            // Y origin in slice pixel coordinates  
    uint32_t slice_index;       // Slice number within the plane (e.g. inline 1000)
    uint8_t dtype;              // Data type: 0=u8, 1=u16, 2=f32, 3=mu-law-u8
    uint8_t compression;        // Compression: 0=none, 1=LZ4, 2=Zstd
    uint32_t payload_bytes;     // Size of compressed payload that follows
    
    // Compile-time size verification - critical for wire protocol compatibility
    static constexpr size_t SIZE = 24; 
};
#pragma pack(pop)

// Compile-time assertion to ensure header is exactly 24 bytes
// This prevents accidental padding that would break the wire protocol
static_assert(sizeof(TileHeader) == TileHeader::SIZE, 
              "TileHeader must be exactly 24 bytes for wire protocol compatibility");

/**
 * Data type enumeration for seismic pixel values
 * 
 * Different seismic datasets use different data formats depending on:
 * - Source data precision requirements
 * - Storage and bandwidth constraints
 * - Processing pipeline outputs
 */
enum class DataType : uint8_t {
    U8 = 0,        // 8-bit unsigned integer (1 byte per sample, good for amplitude displays)
    U16 = 1,       // 16-bit unsigned integer (2 bytes per sample, higher precision)  
    F32 = 2,       // 32-bit floating point (4 bytes per sample, full precision)
    MuLawU8 = 3    // μ-law compressed 8-bit (logarithmic compression for audio-like data)
};

/**
 * Compression algorithm enumeration
 * 
 * Different compression algorithms trade off between speed and compression ratio:
 * - None: No compression (fastest, largest size)
 * - LZ4: Fast compression with moderate ratio (good for real-time)
 * - Zstd: Slower compression with better ratio (good for storage/slow connections)
 */
enum class CompressionType : uint8_t {
    None = 0,      // No compression applied
    LZ4 = 1,       // LZ4 fast compression
    Zstd = 2       // Zstandard high-ratio compression
};

/**
 * Seismic plane type enumeration
 * 
 * Seismic data is viewed through three orthogonal plane orientations:
 * - Inline: Parallel to seismic acquisition lines (shows geological structure)
 * - Crossline: Perpendicular to acquisition lines (shows lateral continuity)  
 * - Time/Depth: Horizontal time slices (shows stratigraphic layers)
 */
enum class PlaneType : uint8_t {
    Inline = 0,      // Inline plane (parallel to seismic lines)
    Crossline = 1,   // Crossline plane (perpendicular to seismic lines)
    TimeDepth = 2    // Time/depth plane (horizontal slices)
};

class TileMessage {
public:
    TileMessage() = default;
    
    // Create tile message with data
    TileMessage(PlaneType plane, uint32_t slice_idx,
                uint32_t tile_x, uint32_t tile_y,
                uint16_t tile_w, uint16_t tile_h,
                DataType dtype, CompressionType compression,
                std::vector<uint8_t> payload);
    
    // Serialize to binary format
    std::vector<uint8_t> serialize() const;
    
    // Deserialize from binary format
    static std::unique_ptr<TileMessage> deserialize(const uint8_t* data, size_t size);
    
    // Accessors
    const TileHeader& header() const { return header_; }
    const std::vector<uint8_t>& payload() const { return payload_; }
    
    // Validation
    bool is_valid() const;
    size_t total_size() const { return TileHeader::SIZE + payload_.size(); }
    
    // Data type utilities
    static size_t bytes_per_sample(DataType dtype);
    static const char* dtype_name(DataType dtype);
    static const char* compression_name(CompressionType comp);
    static const char* plane_name(PlaneType plane);

private:
    TileHeader header_{};
    std::vector<uint8_t> payload_;
};

// Tile data container (uncompressed)
struct TileData {
    PlaneType plane;
    uint32_t slice_index;
    uint32_t tile_x, tile_y;
    uint16_t tile_w, tile_h;
    DataType dtype;
    std::vector<uint8_t> data; // Raw uncompressed pixel data
    
    // Validation
    bool is_valid() const;
    size_t expected_data_size() const;
    
    // Create compressed tile message
    std::unique_ptr<TileMessage> compress(CompressionType compression) const;
};

} // namespace remoteview::protocol