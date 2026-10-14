#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace frameforge {

class PngWriter final {
public:
    PngWriter(const std::filesystem::path& outputDirectory, int width, int height);
    ~PngWriter();

    PngWriter(const PngWriter&) = delete;
    PngWriter& operator=(const PngWriter&) = delete;

    [[nodiscard]] std::filesystem::path write(const AVFrame* source,
                                              std::uint64_t frameIndex);

private:
    void initializeEncoder(const AVFrame* source);

    std::filesystem::path outputDirectory_;
    int width_ = 0;
    int height_ = 0;
    AVCodecContext* encoder_ = nullptr;
    AVFrame* rgbFrame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* scaler_ = nullptr;
};

}  // namespace frameforge
