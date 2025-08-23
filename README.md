# RemoteView Server

Binary-tile streaming seismic viewer server implementation in C++20.

## Architecture Overview

RemoteView server streams **binary amplitude tiles** (not video) from a GPU-capable Rocky Linux server to web browsers. The system uses WebRTC DataChannel for low-latency tile delivery with adaptive quality based on network conditions and client capabilities.

### Key Components

- **VdsAccess**: HueSpace/OpenVDS integration for reading seismic data
- **TileCache**: CPU LRU cache with prefetch and motion-vector hints  
- **Compressor**: LZ4 (default) / Zstd compression with adaptive selection
- **RTC**: WebRTC DataChannel for unordered, unreliable tile streaming
- **Protocol**: JSON control messages and binary tile framing
- **Adaptivity**: Dynamic adjustment of tile size, data type, compression
- **Metrics**: Performance monitoring with JSON endpoint

### Wire Protocol

**Control Messages (JSON over DataChannel):**
```json
{"t":"set_slice","inline":1234,"xline":980,"z":1600}
{"t":"set_view","plane":"inline","index":1238,"drag":true,"vx":+1}  
{"t":"set_lut","name":"SeismicRWB","clipPct":98,"gain":1.4}
{"t":"quality","prefer":{"dtype":"u8","downsample":2}}
{"t":"ping","id":42}
```

**Binary Tiles (24-byte header + payload):**
```
[Header 24B]
  u8   msgType = 0x01      // Tile
  u8   plane   = 0,1,2     // inline,xline,z  
  u16  tileW, tileH
  u32  tileX, tileY        // origin in slice pixel space
  u32  sliceIndex          // current plane index
  u8   dtype = 0=u8,1=u16,2=f32,3=mu-law-u8
  u8   comp  = 0=none,1=lz4,2=zstd
  u32  uncompressedBytes
  u32  payloadBytes
[payload payloadBytes]
```

## Build Requirements

### Remote Build Environment (Rocky Linux)
- GCC 8.4.1+ with C++20 support
- CMake 3.20+
- HueSpace SDK at `/home/rocky/HueSpace-13.3.0-449-gcc8.4.1-redhat-8-x86_64`
- OpenVDS libraries
- WebRTC development libraries
- LZ4 and Zstd compression libraries
- nlohmann/json, spdlog

### Dependencies
```bash
# On Rocky Linux
sudo dnf install cmake gcc-c++ nlohmann-json-devel spdlog-devel
sudo dnf install lz4-devel libzstd-devel openssl-devel
```

## Build and Deploy

### Local Development
```bash
# Clone and prepare
git clone <repo> && cd remoteview-server

# Deploy to remote server (builds automatically)
./deploy_remote.sh
```

### Manual Remote Build
```bash
# On remote server
cd /home/rocky/remoteview-server
export HUE_LICENSE_FILE=5053@license.cloud.bluware.com

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
make -j$(nproc)
make install
```

## Configuration

### Command Line
```bash
./remoteview_server --vds=/path/to/data.vds --tile=256 --comp=lz4 --cache-mb=2048
```

### Config File (JSON)
```json
{
  "vds": {"path": "/home/rocky/onnia2x3d_mig_Time.vds"},
  "server": {"port": 8080, "bind_address": "0.0.0.0"},
  "tiles": {"default_size": 256, "max_in_flight": 64},
  "cache": {"size_mb": 1024},
  "compression": {"algorithm": "lz4", "level": 1},
  "adaptivity": {"adaptive_tile_size": true, "adaptive_compression": true}
}
```

## Performance Targets

- **Latency**: <120ms median, <250ms p95 tile arrival
- **Cache**: >70% hit ratio during scrubbing
- **Memory**: Bounded queues, <75% host memory usage
- **Throughput**: Interactive scrubbing without frame drops

## Security

- DTLS-SRTP for media channels
- WSS for WebRTC signaling  
- SSH tunnels for development
- No disk writes of tile/frame data

## Testing

```bash
# Run unit tests
cd build && ctest --output-on-failure

# Run integration tests with real VDS
./tests/integration_tests --vds=/path/to/test.vds

# Performance benchmarks
./benchmarks/tile_performance_bench
```

## Monitoring

Metrics available at `http://localhost:9090/metrics` (JSON):
- Tiles/second per plane
- Cache hit ratios
- Compression ratios
- Memory usage
- Network RTT and bandwidth

## Architecture Decisions

See `docs/ADRs/` for:
1. Binary tiles over video encoding rationale
2. HueSpace C++ vs PyOpenVDS worker trade-offs  
3. Default tile size and data type selection
4. LZ4 vs Zstd compression strategies
5. WebRTC transport layer decisions

## Directory Structure

```
src/
├── config/          # Configuration management
├── vds_access/      # HueSpace/OpenVDS integration
├── tile_cache/      # LRU caching with prefetch
├── compressor/      # LZ4/Zstd compression
├── rtc_channel/     # WebRTC DataChannel transport
├── adaptivity/      # Dynamic quality adjustments  
├── metrics/         # Performance monitoring
├── protocol/        # Wire protocol implementation
├── main.cpp         # Entry point
└── server.cpp       # Main server orchestration

tests/               # Extensive test suites
benchmarks/          # Performance benchmarks
```