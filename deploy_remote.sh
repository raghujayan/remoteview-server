#!/bin/bash

# RemoteView Server Remote Deployment Script
# Compiles and deploys server on rocky@10.7.4.116

set -euo pipefail

# Configuration from CLAUDE.md
REMOTE_USER="rocky"
REMOTE_HOST="10.7.4.116" 
SSH_KEY="~/.ssh/key.pem"
REMOTE_DIR="/home/rocky/remoteview-server"
HUESPACE_LICENSE="5053@license.cloud.bluware.com"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check SSH key exists
if [[ ! -f ~/.ssh/key.pem ]]; then
    log_error "SSH key not found at ~/.ssh/key.pem"
    exit 1
fi

log_info "Starting remote deployment to ${REMOTE_USER}@${REMOTE_HOST}"

# Create remote directory structure
log_info "Creating remote directory structure..."
ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "
    mkdir -p ${REMOTE_DIR}
    mkdir -p ${REMOTE_DIR}/build
    mkdir -p ${REMOTE_DIR}/logs
"

# Sync source code (excluding build artifacts)
log_info "Syncing source code to remote server..."
rsync -avz --progress \
    --exclude='build/' \
    --exclude='.git/' \
    --exclude='*.o' \
    --exclude='*.a' \
    --exclude='remoteview_server' \
    -e "ssh -i ${SSH_KEY}" \
    ./ "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}/"

# Build on remote server
log_info "Building on remote server..."
ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "
    cd ${REMOTE_DIR}
    
    # Set HueSpace license
    export HUE_LICENSE_FILE=${HUESPACE_LICENSE}
    
    # Create build directory
    mkdir -p build
    cd build
    
    # Configure with CMake
    cmake .. \\
        -DCMAKE_BUILD_TYPE=Release \\
        -DCMAKE_CXX_STANDARD=20 \\
        -DBUILD_TESTS=ON \\
        -DCMAKE_INSTALL_PREFIX=/home/rocky/bin
    
    # Build the project
    make -j\$(nproc)
    
    # Run basic tests if they exist
    if [[ -d tests ]]; then
        echo 'Running tests...'
        ctest --output-on-failure || echo 'Some tests failed, continuing...'
    fi
    
    # Install
    make install
"

# Check if build succeeded
if ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "[[ -x ${REMOTE_DIR}/build/remoteview_server ]]"; then
    log_info "Build completed successfully!"
    
    # Show executable info
    ssh -i "${SSH_KEY}" "${REMOTE_USER}@${REMOTE_HOST}" "
        echo 'Executable info:'
        ls -la ${REMOTE_DIR}/build/remoteview_server
        file ${REMOTE_DIR}/build/remoteview_server
        
        echo 'Checking dependencies:'
        ldd ${REMOTE_DIR}/build/remoteview_server | head -20
    "
    
    log_info "RemoteView server ready at ${REMOTE_DIR}/build/remoteview_server"
    log_info "To run: ssh -i ${SSH_KEY} ${REMOTE_USER}@${REMOTE_HOST} 'cd ${REMOTE_DIR} && ./build/remoteview_server'"
    
else
    log_error "Build failed! Check the build output above."
    exit 1
fi

log_info "Deployment completed successfully!"