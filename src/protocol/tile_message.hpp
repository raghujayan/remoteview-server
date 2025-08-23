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

/**
 * Binary tile message header - exactly 24 bytes with strict protocol validation
 * 
 * PROTOCOL VERSION: 0x01
 * ENDIANNESS: Little-endian (x86/ARM standard)
 * MAX_FRAME_SIZE: 16MB (prevents memory exhaustion attacks)
 * 
 * Version enforcement ensures client/server compatibility.
 * Endianness validation prevents corruption on big-endian systems.
 * Frame size limits prevent DoS attacks via oversized allocations.
 */
#pragma pack(push, 1)
struct TileHeader {
    uint8_t protocol_version = 0x01;  // RESERVED: Protocol version (0x01), reject unknown versions
    uint8_t plane;                    // Orthogonal plane: 0=inline, 1=crossline, 2=time/depth
    uint16_t tile_w;                  // Tile width in pixels (typically 256) - LITTLE ENDIAN
    uint16_t tile_h;                  // Tile height in pixels (typically 256) - LITTLE ENDIAN  
    uint32_t tile_x;                  // X origin in slice pixel coordinates - LITTLE ENDIAN
    uint32_t tile_y;                  // Y origin in slice pixel coordinates - LITTLE ENDIAN
    uint32_t slice_index;             // Slice number within the plane - LITTLE ENDIAN
    uint8_t dtype;                    // Data type: 0=u8, 1=u16, 2=f32, 3=mu-law-u8
    uint8_t compression;              // Compression: 0=none, 1=LZ4, 2=Zstd  
    uint32_t payload_bytes;           // Size of compressed payload - LITTLE ENDIAN
    
    // Protocol constants for validation
    static constexpr uint8_t CURRENT_VERSION = 0x01;
    static constexpr size_t SIZE = 24;
    static constexpr size_t MAX_FRAME_SIZE = 16 * 1024 * 1024; // 16MB limit
    
    /**
     * Validate protocol version and frame size
     * @return true if header is valid and safe to process
     */
    bool is_valid() const {
        return protocol_version == CURRENT_VERSION && 
               payload_bytes <= MAX_FRAME_SIZE &&
               validate_tile_bounds() &&
               validate_enums() &&
               validate_slice_bounds();
    }

private:
    /**
     * Validate tile dimensions and coordinates
     */
    bool validate_tile_bounds() const {
        // Tile dimensions must be positive and reasonable
        if (tile_w == 0 || tile_h == 0) return false;
        if (tile_w > 2048 || tile_h > 2048) return false; // Max 2K tiles
        
        // Tile coordinates must be reasonable (prevent integer overflow)
        if (tile_x > 1000000 || tile_y > 1000000) return false;
        
        // Pixel count check (prevent huge allocations)
        uint64_t pixel_count = static_cast<uint64_t>(tile_w) * tile_h;
        if (pixel_count > 4 * 1024 * 1024) return false; // Max 4M pixels per tile
        
        return true;
    }
    
    /**
     * Validate enum values are within bounds
     */
    bool validate_enums() const {
        // Plane type: 0=Inline, 1=Crossline, 2=TimeDepth
        if (plane > 2) return false;
        
        // Data type: 0=U8, 1=U16, 2=F32, 3=MuLawU8
        if (dtype > 3) return false;
        
        // Compression: 0=None, 1=LZ4, 2=Zstd
        if (compression > 2) return false;
        
        return true;
    }
    
    /**
     * Validate slice index bounds
     */
    bool validate_slice_bounds() const {
        // Slice index should be reasonable (prevent bad requests)
        if (slice_index > 100000) return false; // Max 100K slices per volume
        
        return true;
    }

public:
    
    /**
     * Validate endianness by checking a magic number
     * Call this once during connection setup
     */
    static bool validate_endianness() {
        uint32_t magic = 0x12345678;
        uint8_t* bytes = reinterpret_cast<uint8_t*>(&magic);
        return bytes[0] == 0x78; // Little-endian: LSB first
    }
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
    
    // Validation utilities for runtime bounds checking
    struct ValidationLimits {
        static constexpr uint16_t MAX_TILE_DIMENSION = 2048;
        static constexpr uint32_t MAX_COORDINATE = 1000000;
        static constexpr uint32_t MAX_SLICE_INDEX = 100000;
        static constexpr uint64_t MAX_PIXELS_PER_TILE = 4 * 1024 * 1024; // 4M pixels
        static constexpr size_t MAX_PAYLOAD_SIZE = 16 * 1024 * 1024;     // 16MB
    };
    
    /**
     * Validate tile request parameters before processing
     * @param plane Seismic plane type
     * @param slice_idx Slice index in volume
     * @param tile_x Tile X coordinate
     * @param tile_y Tile Y coordinate  
     * @param tile_w Tile width in pixels
     * @param tile_h Tile height in pixels
     * @return true if parameters are safe to process
     */
    static bool validate_tile_request(PlaneType plane, uint32_t slice_idx, 
                                     uint32_t tile_x, uint32_t tile_y,
                                     uint16_t tile_w, uint16_t tile_h);
    
    /**
     * Validate data type and compression combination
     * @param dtype Data type for pixels
     * @param compression Compression algorithm
     * @return true if combination is supported
     */
    static bool validate_format_combination(DataType dtype, CompressionType compression);

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