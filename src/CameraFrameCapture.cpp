#include "CameraFrameCapture.hpp"

#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <jpeglib.h>
}

namespace fs = std::filesystem;

// Frame data structure for queue
struct Frame {
    std::vector<uint8_t> data;
    int width;
    int height;
    uint64_t frameNumber;
    uint64_t computerTimeMs;      // Computer receive time in milliseconds (when frame decoded)
    uint64_t hardwareTimeNs;      // Hardware timestamp in nanoseconds
    bool hwTimeValid;             // True if hardware time is from PTS, false if fallback
};

// Thread-safe queue for frames
class CameraFrameCapture::FrameQueueImpl {
public:
    std::queue<Frame> queue;
    std::mutex mutex;
    std::condition_variable cv;
    const size_t maxSize = 15;

    bool push(Frame frame) {
        std::unique_lock<std::mutex> lock(mutex);
        if (queue.size() >= maxSize) {
            return false;  // Queue full, frame dropped
        }
        queue.push(std::move(frame));
        lock.unlock();
        cv.notify_one();
        return true;
    }

    bool pop(Frame& frame, int timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this] { return !queue.empty(); })) {
            return false;
        }
        if (queue.empty()) return false;
        frame = std::move(queue.front());
        queue.pop();
        return true;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) queue.pop();
    }
};

// Helper to encode frame to JPEG
static void encodeFrameToJPEG(const Frame& frame, int quality,
                              const std::string& filepath) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    FILE* outfile = fopen(filepath.c_str(), "wb");
    if (!outfile) {
        std::cerr << "Failed to open file: " << filepath << std::endl;
        return;
    }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = frame.width;
    cinfo.image_height = frame.height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    JSAMPROW row_pointer[1];
    const uint8_t* imageData = frame.data.data();
    int rowStride = frame.width * 3;

    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = (JSAMPROW)(imageData + cinfo.next_scanline * rowStride);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    fclose(outfile);
    jpeg_destroy_compress(&cinfo);
}

CameraFrameCapture::CameraFrameCapture(
    const std::string& rtspUrl,
    const std::string& outputFolder,
    int numWriteThreads,
    int jpegQuality)
    : rtspUrl_(rtspUrl),
      outputFolder_(outputFolder),
      numWriteThreads_(numWriteThreads),
      jpegQuality_(jpegQuality),
      frameQueue_(std::make_shared<FrameQueueImpl>()) {

    // Ensure output folder exists
    try {
        fs::create_directories(outputFolder_);
    } catch (const std::exception& e) {
        std::cerr << "Failed to create output folder: " << e.what() << std::endl;
    }
}

CameraFrameCapture::~CameraFrameCapture() {
    stop();
}

bool CameraFrameCapture::start() {
    if (running_.exchange(true)) {
        return false;  // Already running
    }

    shouldStop_.store(false);

    try {
        captureThread_ = std::make_unique<std::thread>(&CameraFrameCapture::captureThreadFunc, this);

        for (int i = 0; i < numWriteThreads_; ++i) {
            writeThreads_.push_back(
                std::make_unique<std::thread>(&CameraFrameCapture::writeThreadFunc, this)
            );
        }
        return true;
    } catch (const std::exception& e) {
        running_.store(false);
        reportError(ErrorType::ThreadError,
                   std::string("Failed to start threads: ") + e.what(),
                   true);
        return false;
    }
}

void CameraFrameCapture::stop() {
    if (!running_.exchange(false)) {
        return;  // Not running
    }

    shouldStop_.store(true);

    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
    }

    for (auto& thread : writeThreads_) {
        if (thread && thread->joinable()) {
            thread->join();
        }
    }
    writeThreads_.clear();
}

bool CameraFrameCapture::isRunning() const {
    return running_.load();
}

void CameraFrameCapture::setErrorCallback(ErrorCallback callback) {
    errorCallback_ = callback;
}

FrameStats CameraFrameCapture::getStats() const {
    return {
        capturedFrames_.load(),
        writtenFrames_.load(),
        droppedFrames_.load(),
        currentFPS_.load()
    };
}

void CameraFrameCapture::reportError(ErrorType type, const std::string& message, bool isFatal) {
    if (errorCallback_) {
        auto ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        ErrorInfo info{
            type,
            message,
            static_cast<uint64_t>(ns),
            isFatal
        };
        errorCallback_(info);
    }

    if (isFatal) {
        running_.store(false);
    }
}

