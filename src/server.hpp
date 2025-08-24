#pragma once

#include "config/config.hpp"
#include "vds_access/vds_reader.hpp"
#include "tile_cache/cache.hpp"
#include "rtc_channel/webrtc_server.hpp"
#include "metrics/metrics_collector.hpp"
#include "adaptivity/adapt.hpp"
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

private:
    void setup_signal_handlers();
    void main_loop();
    
    std::shared_ptr<Config> config_;
    std::unique_ptr<VdsReader> vds_reader_;
    std::unique_ptr<TileCache> tile_cache_;
    std::unique_ptr<WebRtcServer> webrtc_server_;
    std::unique_ptr<MetricsCollector> metrics_;
    std::unique_ptr<Adapt> adaptivity_;
    
    TestingHooks testing_hooks_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
};

} // namespace remoteview