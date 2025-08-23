#include <gtest/gtest.h>
#include "protocol/tile_message.hpp"
#include <vector>

using namespace remoteview::protocol;

class ProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test payload
        test_payload_.resize(1024);
        std::fill(test_payload_.begin(), test_payload_.end(), 0x42);
    }
    
    std::vector<uint8_t> test_payload_;
};

TEST_F(ProtocolTest, TileHeaderSize) {
    // Ensure tile header is exactly 24 bytes as per spec
    EXPECT_EQ(sizeof(TileHeader), 24);
    EXPECT_EQ(TileHeader::SIZE, 24);
}

TEST_F(ProtocolTest, TileMessageConstruction) {
    TileMessage msg(PlaneType::Inline, 100, 256, 512, 128, 128,
                   DataType::U8, CompressionType::LZ4, test_payload_);
    
    const auto& header = msg.header();
    EXPECT_EQ(header.protocol_version, 0x01);
    EXPECT_EQ(header.plane, static_cast<uint8_t>(PlaneType::Inline));
    EXPECT_EQ(header.slice_index, 100);
    EXPECT_EQ(header.tile_x, 256);
    EXPECT_EQ(header.tile_y, 512);
    EXPECT_EQ(header.tile_w, 128);
    EXPECT_EQ(header.tile_h, 128);
    EXPECT_EQ(header.dtype, static_cast<uint8_t>(DataType::U8));
    EXPECT_EQ(header.compression, static_cast<uint8_t>(CompressionType::LZ4));
    EXPECT_EQ(header.payload_bytes, test_payload_.size());
    
    EXPECT_EQ(msg.payload().size(), test_payload_.size());
    EXPECT_TRUE(msg.is_valid());
}

TEST_F(ProtocolTest, TileMessageSerialization) {
    TileMessage msg(PlaneType::Crossline, 200, 0, 0, 256, 256,
                   DataType::U16, CompressionType::Zstd, test_payload_);
    
    // Serialize
    auto serialized = msg.serialize();
    EXPECT_EQ(serialized.size(), TileHeader::SIZE + test_payload_.size());
    
    // Deserialize
    auto deserialized = TileMessage::deserialize(serialized.data(), serialized.size());
    EXPECT_NE(deserialized, nullptr);
    EXPECT_TRUE(deserialized->is_valid());
    
    // Compare headers
    const auto& orig_header = msg.header();
    const auto& deser_header = deserialized->header();
    EXPECT_EQ(orig_header.protocol_version, deser_header.protocol_version);
    EXPECT_EQ(orig_header.plane, deser_header.plane);
    EXPECT_EQ(orig_header.slice_index, deser_header.slice_index);
    EXPECT_EQ(orig_header.tile_x, deser_header.tile_x);
    EXPECT_EQ(orig_header.tile_y, deser_header.tile_y);
    EXPECT_EQ(orig_header.tile_w, deser_header.tile_w);
    EXPECT_EQ(orig_header.tile_h, deser_header.tile_h);
    EXPECT_EQ(orig_header.dtype, deser_header.dtype);
    EXPECT_EQ(orig_header.compression, deser_header.compression);
    EXPECT_EQ(orig_header.payload_bytes, deser_header.payload_bytes);
    
    // Compare payload
    EXPECT_EQ(msg.payload(), deserialized->payload());
}

TEST_F(ProtocolTest, TileMessageInvalidDeserialization) {
    // Too small data
    std::vector<uint8_t> small_data(10);
    auto result = TileMessage::deserialize(small_data.data(), small_data.size());
    EXPECT_EQ(result, nullptr);
    
    // Create valid header but insufficient payload
    TileMessage valid_msg(PlaneType::Inline, 0, 0, 0, 1, 1, 
                         DataType::U8, CompressionType::None, test_payload_);
    auto serialized = valid_msg.serialize();
    
    // Truncate the payload
    serialized.resize(TileHeader::SIZE + 10); // Not enough payload
    result = TileMessage::deserialize(serialized.data(), serialized.size());
    EXPECT_EQ(result, nullptr);
}

