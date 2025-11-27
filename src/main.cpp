#include "CameraFrameCapture.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <string>
#include <cstring>

int main(int argc, char* argv[]) {
    std::cout << "=== FLIR M364C Frame Capture Test ===" << std::endl;
    std::cout << std::endl;

    // Parse required arguments
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <camera_ip> <stream_name> <port>" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Examples:" << std::endl;
        std::cerr << "  " << argv[0] << " 169.254.50.183 vis.0 8554   # Visible1, vis.0, port 8554" << std::endl;
        std::cerr << "  " << argv[0] << " 169.254.80.109 vis.0 8554   # Visible2, vis.0, port 8554" << std::endl;
        std::cerr << "  " << argv[0] << " 169.254.50.183 vis.1 8554   # Visible1, vis.1, port 8554" << std::endl;
        std::cerr << "  " << argv[0] << " 192.168.1.100 custom_stream 9000  # Custom IP/stream/port" << std::endl;
        return 1;
    }

    std::string cameraIp = argv[1];
    std::string streamName = argv[2];
    int port = std::stoi(argv[3]);

    // Determine camera description
    std::string cameraDesc;
    if (cameraIp == "169.254.50.183") {
        cameraDesc = "Visible1";
    } else if (cameraIp == "169.254.80.109") {
        cameraDesc = "Visible2";
    } else {
        cameraDesc = cameraIp;
    }

    // Build RTSP URL
    std::string rtspUrl = "rtsp://" + cameraIp + ":" + std::to_string(port) + "/" + streamName;
    std::string streamDesc = streamName;

    std::string outputFolder = "/media/samsung/projects/Dual_FLIR_cpp_multi-stage/camera-driver/output";
    int numWriteThreads = 4;
    int jpegQuality = 85;

    CameraFrameCapture capture(rtspUrl, outputFolder, numWriteThreads, jpegQuality);

    // Set error callback
    capture.setErrorCallback([](const ErrorInfo& error) {
        std::string typeStr;
        switch (error.type) {
            case ErrorType::ConnectionFailed:
                typeStr = "CONNECTION_FAILED";
                break;
            case ErrorType::FrameDecodeError:
                typeStr = "FRAME_DECODE_ERROR";
                break;
            case ErrorType::WriteError:
                typeStr = "WRITE_ERROR";
                break;
            case ErrorType::ThreadError:
                typeStr = "THREAD_ERROR";
                break;
            default:
                typeStr = "OTHER";
                break;
        }

        std::string severity = error.isFatal ? "FATAL" : "WARNING";
        std::cerr << "[" << severity << "] " << typeStr << ": " << error.message << std::endl;
    });

    // Start capture
    std::cout << "Starting capture..." << std::endl;
    std::cout << "Camera: " << cameraDesc << std::endl;
    std::cout << "Stream: " << streamDesc << std::endl;
    std::cout << "RTSP URL: " << rtspUrl << std::endl;
    std::cout << "Output folder: " << outputFolder << std::endl;
    std::cout << "Write threads: " << numWriteThreads << std::endl;
    std::cout << "JPEG quality: " << jpegQuality << "%" << std::endl;
    std::cout << std::endl;

    if (!capture.start()) {
        std::cerr << "Failed to start capture!" << std::endl;
        return 1;
    }

    // Monitor and print stats every second
    std::cout << "Running capture. Press Ctrl+C to stop." << std::endl;
    std::cout << std::string(80, '-') << std::endl;
    std::cout << std::setw(20) << "Time"
              << std::setw(15) << "Captured"
              << std::setw(15) << "Written"
              << std::setw(15) << "Dropped"
              << std::setw(15) << "FPS"
              << std::endl;
    std::cout << std::string(80, '-') << std::endl;

    auto startTime = std::chrono::high_resolution_clock::now();

    while (capture.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        auto stats = capture.getStats();
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime);

        std::cout << std::setw(20) << elapsed.count() << "s"
                  << std::setw(15) << stats.capturedFrames
                  << std::setw(15) << stats.writtenFrames
                  << std::setw(15) << stats.droppedFrames
                  << std::setw(15) << std::fixed << std::setprecision(1) << stats.currentFPS
                  << " Hz"
                  << std::endl;
    }

    std::cout << std::string(80, '-') << std::endl;
    std::cout << "Capture stopped." << std::endl;

    // Final stats
    auto stats = capture.getStats();
    std::cout << std::endl;
    std::cout << "Final Statistics:" << std::endl;
    std::cout << "  Captured frames: " << stats.capturedFrames << std::endl;
    std::cout << "  Written frames: " << stats.writtenFrames << std::endl;
    std::cout << "  Dropped frames: " << stats.droppedFrames << std::endl;
    std::cout << "  Last FPS: " << std::fixed << std::setprecision(1) << stats.currentFPS << std::endl;

    return 0;
}
