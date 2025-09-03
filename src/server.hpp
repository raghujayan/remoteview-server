#pragma once

#include "config/config.hpp"
#include "vds_access/vds_reader.hpp"
#include "tile_cache/cache.hpp"
#include "rtc_channel/webrtc_server.hpp"
#include "metrics/metrics_collector.hpp"
#include "adaptivity/adapt.hpp"
#include "opengl_renderer/huespace_renderer.hpp"
#include <memory>
#include <atomic>

namespace remoteview {

struct TestingHooks {
    bool enabled = false;
    uint32_t roi_inline_start = 0;
    uint32_t roi_inline_end = 0;
    uint32_t roi_xline_start = 0;
    uint32_t roi_xline_end = 0;
    uint32_t roi_z_start = 0;
    uint32_t roi_z_end = 0;
    bool enable_tile_dump = false;
    std::string dump_directory = "./tile_dumps";
    bool enable_performance_profiling = false;
    uint32_t max_test_tiles = 100;
    
    bool is_in_roi(uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx) const {
        if (roi_inline_end == 0 && roi_xline_end == 0 && roi_z_end == 0) {
            return true; // No ROI set, allow all
        }
        return inline_idx >= roi_inline_start && inline_idx <= roi_inline_end &&
               xline_idx >= roi_xline_start && xline_idx <= roi_xline_end &&
               z_idx >= roi_z_start && z_idx <= roi_z_end;
    }
};

class Server {
public:
    explicit Server(std::shared_ptr<Config> config);
    ~Server();
    
    void start();
    void stop();
    bool is_running() const { return running_.load(); }
    
    /**
     * Handle slice change from client - triggers prefetch cancellation
     * @param inline_idx New inline slice index
     * @param xline_idx New crossline slice index
     * @param z_idx New time/depth slice index
     */
    void handle_slice_change(uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx);
    
    /**
     * Configure testing hooks for development and QA
     * @param hooks Testing configuration including ROI, dumping, profiling
     */
    void configure_testing_hooks(const TestingHooks& hooks);
    
    /**
     * Get VDS reader for tile generation
     * @return Pointer to VDS reader instance
     */
    VdsReader* get_vds_reader() const { return vds_reader_.get(); }
    
    /**
     * Generate actual VDS tile data for client
     * @param plane_index 0=inline, 1=crossline, 2=timedepth
     * @param slice_index Slice number within the plane
     * @param tile_x X offset in slice coordinates
     * @param tile_y Y offset in slice coordinates  
     * @param tile_w Tile width in pixels
     * @param tile_h Tile height in pixels
     * @return Vector of tile data bytes, empty if failed
     */
    std::vector<uint8_t> read_vds_tile_data(uint32_t plane_index, uint32_t slice_index,
                                           uint32_t tile_x, uint32_t tile_y, 
                                           uint32_t tile_w, uint32_t tile_h);
    
    /**
     * Generate and send VDS tiles for all active sessions
     * @param session_id Client session to send tiles to
     * @param inline_idx Inline slice index
     * @param xline_idx Crossline slice index  
     * @param z_idx Time/depth slice index
     */
    void generate_and_send_vds_tiles(const std::string& session_id, 
                                     uint32_t inline_idx, uint32_t xline_idx, uint32_t z_idx);
    
    /**
     * Test server-side OpenGL rendering with HueSpace
     * @return true if rendering test successful
     */
    bool test_opengl_rendering();

private:
    void setup_signal_handlers();
    void main_loop();
    
    std::shared_ptr<Config> config_;
    std::unique_ptr<VdsReader> vds_reader_;
    std::unique_ptr<TileCache> tile_cache_;
    std::unique_ptr<WebRtcServer> webrtc_server_;
    std::unique_ptr<MetricsCollector> metrics_;
    std::unique_ptr<Adapt> adaptivity_;
    std::unique_ptr<HueSpaceRenderer> opengl_renderer_;
    
    TestingHooks testing_hooks_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
};

} // namespace remoteview