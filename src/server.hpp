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

class Server {
public:
    explicit Server(std::shared_ptr<Config> config);
    ~Server();
    
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

private:
    void setup_signal_handlers();
    void main_loop();
    
    std::shared_ptr<Config> config_;
    std::unique_ptr<VdsReader> vds_reader_;
    std::unique_ptr<TileCache> tile_cache_;
    std::unique_ptr<WebRtcServer> webrtc_server_;
    std::unique_ptr<MetricsCollector> metrics_;
    std::unique_ptr<Adapt> adaptivity_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
};

} // namespace remoteview