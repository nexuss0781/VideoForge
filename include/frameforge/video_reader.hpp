#pragma once

#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace frameforge {

struct VideoMetadata {
    std::string path;
    std::string codec;
    std::string pixelFormat;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double durationSeconds = 0.0;
    std::int64_t bitRate = 0;
    std::uint64_t containerFrameHint = 0;
    std::uint32_t audioStreamCount = 0;
    AVRational timeBase{0, 1};
    std::string colorRange;
    std::string colorPrimaries;
    std::string colorTransfer;
    std::string colorSpace;
    std::string chromaLocation;
    std::uint32_t sampleAspectRatioNumerator = 1;
    std::uint32_t sampleAspectRatioDenominator = 1;
    std::string fieldOrder;
    // False when packing decoded planes alone would discard additional streams,
    // chapters, user metadata, or known display-affecting stream properties.
    bool nativeFrameArchiveEligible = false;
};

class VideoReader final {
public:
    explicit VideoReader(const std::string& path, int threadCount = 0);
    ~VideoReader();

    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;
    VideoReader(VideoReader&&) = delete;
    VideoReader& operator=(VideoReader&&) = delete;

    [[nodiscard]] const VideoMetadata& metadata() const noexcept { return metadata_; }
    [[nodiscard]] int decoderThreadCount() const noexcept { return decoderThreadCount_; }

    // The callback receives frames in presentation order. The AVFrame is owned by
    // this reader and is valid only for the duration of the callback.
    std::uint64_t decode(const std::function<void(const AVFrame*, std::uint64_t)>& callback);

private:
    void release() noexcept;
    void drainDecoder(const std::function<void(const AVFrame*, std::uint64_t)>& callback,
                      std::uint64_t& frameIndex);

    AVFormatContext* formatContext_ = nullptr;
    AVCodecContext* codecContext_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int videoStreamIndex_ = -1;
    int decoderThreadCount_ = 0;
    VideoMetadata metadata_;
};

[[nodiscard]] std::string ffmpegErrorString(int errorCode);
[[nodiscard]] double frameTimestampSeconds(const AVFrame* frame,
                                           const VideoMetadata& metadata,
                                           std::uint64_t fallbackIndex);

}  // namespace frameforge
