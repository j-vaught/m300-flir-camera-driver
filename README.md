# Camera Stream Driver

A high-performance C++ driver for capturing RTSP video streams from FLIR M300 thermal cameras and saving frames as JPEG files with precise timestamp metadata.

## Features

- **Multi-camera support** - Connect to multiple FLIR cameras simultaneously
- **Flexible stream selection** - Support for any RTSP stream endpoint and custom ports
- **Low-latency encoding** - Parallel JPEG encoding with libjpeg-turbo (9-42ms per frame)
- **Precise timestamping** - Hardware timestamps from camera stream + computer receive time
- **Latency measurement** - Per-frame encode time tracking in filenames
- **Error callbacks** - Detailed error handling with structured error reporting
- **Statistics tracking** - Real-time FPS, frame count, and drop monitoring

## Hardware Tested

- **FLIR M364C** (Visible1): `169.254.50.183`
  - vis.0: 1920x1080 H.264 stream
  - vis.1: 1280x720 MJPEG stream
- **FLIR M364C** (Visible2): `169.254.80.109`
  - vis.0: 1920x1080 H.264 stream
  - vis.1: 1280x720 MJPEG stream

## Performance

| Camera | Stream | Resolution | FPS | Avg Latency | Range |
|--------|--------|------------|-----|-------------|-------|
| V1 | vis.0 | 1920x1080 H.264 | 30 | 34.35ms | 18-48ms |
| V1 | vis.1 | 1280x720 MJPEG | 30 | 18.54ms | 14-21ms |
| V2 | vis.0 | 1920x1080 H.264 | 30 | 33.87ms | 21-41ms |
| V2 | vis.1 | 1280x720 MJPEG | 30 | 14.05ms | 5-23ms |

## Installation

### Dependencies

```bash
# Ubuntu/Debian
sudo apt-get install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libjpeg-turbo-dev

# Or use your package manager for:
# - FFmpeg development libraries
# - libjpeg-turbo development libraries
# - C++17 compiler (gcc/clang)
# - CMake 3.10+
```

### Build

```bash
cd camera_stream_driver
mkdir build
cd build
cmake ..
make -j4
```

Executable: `build/camera_test`

## Usage

```bash
./build/camera_test <camera_ip> <stream_name> <port>
```

### Examples

**Visible1, vis.0 stream (1920x1080 H.264):**
```bash
./build/camera_test 169.254.50.183 vis.0 8554
```

**Visible1, vis.1 stream (1280x720 MJPEG):**
```bash
./build/camera_test 169.254.50.183 vis.1 8554
```

**Visible2, vis.0 stream:**
```bash
./build/camera_test 169.254.80.109 vis.0 8554
```

**Custom camera/stream/port:**
```bash
./build/camera_test 192.168.1.100 thermal_stream 9000
```

## Filename Format

Captured JPEG files follow this naming convention:

```
YYYY.MM.DD_HH.MM.SS.mmm_HW_hwTimeNs_latencyMs.jpg
```

Example:
```
2025.11.27_09.22.48.803_HW_700000000_18ms.jpg
│          │  │  │ │  │  │  │          │  │
│          │  │  │ │  │  │  │          │  └─ Encoding latency (milliseconds)
│          │  │  │ │  │  │  └──────────────── Hardware time from camera (nanoseconds)
└──────────┴──┴──┴─┴──┴──┴───────────────────── Computer receive time (YYYY.MM.DD_HH.MM.SS.mmm)
```

- **YYYY.MM.DD_HH.MM.SS.mmm** - When frame was received by driver (system time)
- **HW** - Hardware timestamp present (from camera stream PTS)
- **hwTimeNs** - Camera stream timestamp in nanoseconds
- **latencyMs** - JPEG encode + write time in milliseconds

## Output Directory

By default, frames are saved to:
```
./output/
```

This can be modified in `src/main.cpp` (line ~50):
```cpp
std::string outputFolder = "/path/to/desired/directory";
```

## Configuration

Edit `src/main.cpp` to adjust:

```cpp
int numWriteThreads = 4;    // Parallel JPEG encoding threads
int jpegQuality = 85;        // JPEG quality 0-100
```

Recompile after changes:
```bash
cd build
cmake ..
make -j4
```

## API Usage (Library)

Use the driver as a library in your own C++ code:

```cpp
#include "CameraFrameCapture.hpp"

int main() {
    // Create capture instance
    CameraFrameCapture capture(
        "rtsp://169.254.50.183:8554/vis.0",  // RTSP URL
        "/output/frames/",                     // Output folder
        4,                                     // Write threads
        85                                     // JPEG quality
    );

    // Set error callback
    capture.setErrorCallback([](const ErrorInfo& error) {
        std::cerr << "[" << (error.isFatal ? "FATAL" : "WARNING") << "] "
                  << error.message << std::endl;
    });

    // Start capture
    if (!capture.start()) {
        std::cerr << "Failed to start capture" << std::endl;
        return 1;
    }

    // Monitor
    std::cout << "Capturing... Press Ctrl+C to stop" << std::endl;
    while (capture.isRunning()) {
        auto stats = capture.getStats();
        std::cout << "Frames: " << stats.capturedFrames
                  << " | Written: " << stats.writtenFrames
                  << " | FPS: " << stats.currentFPS << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    capture.stop();
    return 0;
}
```

### Public API

**Constructor:**
```cpp
CameraFrameCapture(
    const std::string& rtspUrl,      // RTSP stream URL
    const std::string& outputFolder, // Output directory path
    int numWriteThreads = 4,         // Number of write threads
    int jpegQuality = 85             // JPEG quality (0-100)
);
```

**Methods:**
```cpp
bool start();                    // Start capture threads
void stop();                     // Stop gracefully
bool isRunning() const;          // Check if running
void setErrorCallback(ErrorCallback callback);
FrameStats getStats() const;     // Get frame statistics
```

**Error Callback:**
```cpp
struct ErrorInfo {
    ErrorType type;              // CONNECTION_FAILED, FRAME_DECODE_ERROR, etc.
    std::string message;         // Error description
    uint64_t timestamp;          // When error occurred
    bool isFatal;                // Whether this stops capture
};

using ErrorCallback = std::function<void(const ErrorInfo& error)>;
```

**Statistics:**
```cpp
struct FrameStats {
    uint64_t capturedFrames;     // Frames decoded from stream
    uint64_t writtenFrames;      // Frames successfully written
    uint64_t droppedFrames;      // Frames dropped due to queue overflow
    float currentFPS;            // Current capture FPS
};
```

## Troubleshooting

**No frames being captured:**
- Verify camera is reachable: `ping 169.254.50.183`
- Check RTSP URL is correct
- Verify stream endpoint exists (vis.0, vis.1, etc.)
- Check firewall allows RTSP (port 8554)

**High latency (>50ms):**
- Reduce JPEG quality setting
- Use MJPEG streams (vis.1) instead of H.264 (vis.0)
- Increase `numWriteThreads` for parallel encoding
- Check disk write speed for output directory

**Connection timeouts:**
- Increase `max_delay` in src/CameraFrameCapture.cpp:225 (in microseconds)
- Use TCP transport instead of UDP

**Frame drops:**
- Reduce write thread workload
- Use SSD for output directory
- Reduce JPEG quality

## License

See repository LICENSE file.

## Documentation

- **Latency Analysis** - See `/docs/latency_analysis.png` for detailed encoding latency histograms
- **API Header** - See `include/CameraFrameCapture.hpp` for complete interface documentation
