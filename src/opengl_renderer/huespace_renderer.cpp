// Include HueSpace headers FIRST to avoid X11 macro conflicts
#include <HueSpace3/ProxyInterfaceFactory.h>
#include <HueSpace3/ProxyInterface.h>
#include <HueSpace3/ConfigMemoryManagement.h>
#include <HueSpace3/Workspace.h>
#include <HueSpace3/SceneManager.h>
#include <HueSpace3/Scene.h>
#include <HueSpace3/ProjectManager.h>
#include <HueSpace3/Project.h>
#include <HueSpace3/VDS.h>
#include <HueSpace3/VDSManager.h>
#include <HueSpace3/RenderView.h>
#include <HueSpace3/RenderViewClient.h>
#include <HueSpace3/Viewer.h>
#include <HueSpace3/DataBlock.h>

// Now include our headers and others (OpenGL headers already included in .hpp)
#include "huespace_renderer.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

// Note: Using public HueSpace SDK - low-level graphics headers not available
#include <cstring>
#include <stdexcept>

namespace remoteview {

HueSpaceRenderer::HueSpaceRenderer(std::shared_ptr<Config> config) 
    : config_(std::move(config)) {
}

HueSpaceRenderer::~HueSpaceRenderer() {
    cleanup();
}

bool HueSpaceRenderer::initialize(const RenderConfig& render_config) {
    spdlog::info("🎨 Initializing HueSpace OpenGL renderer...");
    
    render_config_ = render_config;
    
    try {
        // Create headless OpenGL context
        if (!create_gl_context()) {
            spdlog::error("Failed to create OpenGL context");
            return false;
        }
        
        if (!make_current()) {
            spdlog::error("Failed to make OpenGL context current");
            return false;
        }
        
        // Get OpenGL info
        const char* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        
        spdlog::info("✅ OpenGL context created");
        spdlog::info("   Version: {}", gl_version ? gl_version : "Unknown");
        spdlog::info("   Renderer: {}", gl_renderer ? gl_renderer : "Unknown");
        
        // Initialize HueSpace
        if (!init_huespace()) {
            spdlog::error("Failed to initialize HueSpace");
            return false;
        }
        
        // Setup OpenGL framebuffers
        if (!setup_framebuffers()) {
            spdlog::error("Failed to setup framebuffers");
            return false;
        }
        
        // Setup HueSpace render view
        if (!setup_render_view()) {
            spdlog::error("Failed to setup HueSpace render view");
            return false;
        }
        
        initialized_ = true;
        spdlog::info("✅ HueSpace renderer initialized successfully");
        
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception during renderer initialization: {}", e.what());
        return false;
    }
}

bool HueSpaceRenderer::load_vds(const std::string& vds_path) {
    if (!initialized_) {
        spdlog::error("Renderer not initialized");
        return false;
    }
    
    spdlog::info("📂 Loading VDS file: {}", vds_path);
    
    try {
        // Create HueSpace project and load VDS - reusing pattern from VdsReader
        auto* workspace = Hue::ProxyLib::Workspace::Instance();
        auto* scene = workspace->Scenes().Create();
        auto* project = scene->Projects().Create();
        
        if (!project) {
            spdlog::error("Failed to create HueSpace project");
            return false;
        }
        
        // Load VDS file
        auto* vds = project->VDSs().RestoreVDSFromFileName(vds_path);
        if (!vds) {
            spdlog::error("Failed to load VDS file: {}", vds_path);
            return false;
        }
        
        // Configure VDS for immediate access
        vds->SetCachePolicy(Hue::ProxyLib::VDSCachePolicy::TimeoutImmediately);
        
        huespace_vds_ = vds;
        vds_loaded_ = true;
        
        spdlog::info("✅ VDS loaded successfully");
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception loading VDS: {}", e.what());
        return false;
    }
}

bool HueSpaceRenderer::set_vds_object(Hue::ProxyLib::VDS* vds_object) {
    if (!initialized_) {
        spdlog::error("Renderer not initialized");
        return false;
    }
    
    if (!vds_object) {
        spdlog::error("VDS object is null");
        return false;
    }
    
    spdlog::info("🔗 Using shared VDS object for OpenGL rendering");
    
    // Use the already-loaded VDS object from VdsReader
    huespace_vds_ = vds_object;
    vds_loaded_ = true;
    
    spdlog::info("✅ VDS object set successfully");
    return true;
}

bool HueSpaceRenderer::render_frame(const SliceParams& slice_params, const CameraParams& camera_params) {
    if (!is_ready()) {
        spdlog::error("Renderer not ready for rendering");
        return false;
    }
    
    // Update parameters
    slice_params_ = slice_params;
    camera_params_ = camera_params;
    
    try {
        if (!make_current()) {
            spdlog::error("Failed to make context current");
            return false;
        }
        
        // Bind target framebuffer
        GLuint target_fb = render_config_.enable_msaa ? msaa_framebuffer_ : framebuffer_;
        glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
        glViewport(0, 0, render_config_.width, render_config_.height);
        
        // Clear buffers
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        
        // Enable depth testing and blending
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        // Update HueSpace camera
        update_camera_matrices();
        
        // Render using HueSpace engine
        render_huespace();
        
        // Resolve MSAA if enabled
        if (render_config_.enable_msaa) {
            resolve_msaa();
        }
        
        // Unbind framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        
        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            spdlog::error("OpenGL error during rendering: {}", error);
            return false;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception during rendering: {}", e.what());
        return false;
    }
}

bool HueSpaceRenderer::get_frame_rgba(std::vector<uint8_t>& out_data, int& out_width, int& out_height) {
    if (!is_ready()) {
        return false;
    }
    
    try {
        if (!make_current()) {
            return false;
        }
        
        out_width = render_config_.width;
        out_height = render_config_.height;
        out_data.resize(out_width * out_height * 4);
        
        // Read from resolved framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, out_width, out_height, GL_RGBA, GL_UNSIGNED_BYTE, out_data.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        
        // Flip vertically (OpenGL bottom-left origin to top-left origin)
        std::vector<uint8_t> temp_row(out_width * 4);
        for (int y = 0; y < out_height / 2; ++y) {
            int top_row = y * out_width * 4;
            int bottom_row = (out_height - 1 - y) * out_width * 4;
            
            std::memcpy(temp_row.data(), &out_data[top_row], out_width * 4);
            std::memcpy(&out_data[top_row], &out_data[bottom_row], out_width * 4);
            std::memcpy(&out_data[bottom_row], temp_row.data(), out_width * 4);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception getting frame data: {}", e.what());
        return false;
    }
}

bool HueSpaceRenderer::get_frame_rgb(std::vector<uint8_t>& out_data, int& out_width, int& out_height) {
    std::vector<uint8_t> rgba_data;
    if (!get_frame_rgba(rgba_data, out_width, out_height)) {
        return false;
    }
    
    // Convert RGBA to RGB
    out_data.resize(out_width * out_height * 3);
    for (int i = 0; i < out_width * out_height; ++i) {
        out_data[i * 3 + 0] = rgba_data[i * 4 + 0];  // R
        out_data[i * 3 + 1] = rgba_data[i * 4 + 1];  // G
        out_data[i * 3 + 2] = rgba_data[i * 4 + 2];  // B
    }
    
    return true;
}

void HueSpaceRenderer::update_slice_params(const SliceParams& params) {
    slice_params_ = params;
}

void HueSpaceRenderer::update_camera_params(const CameraParams& params) {
    camera_params_ = params;
}

bool HueSpaceRenderer::get_vds_dimensions(int& inline_count, int& crossline_count, int& time_count) {
    if (!vds_loaded_) {
        return false;
    }
    
    try {
        auto* vds = static_cast<Hue::ProxyLib::VDS*>(huespace_vds_);
        
        // Get VDS layout information
        const auto* layout = Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->GetVolumeDataLayout(*vds->GetHueObj());
        if (!layout) {
            return false;
        }
        
        // Extract dimensions
        inline_count = layout->GetDimensionNumSamples(0);    // X dimension
        crossline_count = layout->GetDimensionNumSamples(1); // Y dimension  
        time_count = layout->GetDimensionNumSamples(2);      // Z dimension
        
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception getting VDS dimensions: {}", e.what());
        return false;
    }
}

bool HueSpaceRenderer::get_vds_bounds(Vec3& min_coords, Vec3& max_coords) {
    if (!vds_loaded_) {
        return false;
    }
    
    try {
        auto* vds = static_cast<Hue::ProxyLib::VDS*>(huespace_vds_);
        
        // Get coordinate ranges for each dimension
        auto dim0_coords = vds->Dimension0Coordinate(); // X dimension
        auto dim1_coords = vds->Dimension1Coordinate(); // Y dimension  
        auto dim2_coords = vds->Dimension2Coordinate(); // Z dimension
        
        min_coords.x = static_cast<float>(dim0_coords.Min);
        min_coords.y = static_cast<float>(dim1_coords.Min);
        min_coords.z = static_cast<float>(dim2_coords.Min);
        
        max_coords.x = static_cast<float>(dim0_coords.Max);
        max_coords.y = static_cast<float>(dim1_coords.Max);
        max_coords.z = static_cast<float>(dim2_coords.Max);
        
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception getting VDS bounds: {}", e.what());
        return false;
    }
}

void HueSpaceRenderer::cleanup() {
    cleanup_gl();
    cleanup_huespace();
    
    initialized_ = false;
    vds_loaded_ = false;
}

// Private implementation methods

bool HueSpaceRenderer::create_gl_context() {
    spdlog::info("Creating headless OpenGL context...");
    
#ifdef __linux__
    // Open X11 display
    display_ = XOpenDisplay(":0");
    if (!display_) {
        display_ = XOpenDisplay(nullptr);  // Try default display
        if (!display_) {
            spdlog::error("Failed to open X11 display - ensure DISPLAY is set or Xvfb is running");
            return false;
        }
    }
    
    spdlog::info("✅ X11 display opened successfully");
    
    // Configure framebuffer attributes for off-screen rendering
    int fb_attribs[] = {
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_STENCIL_SIZE, 8,
        GLX_DOUBLEBUFFER, False,
        0
    };
    
    // Get framebuffer configurations
    int num_configs = 0;
    GLXFBConfig* fb_configs = glXChooseFBConfig(display_, DefaultScreen(display_), 
                                                fb_attribs, &num_configs);
    if (!fb_configs || num_configs == 0) {
        spdlog::error("No suitable GLX framebuffer configurations found");
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    fb_config_ = fb_configs[0];
    XFree(fb_configs);
    
    // Create OpenGL context using compatibility profile for HueSpace
    int context_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
        0
    };
    
    typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
    glXCreateContextAttribsARBProc glXCreateContextAttribsARB = 
        (glXCreateContextAttribsARBProc) glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    
    if (glXCreateContextAttribsARB) {
        glx_context_ = glXCreateContextAttribsARB(display_, fb_config_, nullptr, True, context_attribs);
    }
    
    if (!glx_context_) {
        spdlog::warn("Failed to create OpenGL 3.3 context, trying legacy...");
        glx_context_ = glXCreateNewContext(display_, fb_config_, GLX_RGBA_TYPE, nullptr, True);
    }
    
    if (!glx_context_) {
        spdlog::error("Failed to create GLX context");
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    // Get visual from FBConfig
    visual_info_ = glXGetVisualFromFBConfig(display_, fb_config_);
    if (!visual_info_) {
        spdlog::error("Failed to get visual from FBConfig");
        glXDestroyContext(display_, glx_context_);
        XCloseDisplay(display_);
        display_ = nullptr;
        glx_context_ = 0;
        return false;
    }
    
    // Create pixmap for offscreen rendering
    Window root = DefaultRootWindow(display_);
    pixmap_ = XCreatePixmap(display_, root, render_config_.width, render_config_.height, visual_info_->depth);
    if (!pixmap_) {
        spdlog::error("Failed to create X11 Pixmap");
        cleanup_gl();
        return false;
    }
    
    // Create GLX pixmap
    glx_pixmap_ = glXCreateGLXPixmap(display_, visual_info_, pixmap_);
    if (!glx_pixmap_) {
        spdlog::error("Failed to create GLX pixmap");
        cleanup_gl();
        return false;
    }
    
    spdlog::info("✅ GLX pixmap created: {}x{}", render_config_.width, render_config_.height);
    return true;
    
#elif defined(_WIN32)
    // Windows implementation would go here
    spdlog::error("Windows OpenGL context creation not implemented yet");
    return false;
#else
    spdlog::error("Unsupported platform for OpenGL context creation");
    return false;
#endif
}

bool HueSpaceRenderer::init_huespace() {
    try {
        spdlog::info("Initializing HueSpace integration...");
        
        // Create HueSpace proxy interface - same pattern as VdsReader
        auto* proxy = Hue::ProxyLib::ProxyInterfaceFactory::CreateProxyInterface();
        if (!proxy) {
            spdlog::error("Failed to create HueSpace proxy interface");
            return false;
        }
        
        huespace_interface_ = proxy;
        
        // Configure memory management for headless operation (simplified)
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetRegisterCUDACallback(false);
        // Note: CacheConfigurer methods may not be available in this SDK version
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetProcessingCPUCacheMax(512);
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetEnableProcessingThread0(true);
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetEnableProcessingThread1(true);
        
        spdlog::info("✅ HueSpace proxy interface initialized");
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Exception initializing HueSpace: {}", e.what());
        return false;
    }
}

bool HueSpaceRenderer::setup_framebuffers() {
    spdlog::info("Setting up OpenGL framebuffers...");
    
    // Generate framebuffer objects
    glGenFramebuffers(1, &framebuffer_);
    
    // Create color texture
    glGenTextures(1, &color_texture_);
    glBindTexture(GL_TEXTURE_2D, color_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, render_config_.width, render_config_.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Create depth texture
    glGenTextures(1, &depth_texture_);
    glBindTexture(GL_TEXTURE_2D, depth_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, render_config_.width, render_config_.height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    
    // Attach textures to framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth_texture_, 0);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::error("Main framebuffer incomplete");
        return false;
    }
    
    // Setup MSAA framebuffer if enabled
    if (render_config_.enable_msaa && render_config_.msaa_samples > 1) {
        glGenFramebuffers(1, &msaa_framebuffer_);
        
        // Create MSAA color texture
        glGenTextures(1, &msaa_color_texture_);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msaa_color_texture_);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, render_config_.msaa_samples, GL_RGBA8, render_config_.width, render_config_.height, GL_TRUE);
        
        // Create MSAA depth texture
        glGenTextures(1, &msaa_depth_texture_);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msaa_depth_texture_);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, render_config_.msaa_samples, GL_DEPTH24_STENCIL8, render_config_.width, render_config_.height, GL_TRUE);
        
        // Attach to MSAA framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, msaa_framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, msaa_color_texture_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, msaa_depth_texture_, 0);
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            spdlog::error("MSAA framebuffer incomplete");
            return false;
        }
        
