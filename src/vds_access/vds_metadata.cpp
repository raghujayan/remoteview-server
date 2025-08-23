#include "vds_metadata.hpp"
#include <HueSpace3/ProxyBLOB.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace remoteview {

VdsMetadata VdsMetadataExtractor::extract_from_layout(const Hue::HueSpaceLib::VolumeDataLayout* layout) {
    VdsMetadata metadata;
    
    if (!layout) {
        throw std::runtime_error("VolumeDataLayout is null");
    }
    
    extract_dimensions(layout, metadata);
    extract_channels(layout, metadata);
    extract_coordinate_system(layout, metadata);
    calculate_downsample_levels(metadata);
    
    return metadata;
}

void VdsMetadataExtractor::extract_dimensions(const Hue::HueSpaceLib::VolumeDataLayout* layout, VdsMetadata& metadata) {
    int dimension_count = layout->GetDimensionality();
    
    metadata.dimensions.reserve(dimension_count);
    
    // Follow the pattern from StandAloneLoad.cpp commented metadata section
    for (int i = 0; i < dimension_count; ++i) {
        Hue::HueSpaceLib::VolumeDataAxisDescriptor axis_desc = layout->GetAxisDescriptor(i);
        
        VdsMetadata::Dimension dimension;
        dimension.name = axis_desc.GetName();
        dimension.unit = axis_desc.GetUnit();
        dimension.size = axis_desc.GetNumSamples();
        dimension.min_value = axis_desc.GetCoordinateMin();
        dimension.max_value = axis_desc.GetCoordinateMax();
        
        // Calculate step size from coordinate range and samples
        if (dimension.size > 1) {
            dimension.step_size = (dimension.max_value - dimension.min_value) / (dimension.size - 1);
        } else {
            dimension.step_size = 1.0f;
        }
        
        metadata.dimensions.push_back(dimension);
        
        // Map to standard seismic dimensions (following StandAloneLoad coordinate convention)
        if (i == 0) { // Typically Inline
            metadata.inline_size = dimension.size;
            metadata.inline_min = dimension.min_value;
            metadata.inline_max = dimension.max_value;
            metadata.inline_step = dimension.step_size;
        } else if (i == 1) { // Typically Crossline
            metadata.crossline_size = dimension.size;
            metadata.crossline_min = dimension.min_value;
            metadata.crossline_max = dimension.max_value;
            metadata.crossline_step = dimension.step_size;
        } else if (i == 2) { // Typically Time/Depth
            metadata.time_samples = dimension.size;
            metadata.time_min = dimension.min_value;
            metadata.time_max = dimension.max_value;
            metadata.time_step = dimension.step_size;
        }
        
        spdlog::debug("Dimension {}: {} ({}) - {} samples, {:.3f} to {:.3f} step {:.6f}",
                     i, dimension.name, dimension.unit,
                     dimension.size, dimension.min_value,
                     dimension.max_value, dimension.step_size);
    }
}

void VdsMetadataExtractor::extract_channels(const Hue::HueSpaceLib::VolumeDataLayout* layout, VdsMetadata& metadata) {
    int channel_count = layout->GetChannelCount();
    
    metadata.channels.reserve(channel_count);
    
    // Follow the pattern from StandAloneLoad.cpp commented metadata section
    for (int i = 0; i < channel_count; ++i) {
        Hue::HueSpaceLib::VolumeDataChannelDescriptor channel_desc = layout->GetChannelDescriptor(i);
        
        VdsMetadata::Channel channel;
        channel.name = channel_desc.GetName();
        channel.unit = channel_desc.GetUnit();
        channel.format = channel_desc.GetFormat();
        channel.components = static_cast<uint32_t>(channel_desc.GetComponents());
        channel.range_min = channel_desc.GetValueRangeMin();
        channel.range_max = channel_desc.GetValueRangeMax();
        channel.no_value = channel_desc.GetNoValue();
        channel.use_no_value = channel_desc.IsUseNoValue();
        
        metadata.channels.push_back(channel);
        
        // Check for trace-mapped data (from StandAloneLoad pattern)
        if (channel_desc.GetMapping() == Hue::HueSpaceLib::VolumeDataMapping::VOLUMEDATAMAPPING_PER_TRACE) {
            int values_per_trace = channel_desc.GetMappedValueCount();
            spdlog::debug("Channel {}: {} - trace-mapped with {} values per trace",
                         i, channel.name, values_per_trace);
        } else {
            spdlog::debug("Channel {}: {} ({}) - format {}, range {:.3f} to {:.3f}",
                         i, channel.name, channel.unit,
                         static_cast<int>(channel.format),
                         channel.range_min, channel.range_max);
        }
    }
}