void CameraFrameCapture::captureThreadFunc() {
    // Open input
    AVFormatContext* formatCtx = avformat_alloc_context();
    if (!formatCtx) {
        reportError(ErrorType::ConnectionFailed, "Failed to allocate AVFormatContext", true);
        return;
    }

    // Set low latency options
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "buffer_size", "32768", 0);
    av_dict_set(&options, "max_delay", "500000", 0);  // 500ms max delay

    if (avformat_open_input(&formatCtx, rtspUrl_.c_str(), nullptr, &options) != 0) {
        av_dict_free(&options);
        reportError(ErrorType::ConnectionFailed,
                   "Failed to open RTSP stream: " + rtspUrl_, true);
        avformat_free_context(formatCtx);
        return;
    }
    av_dict_free(&options);

    // Find stream info
    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        reportError(ErrorType::ConnectionFailed, "Failed to find stream info", true);
        avformat_close_input(&formatCtx);
        return;
    }

    // Find video stream
    int videoStreamIdx = -1;
    for (unsigned int i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIdx = i;
            break;
        }
    }

    if (videoStreamIdx < 0) {
        reportError(ErrorType::ConnectionFailed, "No video stream found", true);
        avformat_close_input(&formatCtx);
        return;
    }

    // Get codec
    AVCodecParameters* codecpar = formatCtx->streams[videoStreamIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        reportError(ErrorType::FrameDecodeError, "Codec not found", true);
        avformat_close_input(&formatCtx);
        return;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        reportError(ErrorType::FrameDecodeError, "Failed to allocate codec context", true);
        avformat_close_input(&formatCtx);
        return;
    }

    avcodec_parameters_to_context(codecCtx, codecpar);
    codecCtx->thread_count = 4;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        reportError(ErrorType::FrameDecodeError, "Failed to open codec", true);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return;
    }

    // Allocate frames
    AVFrame* rawFrame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    SwsContext* swsCtx = nullptr;
    uint8_t* buffer = nullptr;

    if (!rawFrame || !rgbFrame || !packet) {
        reportError(ErrorType::FrameDecodeError, "Failed to allocate frame buffers", true);
        if (rawFrame) av_frame_free(&rawFrame);
        if (rgbFrame) av_frame_free(&rgbFrame);
        if (packet) av_packet_free(&packet);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return;
    }

    // Initialize SWS context for RGB conversion
    swsCtx = sws_getContext(
        codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
        codecCtx->width, codecCtx->height, AV_PIX_FMT_RGB24,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsCtx) {
        reportError(ErrorType::FrameDecodeError, "Failed to create SWS context", true);
        av_frame_free(&rawFrame);
        av_frame_free(&rgbFrame);
        av_packet_free(&packet);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return;
    }

    // Allocate buffer for RGB frame
    int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24,
                                               codecCtx->width,
                                               codecCtx->height, 1);
    buffer = (uint8_t*)av_malloc(bufferSize);
    if (!buffer) {
        reportError(ErrorType::FrameDecodeError, "Failed to allocate RGB buffer", true);
        sws_freeContext(swsCtx);
        av_frame_free(&rawFrame);
        av_frame_free(&rgbFrame);
        av_packet_free(&packet);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return;
    }

    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize,
                        buffer, AV_PIX_FMT_RGB24,
                        codecCtx->width, codecCtx->height, 1);

    std::cout << "Connected to RTSP stream: " << rtspUrl_ << std::endl;
    std::cout << "Resolution: " << codecCtx->width << "x" << codecCtx->height << std::endl;

    // Capture loop
    {
        auto lastFpsTime = std::chrono::high_resolution_clock::now();
        uint64_t framesSinceFpsCheck = 0;
        uint64_t frameCounter = 0;

        while (!shouldStop_.load()) {
            if (av_read_frame(formatCtx, packet) < 0) {
                reportError(ErrorType::FrameDecodeError, "Failed to read frame", false);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (packet->stream_index != videoStreamIdx) {
                av_packet_unref(packet);
                continue;
            }

            int ret = avcodec_send_packet(codecCtx, packet);
            av_packet_unref(packet);

            if (ret < 0) continue;

            ret = avcodec_receive_frame(codecCtx, rawFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) continue;
            if (ret < 0) {
                reportError(ErrorType::FrameDecodeError, "Decoding error", false);
                continue;
            }

            // Convert to RGB
            sws_scale(swsCtx,
                     (const uint8_t* const*)rawFrame->data,
                     rawFrame->linesize, 0, codecCtx->height,
                     rgbFrame->data, rgbFrame->linesize);

            // Create frame data
            Frame frame;
            frame.width = codecCtx->width;
            frame.height = codecCtx->height;
            frame.frameNumber = frameCounter++;

            // Capture computer receive time (milliseconds since epoch)
            frame.computerTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count();

            // Extract hardware timestamp from frame (convert PTS to nanoseconds)
            // The PTS is in units of the timebase (typically 1/frame_rate for video)
            if (rawFrame->pts != AV_NOPTS_VALUE) {
                // Get the timebase from the stream
                AVRational timebase = formatCtx->streams[videoStreamIdx]->time_base;
                // Convert PTS to seconds, then to nanoseconds
                double ptsSec = (double)rawFrame->pts * av_q2d(timebase);
                frame.hardwareTimeNs = (uint64_t)(ptsSec * 1e9);
                frame.hwTimeValid = true;
            } else {
                // Fallback: use computer time if hardware timestamp unavailable
                frame.hardwareTimeNs = frame.computerTimeMs * 1000000;
                frame.hwTimeValid = false;
            }

            // Copy RGB data
            int imageSize = frame.width * frame.height * 3;
            frame.data.assign(rgbFrame->data[0],
                            rgbFrame->data[0] + imageSize);

            // Try to push to queue
            if (!frameQueue_->push(std::move(frame))) {
                droppedFrames_.fetch_add(1);
            } else {
                capturedFrames_.fetch_add(1);
            }

            // Update FPS
            framesSinceFpsCheck++;
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastFpsTime);

            if (elapsed.count() >= 1000) {
                float fps = framesSinceFpsCheck * 1000.0f / elapsed.count();
                currentFPS_.store(fps);
                framesSinceFpsCheck = 0;
                lastFpsTime = now;
            }
        }
    }

    // Cleanup
    if (buffer) av_free(buffer);
    if (swsCtx) sws_freeContext(swsCtx);
    if (rgbFrame) av_frame_free(&rgbFrame);
    if (rawFrame) av_frame_free(&rawFrame);
    if (packet) av_packet_free(&packet);
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (formatCtx) avformat_close_input(&formatCtx);

    std::cout << "Capture thread exiting" << std::endl;
}

