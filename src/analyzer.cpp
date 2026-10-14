#include "frameforge/analyzer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libswscale/swscale.h>
}

namespace frameforge {
namespace {

struct FrameSignature {
    std::array<std::uint64_t, 16> histogram{};
    double mean = 0.0;
    double standardDeviation = 0.0;
};

struct SceneSegment {
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
    double startTime = 0.0;
    double endTimeExclusive = 0.0;
};

FrameSignature summarize(const std::vector<std::uint8_t>& pixels) {
    FrameSignature signature;
    if (pixels.empty()) {
        return signature;
    }

    long double sum = 0.0L;
    long double sumSquares = 0.0L;
    for (const std::uint8_t pixel : pixels) {
        const auto value = static_cast<std::uint64_t>(pixel);
        ++signature.histogram[value >> 4U];
        sum += static_cast<long double>(value);
        sumSquares += static_cast<long double>(value) * static_cast<long double>(value);
    }
    const long double count = static_cast<long double>(pixels.size());
    const long double mean = sum / count;
    const long double variance = std::max(0.0L, sumSquares / count - mean * mean);
    signature.mean = static_cast<double>(mean);
    signature.standardDeviation = static_cast<double>(std::sqrt(variance));
    return signature;
}

double histogramDistance(const FrameSignature& a, const FrameSignature& b,
                         std::size_t pixelCount) {
    if (pixelCount == 0U) {
        return 0.0;
    }
    std::uint64_t totalDifference = 0;
    for (std::size_t i = 0; i < a.histogram.size(); ++i) {
        const std::uint64_t left = a.histogram[i];
        const std::uint64_t right = b.histogram[i];
        totalDifference += left > right ? left - right : right - left;
    }
    return static_cast<double>(totalDifference) /
           (2.0 * static_cast<double>(pixelCount));
}

void writeSegment(std::ostream& output, const SceneSegment& segment) {
    output << "{\"start_frame\":" << segment.startFrame
           << ",\"end_frame\":" << segment.endFrame
           << ",\"frame_count\":" << (segment.endFrame - segment.startFrame + 1U)
           << ",\"start_time_seconds\":" << segment.startTime
           << ",\"end_time_seconds_exclusive\":" << segment.endTimeExclusive << '}';
}

}  // namespace

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8U);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += hex[(ch >> 4U) & 0x0FU];
                    escaped += hex[ch & 0x0FU];
                } else {
                    escaped += static_cast<char>(ch);
                }
        }
    }
    return escaped;
}