void VdsMetadataExtractor::extract_coordinate_system(const Hue::HueSpaceLib::VolumeDataLayout* layout, VdsMetadata& metadata) {
    // Extract coordinate reference system information
    // Following the pattern from StandAloneLoad.cpp commented section
    
    metadata.has_trace_coordinates = false;
    metadata.coordinate_system = "Unknown";
    
    try {
        // Check for survey coordinate system metadata (from StandAloneLoad pattern)
        if (layout->IsMetadataDoubleVector2Available("SurveyCoordinateSystem", "Origin")) {
            Hue::HueSpaceLib::DoubleVector2 origin = layout->GetMetadataDoubleVector2("SurveyCoordinateSystem", "Origin");
            Hue::HueSpaceLib::DoubleVector2 inline_spacing = layout->GetMetadataDoubleVector2("SurveyCoordinateSystem", "InlineSpacing");
            Hue::HueSpaceLib::DoubleVector2 crossline_spacing = layout->GetMetadataDoubleVector2("SurveyCoordinateSystem", "CrosslineSpacing");
            
            metadata.has_trace_coordinates = true;
            metadata.coordinate_system = "Survey Coordinate System";
            
            spdlog::debug("Survey coordinates - Origin: ({:.2f}, {:.2f})", origin.X, origin.Y);
            spdlog::debug("Inline spacing: ({:.2f}, {:.2f})", inline_spacing.X, inline_spacing.Y);
            spdlog::debug("Crossline spacing: ({:.2f}, {:.2f})", crossline_spacing.X, crossline_spacing.Y);
        }
        
        // Check for SEGY text header (from StandAloneLoad pattern)
        if (layout->IsMetadataBLOBAvailable("", "SEGYTextHeader")) {
            const Hue::HueSpaceLib::ProxyBLOB* blob = layout->GetMetadataBLOB("", "SEGYTextHeader");
            if (blob && blob->GetDataSize() > 0) {
                spdlog::debug("SEGY text header available ({} bytes)", blob->GetDataSize());
            }
        }
        
    } catch (const std::exception& e) {
        spdlog::warn("Failed to extract coordinate system information: {}", e.what());
        metadata.coordinate_system = "Unknown";
        metadata.has_trace_coordinates = false;
    }
}

void VdsMetadataExtractor::calculate_downsample_levels(VdsMetadata& metadata) {
    // Calculate maximum downsample levels based on data dimensions
    uint32_t max_dimension = std::max(metadata.inline_size, metadata.crossline_size);
    metadata.max_downsample_level = 0;
    
    // Calculate levels until minimum tile size is reached
    constexpr uint32_t MIN_SIZE_AT_MAX_LEVEL = 32;
    
    uint32_t current_size = max_dimension;
    while (current_size > MIN_SIZE_AT_MAX_LEVEL) {
        metadata.max_downsample_level++;
        current_size /= 2;
    }
    
    // Pre-calculate sizes for each level
    metadata.level_sizes_inline.reserve(metadata.max_downsample_level + 1);
    metadata.level_sizes_crossline.reserve(metadata.max_downsample_level + 1);
    
    for (uint32_t level = 0; level <= metadata.max_downsample_level; ++level) {
        uint32_t factor = 1 << level;
        metadata.level_sizes_inline.push_back(
            std::max(1u, metadata.inline_size / factor));
        metadata.level_sizes_crossline.push_back(
            std::max(1u, metadata.crossline_size / factor));
    }
    
    // Calculate storage sizes
    if (!metadata.channels.empty()) {
        auto& primary_channel = metadata.channels[0];
        size_t bytes_per_sample;
        
        // Map HueSpace format to byte size
        switch (primary_channel.format) {
            case Hue::HueSpaceLib::DataBlock::Format_U8:
                bytes_per_sample = 1;
                break;
            case Hue::HueSpaceLib::DataBlock::Format_U16:
                bytes_per_sample = 2;
                break;
            case Hue::HueSpaceLib::DataBlock::Format_R32:
                bytes_per_sample = 4;
                break;
            default:
                bytes_per_sample = 4;
                break;
        }
        
        metadata.uncompressed_size = static_cast<size_t>(metadata.inline_size) *
                                   metadata.crossline_size *
                                   metadata.time_samples *
                                   bytes_per_sample;
    }
    
    spdlog::info("VDS downsample levels: {} (max dimension {} -> min {})",
                 metadata.max_downsample_level + 1,
                 max_dimension,
                 max_dimension >> metadata.max_downsample_level);
}

} // namespace remoteview