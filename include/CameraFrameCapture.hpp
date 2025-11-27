#pragma once

#include <string>
#include <functional>
#include <cstdint>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// Error callback structures
enum class ErrorType {
    ConnectionFailed,
    FrameDecodeError,
    WriteError,
    ThreadError,
    Other
};

struct ErrorInfo {
    ErrorType type;
    std::string message;
    uint64_t timestamp;
    bool isFatal;
};

using ErrorCallback = std::function<void(const ErrorInfo& error)>;

// Frame statistics
struct FrameStats {
    uint64_t capturedFrames;
    uint64_t writtenFrames;
    uint64_t droppedFrames;
    float currentFPS;
};

// Main camera capture driver class
class CameraFrameCapture {
public:
    // Constructor - initialize with RTSP stream and output folder
    CameraFrameCapture(
        const std::string& rtspUrl,
        const std::string& outputFolder,
        int numWriteThreads = 4,
        int jpegQuality = 85
    );

    // Destructor - cleanup resources
    ~CameraFrameCapture();

    // Control methods
    bool start();
    void stop();
    bool isRunning() const;

    // Error callback registration
    void setErrorCallback(ErrorCallback callback);

    // Statistics
    FrameStats getStats() const;

private:
    // Internal state
    std::string rtspUrl_;
    std::string outputFolder_;
    int numWriteThreads_;
    int jpegQuality_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shouldStop_{false};

    // Statistics
    std::atomic<uint64_t> capturedFrames_{0};
    std::atomic<uint64_t> writtenFrames_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<float> currentFPS_{0.0f};

    // Threads
    std::unique_ptr<std::thread> captureThread_;
    std::vector<std::unique_ptr<std::thread>> writeThreads_;

    // Error callback
    ErrorCallback errorCallback_;

    // Shared frame queue (forward declared)
    class FrameQueueImpl;
    std::shared_ptr<FrameQueueImpl> frameQueue_;

    // Internal helper methods
    void captureThreadFunc();
    void writeThreadFunc();
    void reportError(ErrorType type, const std::string& message, bool isFatal);
};