void analyzeVideo(VideoReader& reader, std::ostream& output,
                  const AnalysisOptions& options) {
    if (options.sampleWidth <= 0 || options.sampleHeight <= 0) {
        throw std::invalid_argument("analysis sample width and height must be positive");
    }
    if (!std::isfinite(options.sceneThreshold) || options.sceneThreshold <= 0.0 ||
        options.sceneThreshold > 1.0) {
        throw std::invalid_argument("scene threshold must be greater than 0 and at most 1");
    }

    const VideoMetadata& metadata = reader.metadata();
    output << std::setprecision(9);
    output << "{\"schema_version\":\"1.0\",\"input\":\""
           << jsonEscape(metadata.path) << "\",\"stream\":{\"codec\":\""
           << jsonEscape(metadata.codec) << "\",\"pixel_format\":\""
           << jsonEscape(metadata.pixelFormat) << "\",\"width\":" << metadata.width
           << ",\"height\":" << metadata.height << ",\"nominal_fps\":"
           << metadata.fps << ",\"container_duration_seconds\":"
           << metadata.durationSeconds << ",\"bit_rate_bits_per_second\":"
           << metadata.bitRate << ",\"container_frame_count_hint\":"
           << metadata.containerFrameHint << "},\"analysis\":{\"method\":\"decoded-frame luma statistics, adjacent-frame difference and 16-bin histogram scene-change heuristic\",\"sample_size\":["
           << options.sampleWidth << ',' << options.sampleHeight
           << "],\"scene_threshold\":" << options.sceneThreshold
           << ",\"frames\":[";

    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler(nullptr, &sws_freeContext);
    std::vector<std::uint8_t> currentPixels(
        static_cast<std::size_t>(options.sampleWidth) *
        static_cast<std::size_t>(options.sampleHeight));
    std::vector<std::uint8_t> previousPixels;
    FrameSignature previousSignature;
    std::vector<SceneSegment> segments;
    std::uint64_t decodedCount = 0;
    std::uint64_t currentSceneStart = 0;
    double currentSceneStartTime = 0.0;
    double lastTimestamp = 0.0;
    bool havePrevious = false;
    double maxFrameInterval = metadata.fps > 0.0 ? 1.0 / metadata.fps : 0.0;

    const std::uint64_t count = reader.decode([&](const AVFrame* frame,
                                                 std::uint64_t index) {
        SwsContext* updated = sws_getCachedContext(
            scaler.release(), frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format), options.sampleWidth,
            options.sampleHeight, AV_PIX_FMT_GRAY8, SWS_FAST_BILINEAR,
            nullptr, nullptr, nullptr);
        if (updated == nullptr) {
            throw std::runtime_error("cannot create frame-analysis scaler");
        }
        scaler.reset(updated);

        std::uint8_t* destination[4] = {currentPixels.data(), nullptr, nullptr, nullptr};
        int destinationStride[4] = {options.sampleWidth, 0, 0, 0};
        const int scaledRows = sws_scale(scaler.get(), frame->data, frame->linesize,
                                         0, frame->height, destination,
                                         destinationStride);
        if (scaledRows != options.sampleHeight) {
            throw std::runtime_error("frame-analysis scaler returned an incomplete frame");
        }

        const FrameSignature signature = summarize(currentPixels);
        const double timestamp = frameTimestampSeconds(frame, metadata, index);
        double motionScore = 0.0;
        double histogramScore = 0.0;
        double sceneScore = 0.0;
        bool sceneStart = index == 0U;
        if (havePrevious) {
            std::uint64_t absoluteDifference = 0;
            for (std::size_t i = 0; i < currentPixels.size(); ++i) {
                const int difference = static_cast<int>(currentPixels[i]) -
                                       static_cast<int>(previousPixels[i]);
                absoluteDifference += static_cast<std::uint64_t>(std::abs(difference));
            }
            motionScore = static_cast<double>(absoluteDifference) /
                          (255.0 * static_cast<double>(currentPixels.size()));
            histogramScore = histogramDistance(signature, previousSignature,
                                                currentPixels.size());
            sceneScore = 0.65 * histogramScore + 0.35 * motionScore;
            sceneStart = sceneScore >= options.sceneThreshold && histogramScore >= 0.08;
            if (sceneStart) {
                segments.push_back(SceneSegment{currentSceneStart, index - 1U,
                                                currentSceneStartTime, timestamp});
                currentSceneStart = index;
                currentSceneStartTime = timestamp;
            }
        } else {
            currentSceneStart = 0;
            currentSceneStartTime = timestamp;
        }

        if (index > 0U && timestamp > lastTimestamp) {
            maxFrameInterval = timestamp - lastTimestamp;
        }
        lastTimestamp = timestamp;

        if (index != 0U) {
            output << ',';
        }
        output << "{\"index\":" << index << ",\"timestamp_seconds\":"
               << timestamp << ",\"key_frame\":"
               << (((frame->flags & AV_FRAME_FLAG_KEY) != 0) ? "true" : "false")
               << ",\"luma_mean_0_255\":" << signature.mean
               << ",\"luma_stddev_0_255\":" << signature.standardDeviation
               << ",\"motion_score_0_1\":" << motionScore
               << ",\"histogram_distance_0_1\":" << histogramScore
               << ",\"scene_score_0_1\":" << sceneScore
               << ",\"scene_start\":" << (sceneStart ? "true" : "false") << '}';

        previousPixels = currentPixels;
        previousSignature = signature;
        havePrevious = true;
        decodedCount = index + 1U;
    });

    if (count != decodedCount) {
        throw std::runtime_error("internal frame-count consistency check failed");
    }
    if (decodedCount > 0U) {
        const double computedEnd = lastTimestamp + std::max(0.0, maxFrameInterval);
        const double videoEnd = metadata.durationSeconds > 0.0
                                    ? std::max(metadata.durationSeconds, computedEnd)
                                    : computedEnd;
        segments.push_back(SceneSegment{currentSceneStart, decodedCount - 1U,
                                        currentSceneStartTime, videoEnd});
    }

    double observedDuration = metadata.durationSeconds;
    if (decodedCount > 0U) {
        observedDuration = std::max(observedDuration,
                                    lastTimestamp + std::max(0.0, maxFrameInterval));
    }
    output << "],\"summary\":{\"decoded_frame_count\":" << decodedCount
           << ",\"scene_count\":" << segments.size()
           << ",\"observed_duration_seconds\":" << observedDuration
           << ",\"segments\":[";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0U) {
            output << ',';
        }
        writeSegment(output, segments[i]);
    }
    output << "]}}}\n";
    if (!output) {
        throw std::runtime_error("failed to write analysis JSON");
    }
}

}  // namespace frameforge
