#pragma once

#include "config/config.hpp"
#include <memory>
#include <atomic>

namespace remoteview {

class MetricsCollector {
public:
    explicit MetricsCollector(std::shared_ptr<Config> config);
    ~MetricsCollector();
    
    void start();
    void stop();
    void collect();
    bool is_running() const { return running_.load(); }

private:
    std::shared_ptr<Config> config_;
    std::atomic<bool> running_{false};
    
    // TODO: Add actual metrics implementation members
};

} // namespace remoteview