TEST_F(ProtocolTest, DataTypeUtilities) {
    EXPECT_EQ(TileMessage::bytes_per_sample(DataType::U8), 1);
    EXPECT_EQ(TileMessage::bytes_per_sample(DataType::U16), 2);
    EXPECT_EQ(TileMessage::bytes_per_sample(DataType::F32), 4);
    EXPECT_EQ(TileMessage::bytes_per_sample(DataType::MuLawU8), 1);
    
    EXPECT_STREQ(TileMessage::dtype_name(DataType::U8), "u8");
    EXPECT_STREQ(TileMessage::dtype_name(DataType::U16), "u16");
    EXPECT_STREQ(TileMessage::dtype_name(DataType::F32), "f32");
    EXPECT_STREQ(TileMessage::dtype_name(DataType::MuLawU8), "mu-law-u8");
    
    EXPECT_STREQ(TileMessage::compression_name(CompressionType::None), "none");
    EXPECT_STREQ(TileMessage::compression_name(CompressionType::LZ4), "lz4");
    EXPECT_STREQ(TileMessage::compression_name(CompressionType::Zstd), "zstd");
    
    EXPECT_STREQ(TileMessage::plane_name(PlaneType::Inline), "inline");
    EXPECT_STREQ(TileMessage::plane_name(PlaneType::Crossline), "crossline");
    EXPECT_STREQ(TileMessage::plane_name(PlaneType::TimeDepth), "time-depth");
}

TEST_F(ProtocolTest, TileDataValidation) {
    TileData tile_data;
    tile_data.plane = PlaneType::Inline;
    tile_data.slice_index = 0;
    tile_data.tile_x = 0;
    tile_data.tile_y = 0;
    tile_data.tile_w = 10;
    tile_data.tile_h = 10;
    tile_data.dtype = DataType::U8;
    
    // Valid data size
    tile_data.data.resize(100); // 10x10x1 byte
    EXPECT_TRUE(tile_data.is_valid());
    EXPECT_EQ(tile_data.expected_data_size(), 100);
    
    // Invalid data size
    tile_data.data.resize(50);
    EXPECT_FALSE(tile_data.is_valid());
    
    // Test with different data type
    tile_data.dtype = DataType::U16;
    tile_data.data.resize(200); // 10x10x2 bytes
    EXPECT_TRUE(tile_data.is_valid());
    EXPECT_EQ(tile_data.expected_data_size(), 200);
}

TEST_F(ProtocolTest, TileDataCompression) {
    TileData tile_data;
    tile_data.plane = PlaneType::Inline;
    tile_data.slice_index = 100;
    tile_data.tile_x = 0;
    tile_data.tile_y = 0;
    tile_data.tile_w = 32;
    tile_data.tile_h = 32;
    tile_data.dtype = DataType::U8;
    tile_data.data.resize(1024);
    std::fill(tile_data.data.begin(), tile_data.data.end(), 0x55);
    
    // Compress with different types
    auto lz4_msg = tile_data.compress(CompressionType::LZ4);
    EXPECT_NE(lz4_msg, nullptr);
    EXPECT_EQ(lz4_msg->header().compression, static_cast<uint8_t>(CompressionType::LZ4));
    
    auto zstd_msg = tile_data.compress(CompressionType::Zstd);
    EXPECT_NE(zstd_msg, nullptr);
    EXPECT_EQ(zstd_msg->header().compression, static_cast<uint8_t>(CompressionType::Zstd));
    
    auto none_msg = tile_data.compress(CompressionType::None);
    EXPECT_NE(none_msg, nullptr);
    EXPECT_EQ(none_msg->header().compression, static_cast<uint8_t>(CompressionType::None));
}