#include "frameforge/png_writer.hpp"
#include "frameforge/video_reader.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <new>
#include <sstream>
#include <stdexcept>
#include <system_error>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace frameforge {
namespace {

[[noreturn]] void throwFfmpeg(const std::string& operation, int errorCode) {
    throw std::runtime_error(operation + ": " + ffmpegErrorString(errorCode));
}

}  // namespace

PngWriter::PngWriter(const std::filesystem::path& outputDirectory,
                     int width, int height)
    : outputDirectory_(outputDirectory), width_(width), height_(height) {
    if (width_ <= 0 || height_ <= 0) {
        throw std::invalid_argument("PNG dimensions must be positive");
    }
    std::filesystem::create_directories(outputDirectory_);
}

void PngWriter::initializeEncoder(const AVFrame* source) {
    if (encoder_ != nullptr) {
        return;
    }
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (encoder == nullptr) {
        throw std::runtime_error("the installed FFmpeg build has no PNG encoder");
    }

    unsigned int sourceDepth = 0;
    const AVPixFmtDescriptor* sourceFormat =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source->format));
    if (sourceFormat != nullptr) {
        for (int component = 0; component < sourceFormat->nb_components; ++component) {
            sourceDepth = std::max(sourceDepth,
                                   static_cast<unsigned int>(sourceFormat->comp[component].depth));
        }
    }
    const AVPixelFormat outputFormat = sourceDepth > 8U
                                           ? AV_PIX_FMT_RGB48BE
                                           : AV_PIX_FMT_RGB24;
    encoder_ = avcodec_alloc_context3(encoder);
    if (encoder_ == nullptr) {
        throw std::bad_alloc();
    }
    try {
        encoder_->width = width_;
        encoder_->height = height_;
        encoder_->pix_fmt = outputFormat;
        encoder_->time_base = AVRational{1, 25};
        encoder_->framerate = AVRational{25, 1};
        encoder_->compression_level = 3;
        const int openResult = avcodec_open2(encoder_, encoder, nullptr);
        if (openResult < 0) {
            throwFfmpeg("cannot initialize PNG encoder", openResult);
        }

        rgbFrame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (rgbFrame_ == nullptr || packet_ == nullptr) {
            throw std::bad_alloc();
        }
        rgbFrame_->format = encoder_->pix_fmt;
        rgbFrame_->width = width_;
        rgbFrame_->height = height_;
        const int bufferResult = av_frame_get_buffer(rgbFrame_, 32);
        if (bufferResult < 0) {
            throwFfmpeg("cannot allocate PNG conversion buffer", bufferResult);
        }
    } catch (...) {
        sws_freeContext(scaler_);
        av_packet_free(&packet_);
        av_frame_free(&rgbFrame_);
        avcodec_free_context(&encoder_);
        throw;
    }
}

PngWriter::~PngWriter() {
    sws_freeContext(scaler_);
    av_packet_free(&packet_);
    av_frame_free(&rgbFrame_);
    avcodec_free_context(&encoder_);
}

std::filesystem::path PngWriter::write(const AVFrame* source,
                                       std::uint64_t frameIndex) {
    if (source == nullptr || source->width != width_ || source->height != height_) {
        throw std::invalid_argument("decoded frame dimensions changed during PNG export");
    }
    initializeEncoder(source);

    SwsContext* updated = sws_getCachedContext(
        scaler_, source->width, source->height,
        static_cast<AVPixelFormat>(source->format), width_, height_,
        encoder_->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (updated == nullptr) {
        scaler_ = nullptr;
        throw std::runtime_error("cannot create PNG color-conversion scaler");
    }
    scaler_ = updated;

    const int writableResult = av_frame_make_writable(rgbFrame_);
    if (writableResult < 0) {
        throwFfmpeg("cannot prepare PNG frame buffer", writableResult);
    }
    const int scaledRows = sws_scale(scaler_, source->data, source->linesize,
                                     0, source->height, rgbFrame_->data,
                                     rgbFrame_->linesize);
    if (scaledRows != height_) {
        throw std::runtime_error("PNG color conversion returned an incomplete frame");
    }
    rgbFrame_->pts = static_cast<std::int64_t>(frameIndex);

    std::ostringstream filename;
    filename << "frame_" << std::setw(10) << std::setfill('0') << frameIndex << ".png";
    const std::filesystem::path finalPath = outputDirectory_ / filename.str();
    const std::filesystem::path temporaryPath = finalPath.string() + ".tmp";
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create output image: " + temporaryPath.string());
    }

    try {
        const int sendResult = avcodec_send_frame(encoder_, rgbFrame_);
        if (sendResult < 0) {
            throwFfmpeg("PNG encoder rejected frame", sendResult);
        }
        std::uint64_t packetCount = 0;
        while (true) {
            const int receiveResult = avcodec_receive_packet(encoder_, packet_);
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                break;
            }
            if (receiveResult < 0) {
                throwFfmpeg("PNG encoding failed", receiveResult);
            }
            output.write(reinterpret_cast<const char*>(packet_->data),
                         static_cast<std::streamsize>(packet_->size));
            ++packetCount;
            av_packet_unref(packet_);
            if (!output) {
                throw std::runtime_error("failed while writing PNG image bytes");
            }
        }
        output.close();
        if (packetCount != 1U) {
            throw std::runtime_error("PNG encoder did not produce exactly one image packet");
        }
        std::filesystem::rename(temporaryPath, finalPath);
    } catch (...) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw;
    }
    return finalPath;
}

}  // namespace frameforge