void CameraFrameCapture::writeThreadFunc() {
    Frame frame;
    uint64_t localWriteCounter = 0;

    while (!shouldStop_.load()) {
        if (frameQueue_->pop(frame, 100)) {
            // Format computer time as YYYY.MM.DD_HH.MM.SS.mmm
            time_t seconds = frame.computerTimeMs / 1000;
            uint64_t milliseconds = frame.computerTimeMs % 1000;
            struct tm* timeinfo = localtime(&seconds);

            std::ostringstream timeStr;
            timeStr << std::setfill('0')
                    << (timeinfo->tm_year + 1900) << "."
                    << std::setw(2) << (timeinfo->tm_mon + 1) << "."
                    << std::setw(2) << timeinfo->tm_mday << "_"
                    << std::setw(2) << timeinfo->tm_hour << "."
                    << std::setw(2) << timeinfo->tm_min << "."
                    << std::setw(2) << timeinfo->tm_sec << "."
                    << std::setw(3) << milliseconds;

            // Measure encode + write time
            auto encodeStartTime = std::chrono::high_resolution_clock::now();

            // Generate temporary filename for encoding
            std::ostringstream oss;
            oss << outputFolder_ << "/"
                << timeStr.str() << "_";

            if (frame.hwTimeValid) {
                oss << "HW_" << frame.hardwareTimeNs;
            } else {
                oss << "ERR_" << frame.hardwareTimeNs;
            }

            std::string tempFilepath = oss.str() + ".jpg";

            // Encode and write JPEG
            encodeFrameToJPEG(frame, jpegQuality_, tempFilepath);

            auto encodeEndTime = std::chrono::high_resolution_clock::now();
            uint64_t encodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                encodeEndTime - encodeStartTime).count();

            // Rename file to include encoding time
            std::ostringstream finalOss;
            finalOss << outputFolder_ << "/"
                     << timeStr.str() << "_";

            if (frame.hwTimeValid) {
                finalOss << "HW_" << frame.hardwareTimeNs;
            } else {
                finalOss << "ERR_" << frame.hardwareTimeNs;
            }

            finalOss << "_" << encodeTimeMs << "ms.jpg";

            std::rename(tempFilepath.c_str(), finalOss.str().c_str());

            writtenFrames_.fetch_add(1);
            localWriteCounter++;
        }
    }

    std::cout << "Write thread exiting (wrote " << localWriteCounter << " frames)" << std::endl;
}
