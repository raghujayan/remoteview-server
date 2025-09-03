#!/bin/bash

# RemoteView Server Complete Deployment and Startup Script
# Synchronizes local code, builds remotely, and starts server

set -euo pipefail

# Configuration
REMOTE_USER="rocky"
REMOTE_HOST="10.7.4.116"
SSH_KEY="~/.ssh/key.pem"
REMOTE_DIR="/home/rocky/remoteview-server"
HUESPACE_LICENSE="5053@license.cloud.bluware.com"
VDS_FILE_PATH="/home/rocky/onnia2x3d_mig_Time.vds"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

# Check prerequisites
check_prerequisites() {
    log_step "Checking prerequisites..."
    
    if [[ ! -f ~/.ssh/key.pem ]]; then
        log_error "SSH key not found at ~/.ssh/key.pem"
        exit 1
    fi
    
    if ! command -v ssh &> /dev/null; then
        log_error "SSH command not found"
        exit 1
    fi
    
    if ! command -v rsync &> /dev/null; then
        log_error "rsync command not found"
        exit 1
    fi
    
    log_info "Prerequisites check passed"
}

# Stop existing server
stop_existing_server() {
    log_step "Stopping any existing RemoteView server..."
    
    ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "
        if pgrep -f remoteview_server > /dev/null; then
            echo 'Stopping existing server...'
            pkill -f remoteview_server
            sleep 2
            echo 'Server stopped'
        else
            echo 'No existing server found'
        fi
    " || log_warn "Failed to stop existing server (may not be running)"
}

# Sync local code to remote
sync_code() {
    log_step "Synchronizing local code to remote server..."
    
    # Create remote directory structure
    ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "
        mkdir -p ${REMOTE_DIR}
        mkdir -p ${REMOTE_DIR}/build
        mkdir -p ${REMOTE_DIR}/logs
    "
    
    # Sync source code (excluding build artifacts and git)
    log_info "Syncing source code..."
    rsync -avz --progress \
        --exclude='build/' \
        --exclude='.git/' \
        --exclude='*.o' \
        --exclude='*.a' \
        --exclude='remoteview_server' \
        --exclude='server.log' \
        --exclude='*.log' \
        -e "ssh -i ${SSH_KEY}" \
        ./ "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}/"
    
    log_info "Code synchronization complete"
}

# Build server on remote
build_server() {
    log_step "Building RemoteView server on remote machine..."
    
    ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "
        cd ${REMOTE_DIR}
        
        # Set HueSpace license
        export HUE_LICENSE_FILE=${HUESPACE_LICENSE}
        echo 'HueSpace license set: '\$HUE_LICENSE_FILE
        
        # Check VDS file
        if [[ -f \"${VDS_FILE_PATH}\" ]]; then
            echo 'VDS file found: ${VDS_FILE_PATH}'
            stat \"${VDS_FILE_PATH}\" | grep -E '(Size|Access)'
        else
            echo 'WARNING: VDS file not found at ${VDS_FILE_PATH}'
        fi
        
        # Clean and create build directory
        rm -rf build
        mkdir -p build
        cd build
        
        # Configure with CMake
        echo 'Configuring with CMake...'
        cmake .. \\
            -DCMAKE_BUILD_TYPE=Release \\
            -DCMAKE_CXX_STANDARD=20 \\
            -DBUILD_TESTS=ON \\
            -DCMAKE_INSTALL_PREFIX=/home/rocky/bin
        
        # Build the project
        echo 'Building RemoteView server...'
        make -j\$(nproc)
        
        # Check if build succeeded
        if [[ -x remoteview_server ]]; then
            echo 'Build completed successfully!'
            ls -la remoteview_server
            file remoteview_server
        else
            echo 'Build failed - executable not found'
            exit 1
        fi
    "
    
    log_info "Server build complete"
}

# Start server
start_server() {
    log_step "Starting RemoteView server..."
    
    ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "
        cd ${REMOTE_DIR}
        
        # Set license and start server in background
        export HUE_LICENSE_FILE=${HUESPACE_LICENSE}
        
        # Start server with logging
        echo 'Starting RemoteView server...'
        nohup ./build/remoteview_server config.example.json > server.log 2>&1 &
        
        # Give it a moment to start
        sleep 3
        
        # Check if server started successfully
        if pgrep -f remoteview_server > /dev/null; then
            echo 'Server started successfully!'
            echo 'PID:' \$(pgrep -f remoteview_server)
        else
            echo 'Server failed to start - checking logs:'
            tail -20 server.log
            exit 1
        fi
    "
    
    log_info "Server startup complete"
}

# Verify server status
verify_server() {
    log_step "Verifying server status..."
    
    # Check server process
    log_info "Checking server process..."
    ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "ps aux | grep remoteview_server | grep -v grep"
    
    # Check server logs
    log_info "Recent server logs:"
    ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "cd ${REMOTE_DIR} && tail -10 server.log"
    
    # Test metrics endpoint
    log_info "Testing metrics endpoint..."
    sleep 2
    if curl -s --connect-timeout 5 http://${REMOTE_HOST}:9090/metrics > /dev/null; then
        log_info "✓ Metrics endpoint accessible"
    else
        log_warn "✗ Metrics endpoint not accessible (may need SSH tunnel)"
    fi
    
    log_info "Server verification complete"
}

# Main execution
main() {
    echo "=================================================="
    echo "RemoteView Server Deployment and Startup Script"
    echo "=================================================="
    echo "Remote: ${REMOTE_USER}@${REMOTE_HOST}"
    echo "License: ${HUESPACE_LICENSE}"
    echo "VDS: ${VDS_FILE_PATH}"
    echo ""
    
    check_prerequisites
    stop_existing_server
    sync_code
    build_server
    start_server
    verify_server
    
    echo ""
    echo "=================================================="
    log_info "RemoteView Server deployment complete!"
    echo "=================================================="
    echo "Server Status:"
    echo "  - WebSocket: ${REMOTE_HOST}:8080"
    echo "  - Metrics: ${REMOTE_HOST}:9090"
    echo "  - Logs: ${REMOTE_DIR}/server.log"
    echo ""
    echo "Next steps:"
    echo "  1. Run ./setup_ssh_tunnels.sh to establish client connectivity"
    echo "  2. Run ./start_client.sh to launch the web client"
    echo "  3. Or run ./launch_remoteview_system.sh for complete setup"
    echo ""
}

# Handle script interruption
trap 'log_error "Script interrupted"; exit 1' INT TERM

# Run main function
main "$@"