#pragma once

#include "config/config.hpp"
#include <memory>
#include <string>
#include <vector>

// OpenGL and system headers - include after HueSpace to avoid macro conflicts
#ifdef __linux__
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
// Prevent X11 macros from conflicting with HueSpace
#ifdef None
#undef None
#endif
#ifdef Success
#undef Success
#endif
#elif defined(_WIN32)
#include <GL/gl.h>
#include <windows.h>
#endif

// Simple 3D math structures (no external dependencies)
struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

// Forward declare HueSpace types to avoid heavy includes
namespace Hue {
namespace ProxyLib {
    class IProxyInterface;
    class VDS;
    class RenderView;
    class RenderViewClient;
    class Viewer;
}
namespace HueSpaceLib {
    class RenderInstance;
}
}

namespace remoteview {

/**
 * @brief HueSpace OpenGL renderer for server-side seismic visualization
 * 
 * This class integrates HueSpace's 3D visualization engine with a headless 
 * OpenGL context for server-side rendering. It produces high-quality seismic
 * visualizations that can be streamed to clients as images/video instead of tiles.
 */
class HueSpaceRenderer {
public:
    struct RenderConfig {
        int width = 1920;                    ///< Render target width
        int height = 1080;                   ///< Render target height
        float fov = 45.0f;                   ///< Field of view in degrees
        float near_clip = 0.1f;              ///< Near clipping plane
        float far_clip = 10000.0f;           ///< Far clipping plane
        bool enable_msaa = true;             ///< Enable multi-sampling anti-aliasing
        int msaa_samples = 4;                ///< MSAA sample count
        bool enable_oit = true;              ///< Enable order-independent transparency
        std::string colormap = "seismic";    ///< Colormap name
    };

    struct SliceParams {
        int inline_index = 0;                ///< Inline slice index
        int crossline_index = 0;             ///< Crossline slice index
        int time_index = 0;                  ///< Time/depth slice index
        bool show_inline = true;             ///< Show inline slice
        bool show_crossline = true;          ///< Show crossline slice  
        bool show_time = true;               ///< Show time/depth slice
        float opacity = 1.0f;                ///< Overall opacity
        float amplitude_scale = 1.0f;        ///< Amplitude scaling factor
        float contrast = 1.0f;               ///< Contrast adjustment
        float brightness = 0.0f;             ///< Brightness adjustment
    };

    struct CameraParams {
        Vec3 position = {0, 0, 5};           ///< Camera position
        Vec3 target = {0, 0, 0};             ///< Look-at target
        Vec3 up = {0, 1, 0};                 ///< Up vector
        float zoom = 1.0f;                   ///< Zoom factor
    };

    explicit HueSpaceRenderer(std::shared_ptr<Config> config);
    ~HueSpaceRenderer();

    /**
     * @brief Initialize the renderer with HueSpace integration
     * 
     * @param render_config Render configuration
     * @return true if initialization successful
     */
    bool initialize(const RenderConfig& render_config);

    /**
     * @brief Load VDS file for rendering
     * 
     * @param vds_path Path to VDS file
     * @return true if loaded successfully
     */
    bool load_vds(const std::string& vds_path);
    
    /**
     * @brief Use an already-loaded VDS object for rendering
     * 
     * @param vds_object Pointer to loaded VDS object
     * @return true if set successfully
     */
    bool set_vds_object(Hue::ProxyLib::VDS* vds_object);

    /**
     * @brief Render current scene to framebuffer
     * 
     * @param slice_params Slice rendering parameters
     * @param camera_params Camera parameters
     * @return true if render successful
     */
    bool render_frame(const SliceParams& slice_params, const CameraParams& camera_params);

    /**
     * @brief Get rendered frame as RGB data
     * 
     * @param out_data Output buffer for RGB data (width * height * 3 bytes)
     * @param out_width Width of rendered frame
     * @param out_height Height of rendered frame
     * @return true if frame data retrieved successfully
     */
    bool get_frame_rgb(std::vector<uint8_t>& out_data, int& out_width, int& out_height);

    /**
     * @brief Get rendered frame as RGBA data
     * 
     * @param out_data Output buffer for RGBA data (width * height * 4 bytes)
     * @param out_width Width of rendered frame
     * @param out_height Height of rendered frame
     * @return true if frame data retrieved successfully
     */
    bool get_frame_rgba(std::vector<uint8_t>& out_data, int& out_width, int& out_height);

