#pragma once

// HueSpace API for metadata extraction
#include <HueSpace3/VolumeData.h>
#include <HueSpace3/DataBlock.h>
#include <string>
#include <vector>
#include <cstdint>

namespace remoteview {

struct VdsMetadata {
    struct Dimension {
        std::string name;
        std::string unit;
        uint32_t size;
        float min_value;
        float max_value;
        float step_size;
    };
    
    struct Channel {
        std::string name;
        std::string unit;
        Hue::HueSpaceLib::DataBlock::Format format;
        uint32_t components;
        float range_min;
        float range_max;
        float no_value;
        bool use_no_value;
    };
    
    std::string file_path;
    std::vector<Dimension> dimensions;
    std::vector<Channel> channels;
    
    uint32_t inline_size;
    uint32_t crossline_size;
    uint32_t time_samples;
    
    float inline_min;
    float inline_max;
    float inline_step;
    
    float crossline_min;
    float crossline_max;
    float crossline_step;
    
    float time_min;
    float time_max;
    float time_step;
    
    std::string coordinate_system;
    bool has_trace_coordinates;
    
    size_t uncompressed_size;
    size_t compressed_size;
    
    // Derived properties for tile access
    uint32_t max_downsample_level;
    std::vector<uint32_t> level_sizes_inline;
    std::vector<uint32_t> level_sizes_crossline;
};

class VdsMetadataExtractor {
public:
    static VdsMetadata extract_from_layout(const Hue::HueSpaceLib::VolumeDataLayout* layout);
    
private:
    static void extract_dimensions(const Hue::HueSpaceLib::VolumeDataLayout* layout, VdsMetadata& metadata);
    static void extract_channels(const Hue::HueSpaceLib::VolumeDataLayout* layout, VdsMetadata& metadata);
    static void extract_coordinate_system(const Hue::HueSpaceLib::VolumeDataLayout* layout, VdsMetadata& metadata);
    static void calculate_downsample_levels(VdsMetadata& metadata);
};

} // namespace remoteview