        spdlog::info("✅ MSAA framebuffer setup with {} samples", render_config_.msaa_samples);
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    spdlog::info("✅ Framebuffers setup complete");
    return true;
}

bool HueSpaceRenderer::setup_render_view() {
    // For now, this is a placeholder - we'll integrate with HueSpace's render pipeline
    // The actual integration would involve creating HueSpace render views and viewers
    spdlog::info("✅ HueSpace render view setup (placeholder)");
    return true;
}

void HueSpaceRenderer::update_camera_matrices() {
    // Update HueSpace camera matrices based on camera_params_
    // This would involve setting up HueSpace viewer matrices
}

void HueSpaceRenderer::render_huespace() {
    // Check if VDS is loaded
    if (!huespace_vds_) {
        spdlog::warn("No VDS object available for rendering");
        // Render a red test pattern to indicate no VDS
        glClearColor(0.5f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return;
    }
    
    // For now, render VDS data directly using OpenGL
    // This is a simplified approach until we fully integrate HueSpace API
    
    try {
        // Get VDS dimensions
        // Cast void* to actual VDS type
        auto* vds_object = static_cast<Hue::ProxyLib::VDS*>(huespace_vds_);
        
        // Get VDS layout through VolumeDataAccessInterface
        const auto* layout = Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->GetVolumeDataLayout(*vds_object->GetHueObj());
        if (!layout) {
            spdlog::error("Failed to get VDS layout");
            return;
        }
        
        auto dim = layout->GetDimensionality();
        
        if (dim < 3) {
            spdlog::error("VDS has insufficient dimensions: {}", dim);
            return;
        }
        
        // Get axis descriptors and log them
        auto axis0 = layout->GetAxisDescriptor(0);
        auto axis1 = layout->GetAxisDescriptor(1);  
        auto axis2 = layout->GetAxisDescriptor(2);
        
        spdlog::info("Axis 0: {} samples", axis0.GetNumSamples());
        spdlog::info("Axis 1: {} samples", axis1.GetNumSamples());
        spdlog::info("Axis 2: {} samples", axis2.GetNumSamples());
        
        // Based on sample counts, determine which is which
        // We expect: Inline ~1400-1600, Crossline ~5700, Time/Depth ~1400-1600
        auto sample_axis = axis0;  // Assuming axis 0 is time (1408 samples)
        auto xline_axis = axis1;   // Assuming axis 1 is crossline (5701 samples)
        auto inline_axis = axis2;  // Assuming axis 2 is inline (1600 samples)
        
        int sample_count = sample_axis.GetNumSamples();   // Time samples
        int xline_count = xline_axis.GetNumSamples();     // Crossline samples
        int inline_count = inline_axis.GetNumSamples();   // Inline samples
        
        spdlog::info("VDS actual dimensions - Time: {}, Crossline: {}, Inline: {}", 
                    sample_count, xline_count, inline_count);
        
        // Determine which slice to read based on slice parameters
        int slice_idx = 0;
        int width = 0, height = 0;
        int x_axis = 0, y_axis = 0;
        
        if (slice_params_.show_inline && slice_params_.inline_index >= 0) {
            // Inline slice (crossline x time)
            // For testing, use a very safe small index
            slice_idx = 100;  // Use a small, safe index
            spdlog::info("Inline slice: Using SAFE slice index {}/{} (ignoring UI value {})", 
                        slice_idx, inline_count, slice_params_.inline_index);
            width = xline_count;
            height = sample_count;
            x_axis = 1;  // crossline
            y_axis = 0;  // time
        } else if (slice_params_.show_crossline && slice_params_.crossline_index >= 0) {
            // Crossline slice (inline x time)
            // For testing, use center slice
            slice_idx = xline_count / 2;  // Use center of crossline dimension (should be ~2850)
            spdlog::info("Crossline slice: Using CENTER slice index {}/{} (ignoring UI value {})", 
                        slice_idx, xline_count, slice_params_.crossline_index);
            width = inline_count;
            height = sample_count;
            x_axis = 2;  // inline
            y_axis = 0;  // time
        } else if (slice_params_.show_time && slice_params_.time_index >= 0) {
            // Time slice (inline x crossline)
            // For testing, use center slice
            slice_idx = sample_count / 2;  // Use center of time dimension (should be ~704)
            spdlog::info("Time slice: Using CENTER slice index {}/{} (ignoring UI value {})", 
                        slice_idx, sample_count, slice_params_.time_index);
            width = inline_count;
            height = xline_count;
            x_axis = 2;  // inline
            y_axis = 1;  // crossline
        } else {
            spdlog::warn("No valid slice specified");
            return;
        }
        
        // Downsample the slice to fit in our render target
        int render_width = render_config_.width;
        int render_height = render_config_.height;
        
        // Calculate aspect-preserving dimensions
        float slice_aspect = (float)width / (float)height;
        float frame_aspect = (float)render_width / (float)render_height;
        
        int target_width, target_height;
        if (slice_aspect > frame_aspect) {
            // Slice is wider - fit to width
            target_width = render_width;
            target_height = (int)(render_width / slice_aspect);
        } else {
            // Slice is taller - fit to height
            target_height = render_height;
            target_width = (int)(render_height * slice_aspect);
        }
        
        // Don't create slice_data yet - we'll create it after calculating actual dimensions
        std::vector<float> slice_data;
        
        // Use VDS API to read slice (simplified - normally would use VolumeDataPageAccessor)
        int min_bounds[6] = {0};
        int max_bounds[6] = {0};
        
        if (slice_params_.show_inline) {
            min_bounds[0] = max_bounds[0] = slice_idx;  // Fixed inline
            min_bounds[1] = 0; max_bounds[1] = xline_count;
            min_bounds[2] = 0; max_bounds[2] = sample_count;
        } else if (slice_params_.show_crossline) {
            min_bounds[0] = 0; max_bounds[0] = inline_count;
            min_bounds[1] = max_bounds[1] = slice_idx;  // Fixed crossline
            min_bounds[2] = 0; max_bounds[2] = sample_count;
        } else {
            min_bounds[0] = 0; max_bounds[0] = inline_count;
            min_bounds[1] = 0; max_bounds[1] = xline_count;
            min_bounds[2] = max_bounds[2] = slice_idx;  // Fixed time
        }
        
        // Read actual VDS data using HueSpace API
        // First read at appropriate downsample level for performance
        int downsample_level = 0;
        
        // Calculate optimal downsample level based on size
        while ((width >> downsample_level) > target_width * 2 && 
               (height >> downsample_level) > target_height * 2 && 
               downsample_level < 4) {
            downsample_level++;
        }
        
        // Calculate actual buffer size based on what we're requesting
        // The buffer size depends on the slice type and downsample level
        int buffer_size = 0;
        int actual_width = 0;
        int actual_height = 0;
        
        if (slice_params_.show_inline) {
            // Inline slice: we read full crossline × time
            actual_width = xline_count >> downsample_level;
            actual_height = sample_count >> downsample_level;
        } else if (slice_params_.show_crossline) {
            // Crossline slice: we read full inline × time
            actual_width = inline_count >> downsample_level;
            actual_height = sample_count >> downsample_level;
        } else {
            // Time slice: we read full inline × crossline
            actual_width = inline_count >> downsample_level;
            actual_height = xline_count >> downsample_level;
        }
        
        buffer_size = actual_width * actual_height;
        
        spdlog::info("Buffer allocation for downsample level {}: {}x{} = {} floats", 
                     downsample_level, actual_width, actual_height, buffer_size);
        
        // Recalculate target dimensions based on ACTUAL downsampled dimensions
        // to preserve aspect ratio correctly
        float actual_aspect = (float)actual_width / (float)actual_height;
        if (actual_aspect > frame_aspect) {
            // Slice is wider - fit to width
            target_width = render_width;
            target_height = (int)(render_width / actual_aspect);
        } else {
            // Slice is taller - fit to height
            target_height = render_height;
            target_width = (int)(render_height * actual_aspect);
        }
        
        // Allocate slice_data buffer with correct size
        slice_data.resize(target_width * target_height);
        
        spdlog::info("Target dimensions for rendering: {}x{} (buffer size: {})", 
                    target_width, target_height, slice_data.size());
        
        // Prepare buffer for VDS data with correct size
        std::vector<float> vds_buffer(buffer_size);
        
        // Set up read region coordinates (like the working sample code)
        int startRead[Hue::HueSpaceLib::VolumeDataLayout::Dimensionality_Max] = {0, 0, 0, 0, 0, 0};
        int endRead[Hue::HueSpaceLib::VolumeDataLayout::Dimensionality_Max] = {1, 1, 1, 1, 1, 1};
        
        // VDS dimensions are: [0]=Time, [1]=Crossline, [2]=Inline
        // Set up the correct slice bounds based on type
        // Note: Always use DimensionGroup012 for now, optimization can come later
        
        if (slice_params_.show_inline) {
            // Inline slice: fixed inline, vary crossline and time
            startRead[0] = 0;                  // Time start
            endRead[0] = sample_count;         // Time end (full range, exclusive)
            startRead[1] = 0;                  // Crossline start  
            endRead[1] = xline_count;          // Crossline end (full range, exclusive)
            startRead[2] = slice_idx;          // Inline position (fixed)
            endRead[2] = slice_idx + 1;        // End is exclusive, so +1 for single slice
            
            spdlog::info("Inline slice {}: reading time[{},{}), xline[{},{}), inline[{},{})",
                        slice_idx, startRead[0], endRead[0], startRead[1], endRead[1], 
                        startRead[2], endRead[2]);
            spdlog::info("Request parameters: startRead[2]={}, endRead[2]={} (should be different!)",
                        startRead[2], endRead[2]);
                        
        } else if (slice_params_.show_crossline) {
            // Crossline slice: fixed crossline, vary inline and time
            startRead[0] = 0;                  // Time start
            endRead[0] = sample_count;         // Time end (full range)
            startRead[1] = slice_idx;          // Crossline position (fixed)
            endRead[1] = slice_idx + 1;        // Single crossline slice
            startRead[2] = 0;                  // Inline start
            endRead[2] = inline_count;         // Inline end (full range)
            
            spdlog::info("Crossline slice {}: reading time[{},{}], xline[{},{}], inline[{},{}]",
                        slice_idx, startRead[0], endRead[0], startRead[1], endRead[1],
                        startRead[2], endRead[2]);
                        
        } else {
            // Time slice: fixed time, vary inline and crossline  
            startRead[0] = slice_idx;          // Time position (fixed)
            endRead[0] = slice_idx + 1;        // Single time slice
            startRead[1] = 0;                  // Crossline start
            endRead[1] = xline_count;          // Crossline end (full range)
            startRead[2] = 0;                  // Inline start
            endRead[2] = inline_count;         // Inline end (full range)
            
            spdlog::info("Time slice {}: reading time[{},{}], xline[{},{}], inline[{},{}]",
                        slice_idx, startRead[0], endRead[0], startRead[1], endRead[1],
                        startRead[2], endRead[2]);
        }
        
        // Request VDS data using the same pattern as the working sample
        auto* vda = Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface();
        
        spdlog::info("About to call RequestVolumeSubset with:");
        spdlog::info("  startRead: [{}, {}, {}, {}, {}, {}]", 
                    startRead[0], startRead[1], startRead[2], startRead[3], startRead[4], startRead[5]);
        spdlog::info("  endRead: [{}, {}, {}, {}, {}, {}]",
                    endRead[0], endRead[1], endRead[2], endRead[3], endRead[4], endRead[5]);
        spdlog::info("  downsample_level: {}, channel: 0", downsample_level);
        
        // Use RequestVolumeSubset like the working sample (NOT RequestVolumeSamples)
        auto requestID = vda->RequestVolumeSubset(
            vds_buffer.data(),           // Output buffer
            layout,                      // VDS layout
            Hue::HueSpaceLib::DimensionGroup012,  // Standard dimension group
            downsample_level,            // LOD level
            0,                          // Channel 0
            startRead,                  // Start coordinates
            endRead,                    // End coordinates (exclusive - must be > start)
            Hue::HueSpaceLib::DataBlock::Format_R32  // Float format
        );
        
        // Wait for data to be available
        bool success = vda->WaitForCompletion(requestID);
        
        if (!success) {
            spdlog::warn("Failed to read VDS data, using synthetic fallback");
            // Generate simple synthetic pattern as fallback
            for (int y = 0; y < actual_height; ++y) {
                for (int x = 0; x < actual_width; ++x) {
                    float fx = (float)x / actual_width * 30.0f;
                    float fy = (float)y / actual_height * 10.0f;
                    vds_buffer[y * actual_width + x] = sin(fx) * cos(fy);
                }
            }
        } else {
            spdlog::info("Successfully read VDS data: {}x{} samples", actual_width, actual_height);
        }
        
        // Safety check - ensure buffers are correctly sized
        if (vds_buffer.size() != actual_width * actual_height) {
            spdlog::error("VDS buffer size mismatch! Expected {}, got {}", 
                         actual_width * actual_height, vds_buffer.size());
            return;
        }
        
        spdlog::info("Starting resampling from {}x{} to {}x{}", 
                    actual_width, actual_height, target_width, target_height);
        
        // Sanity check before resampling
        if (target_width <= 0 || target_height <= 0) {
            spdlog::error("Invalid target dimensions: {}x{}", target_width, target_height);
            return;
        }
        
        if (actual_width <= 0 || actual_height <= 0) {
            spdlog::error("Invalid actual dimensions: {}x{}", actual_width, actual_height);
            return;
        }
        
        // Now resample to target dimensions - using simpler nearest neighbor first to isolate issue
        for (int y = 0; y < target_height; ++y) {
            for (int x = 0; x < target_width; ++x) {
                // Simple nearest neighbor sampling to test
                int src_x = (x * actual_width) / target_width;
                int src_y = (y * actual_height) / target_height;
                
                // Clamp to valid range
                src_x = std::min(src_x, actual_width - 1);
                src_y = std::min(src_y, actual_height - 1);
                
                int src_idx = src_y * actual_width + src_x;
                int dst_idx = y * target_width + x;
                
                if (src_idx >= 0 && src_idx < vds_buffer.size() && 
                    dst_idx >= 0 && dst_idx < slice_data.size()) {
                    slice_data[dst_idx] = vds_buffer[src_idx];
                }
            }
        }
        
        // Skip bilinear interpolation for now
        /*
                // Bilinear interpolation from VDS buffer to target
                float src_x = (float)x * (actual_width - 1) / (target_width - 1);
                float src_y = (float)y * (actual_height - 1) / (target_height - 1);
                
        */
        
        // Find min/max for normalization
        float min_val = *std::min_element(slice_data.begin(), slice_data.end());
        float max_val = *std::max_element(slice_data.begin(), slice_data.end());
        float range = max_val - min_val;
        if (range < 0.001f) range = 1.0f;
        
        spdlog::debug("Slice data range: [{}, {}]", min_val, max_val);
        
        // Convert to RGBA using seismic colormap
        std::vector<uint8_t> rgba_data(target_width * target_height * 4);
        for (int i = 0; i < target_width * target_height; ++i) {
            float normalized = (slice_data[i] - min_val) / range;
            normalized = std::max(0.0f, std::min(1.0f, normalized));
            
            // Apply gain and brightness
            normalized = normalized * slice_params_.amplitude_scale + slice_params_.brightness;
            normalized = std::max(0.0f, std::min(1.0f, normalized));
            
            // Simple red-white-blue seismic colormap
            uint8_t r, g, b;
            if (normalized < 0.5f) {
                // Blue to white
                float t = normalized * 2.0f;
                r = (uint8_t)(t * 255);
                g = (uint8_t)(t * 255);
                b = 255;
            } else {
                // White to red
                float t = (normalized - 0.5f) * 2.0f;
                r = 255;
                g = (uint8_t)((1.0f - t) * 255);
                b = (uint8_t)((1.0f - t) * 255);
            }
            
            rgba_data[i * 4 + 0] = r;
            rgba_data[i * 4 + 1] = g;
            rgba_data[i * 4 + 2] = b;
            rgba_data[i * 4 + 3] = 255;
        }
        
        // Create OpenGL texture from slice data
        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target_width, target_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        
        // Render texture to framebuffer
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);
        
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1, 1, -1, 1, -1, 1);
        
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        
        // Draw fullscreen quad
        glBegin(GL_QUADS);
        glColor3f(1, 1, 1);
        glTexCoord2f(0, 1); glVertex2f(-1, -1);
        glTexCoord2f(1, 1); glVertex2f( 1, -1);
        glTexCoord2f(1, 0); glVertex2f( 1,  1);
        glTexCoord2f(0, 0); glVertex2f(-1,  1);
        glEnd();
        
        glDisable(GL_TEXTURE_2D);
        glDeleteTextures(1, &texture);
        
        spdlog::info("✅ Rendered VDS slice: type={}, index={}, original={}x{}, rendered={}x{}", 
                    slice_params_.show_inline ? "inline" : 
                    (slice_params_.show_crossline ? "crossline" : "time"),
                    slice_idx, width, height, target_width, target_height);
        
    } catch (const std::exception& e) {
        spdlog::error("Exception rendering VDS slice: {}", e.what());
        // Render error pattern
        glClearColor(0.5f, 0.0f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
}

void HueSpaceRenderer::resolve_msaa() {
    if (!render_config_.enable_msaa || render_config_.msaa_samples <= 1) {
        return;
    }
    
    // Blit from MSAA framebuffer to regular framebuffer
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_framebuffer_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer_);
    
    glBlitFramebuffer(0, 0, render_config_.width, render_config_.height,
                      0, 0, render_config_.width, render_config_.height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool HueSpaceRenderer::make_current() {
#ifdef __linux__
    if (glx_pixmap_ && glx_context_ != 0) {
        return glXMakeCurrent(display_, glx_pixmap_, glx_context_);
    }
    return false;
#elif defined(_WIN32)
    return wglMakeCurrent(device_context_, gl_context_);
#else
    return false;
#endif
}

bool HueSpaceRenderer::release_context() {
#ifdef __linux__
    return glXMakeContextCurrent(display_, 0, 0, 0);
#elif defined(_WIN32)
    return wglMakeCurrent(nullptr, nullptr);
#else
    return false;
#endif
}

void HueSpaceRenderer::cleanup_gl() {
    if (make_current()) {
        if (msaa_framebuffer_) {
            glDeleteFramebuffers(1, &msaa_framebuffer_);
            msaa_framebuffer_ = 0;
        }
        if (msaa_color_texture_) {
            glDeleteTextures(1, &msaa_color_texture_);
            msaa_color_texture_ = 0;
        }
        if (msaa_depth_texture_) {
            glDeleteTextures(1, &msaa_depth_texture_);
            msaa_depth_texture_ = 0;
        }
        if (framebuffer_) {
            glDeleteFramebuffers(1, &framebuffer_);
            framebuffer_ = 0;
        }
        if (color_texture_) {
            glDeleteTextures(1, &color_texture_);
            color_texture_ = 0;
        }
        if (depth_texture_) {
            glDeleteTextures(1, &depth_texture_);
            depth_texture_ = 0;
        }
        
        release_context();
    }

#ifdef __linux__
    if (glx_pixmap_) {
        glXDestroyGLXPixmap(display_, glx_pixmap_);
        glx_pixmap_ = 0;
    }
    
    if (pixmap_) {
        XFreePixmap(display_, pixmap_);
        pixmap_ = 0;
    }
    
    if (visual_info_) {
        XFree(visual_info_);
        visual_info_ = nullptr;
    }
    
    if (glx_context_) {
        glXDestroyContext(display_, glx_context_);
        glx_context_ = 0;
    }
    
    if (display_) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
#elif defined(_WIN32)
    if (gl_context_) {
        wglDeleteContext(gl_context_);
        gl_context_ = nullptr;
    }
    
    if (device_context_) {
        ReleaseDC(window_handle_, device_context_);
        device_context_ = nullptr;
    }
    
    if (window_handle_) {
        DestroyWindow(window_handle_);
        window_handle_ = nullptr;
    }
#endif
}

void HueSpaceRenderer::cleanup_huespace() {
    // Cleanup HueSpace resources
    huespace_viewer_ = nullptr;
    huespace_render_client_ = nullptr;
    huespace_render_view_ = nullptr;
    huespace_vds_ = nullptr;
    
    if (huespace_interface_) {
        // HueSpace proxy interface cleanup is handled by the framework
        huespace_interface_ = nullptr;
    }
}

} // namespace remoteview