    /**
     * @brief Update slice parameters
     * 
     * @param params New slice parameters
     */
    void update_slice_params(const SliceParams& params);

    /**
     * @brief Update camera parameters
     * 
     * @param params New camera parameters
     */
    void update_camera_params(const CameraParams& params);

    /**
     * @brief Get VDS dimensions
     * 
     * @param inline_count Number of inline slices
     * @param crossline_count Number of crossline slices
     * @param time_count Number of time/depth slices
     * @return true if VDS is loaded and dimensions retrieved
     */
    bool get_vds_dimensions(int& inline_count, int& crossline_count, int& time_count);

    /**
     * @brief Get VDS bounding box
     * 
     * @param min_coords Minimum coordinates (x, y, z)
     * @param max_coords Maximum coordinates (x, y, z)
     * @return true if VDS is loaded and bounds retrieved
     */
    bool get_vds_bounds(Vec3& min_coords, Vec3& max_coords);

    /**
     * @brief Cleanup resources
     */
    void cleanup();

    /**
     * @brief Check if renderer is initialized and ready
     * 
     * @return true if ready for rendering
     */
    bool is_ready() const { return initialized_ && vds_loaded_; }

private:
    // Configuration
    std::shared_ptr<Config> config_;
    RenderConfig render_config_;
    SliceParams slice_params_;
    CameraParams camera_params_;

    // State
    bool initialized_ = false;
    bool vds_loaded_ = false;

    // OpenGL context and resources
#ifdef __linux__
    Display* display_ = nullptr;
    GLXContext glx_context_ = 0;  // GLXContext is unsigned long, not pointer
    GLXPixmap glx_pixmap_ = 0;
    Pixmap pixmap_ = 0;
    XVisualInfo* visual_info_ = nullptr;
    GLXFBConfig fb_config_ = nullptr;
#elif defined(_WIN32)
    HGLRC gl_context_ = nullptr;
    HDC device_context_ = nullptr;
    HWND window_handle_ = nullptr;
#endif

    // OpenGL framebuffer objects
    GLuint framebuffer_ = 0;
    GLuint color_texture_ = 0;
    GLuint depth_texture_ = 0;
    GLuint msaa_framebuffer_ = 0;
    GLuint msaa_color_texture_ = 0;
    GLuint msaa_depth_texture_ = 0;

    // HueSpace objects (using void* to avoid heavy includes in header)
    void* huespace_interface_ = nullptr;        // Hue::ProxyLib::IProxyInterface*
    void* huespace_vds_ = nullptr;              // Hue::ProxyLib::VDS*
    void* huespace_render_view_ = nullptr;      // Hue::ProxyLib::RenderView*
    void* huespace_render_client_ = nullptr;    // Hue::ProxyLib::RenderViewClient*
    void* huespace_viewer_ = nullptr;           // Hue::ProxyLib::Viewer*

    /**
     * @brief Create headless OpenGL context
     * 
     * @return true if successful
     */
    bool create_gl_context();

    /**
     * @brief Initialize HueSpace proxy interface
     * 
     * @return true if successful
     */
    bool init_huespace();

    /**
     * @brief Setup OpenGL framebuffers for rendering
     * 
     * @return true if successful
     */
    bool setup_framebuffers();

    /**
     * @brief Setup HueSpace render view and viewer
     * 
     * @return true if successful
     */
    bool setup_render_view();

    /**
     * @brief Update HueSpace camera matrices
     */
    void update_camera_matrices();

    /**
     * @brief Render using HueSpace engine
     */
    void render_huespace();

    /**
     * @brief Resolve MSAA framebuffer if enabled
     */
    void resolve_msaa();

    /**
     * @brief Make OpenGL context current
     * 
     * @return true if successful
     */
    bool make_current();

    /**
     * @brief Release OpenGL context
     * 
     * @return true if successful
     */
    bool release_context();

    /**
     * @brief Cleanup OpenGL resources
     */
    void cleanup_gl();

    /**
     * @brief Cleanup HueSpace resources
     */
    void cleanup_huespace();
};

} // namespace remoteview