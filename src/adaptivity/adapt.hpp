#pragma once

#include "config/config.hpp"
#include <memory>

namespace remoteview {

class Adapt {
public:
    explicit Adapt(std::shared_ptr<Config> config);
    ~Adapt();
    
    void initialize();
    void shutdown();

private:
    std::shared_ptr<Config> config_;
    
    // TODO: Add actual adaptivity implementation members
};

} // namespace remoteview