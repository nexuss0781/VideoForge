#include "frameforge/video_reader.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

namespace frameforge {
namespace {

[[noreturn]] void throwFfmpeg(const std::string& operation, int errorCode) {
    throw std::runtime_error(operation + ": " + ffmpegErrorString(errorCode));
}

bool validRational(AVRational value) noexcept {
    return value.num > 0 && value.den > 0;
}

bool sameRational(AVRational left, AVRational right) noexcept {
    return validRational(left) && validRational(right) &&
           static_cast<std::int64_t>(left.num) * right.den ==
               static_cast<std::int64_t>(right.num) * left.den;
}

std::string colorRangeName(AVColorRange value) {
    if (value == AVCOL_RANGE_UNSPECIFIED) {
        return {};
    }
    const char* name = av_color_range_name(value);
    return name != nullptr && av_color_range_from_name(name) == value ? name : "";
}

std::string colorPrimariesName(AVColorPrimaries value) {
    if (value == AVCOL_PRI_UNSPECIFIED) {
        return {};
    }
    const char* name = av_color_primaries_name(value);
    return name != nullptr && av_color_primaries_from_name(name) == value ? name : "";
}

std::string colorTransferName(AVColorTransferCharacteristic value) {
    if (value == AVCOL_TRC_UNSPECIFIED) {
        return {};
    }
    const char* name = av_color_transfer_name(value);
    return name != nullptr && av_color_transfer_from_name(name) == value ? name : "";
}

std::string colorSpaceName(AVColorSpace value) {
    if (value == AVCOL_SPC_UNSPECIFIED) {
        return {};
    }
    const char* name = av_color_space_name(value);
    return name != nullptr && av_color_space_from_name(name) == value ? name : "";
}

std::string chromaLocationName(AVChromaLocation value) {
    if (value == AVCHROMA_LOC_UNSPECIFIED) {
        return {};
    }
    const char* name = av_chroma_location_name(value);
    return name != nullptr && av_chroma_location_from_name(name) == value ? name : "";
}

std::string fieldOrderName(AVFieldOrder value) {
    switch (value) {
        case AV_FIELD_UNKNOWN: return {};
        case AV_FIELD_PROGRESSIVE: return "progressive";
        case AV_FIELD_TT: return "tt";
        case AV_FIELD_BB: return "bb";
        case AV_FIELD_TB: return "tb";
        case AV_FIELD_BT: return "bt";
        default: return {};
    }
}

bool supportedFieldOrder(AVFieldOrder value) noexcept {
    return value == AV_FIELD_UNKNOWN || value == AV_FIELD_PROGRESSIVE ||
           value == AV_FIELD_TT || value == AV_FIELD_BB ||
           value == AV_FIELD_TB || value == AV_FIELD_BT;
}

bool consistentSampleAspectRatio(const AVStream& stream) noexcept {
    const AVRational streamRatio = stream.sample_aspect_ratio;
    const AVRational codecRatio = stream.codecpar->sample_aspect_ratio;
    return !validRational(streamRatio) || !validRational(codecRatio) ||
           sameRational(streamRatio, codecRatio);
}

AVRational selectedSampleAspectRatio(const AVStream& stream) noexcept {
    if (validRational(stream.sample_aspect_ratio)) {
        return stream.sample_aspect_ratio;
    }
    if (validRational(stream.codecpar->sample_aspect_ratio)) {
        return stream.codecpar->sample_aspect_ratio;
    }
    return AVRational{1, 1};
}

bool hasOnlyNonSemanticTags(const AVDictionary* dictionary,
                            bool formatDictionary) {
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(dictionary, "", entry,
                                AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        std::string key(entry->key != nullptr ? entry->key : "");
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        const std::string_view value(entry->value != nullptr ? entry->value : "");
        if (key == "encoder") {
            continue;
        }
        if (formatDictionary &&
            (key == "major_brand" || key == "minor_version" ||
             key == "compatible_brands")) {
            continue;
        }
        if (!formatDictionary &&
            ((key == "handler_name" && value == "VideoHandler") ||
             (key == "language" && value == "und") ||
             (key == "vendor_id" && value == "[0][0][0][0]"))) {
            continue;
        }
        return false;
    }
    return true;
}

bool hasSupportedDisplayMetadata(const AVStream& stream) {
    const AVCodecParameters& parameters = *stream.codecpar;
    const bool namesSupported =
        (parameters.color_range == AVCOL_RANGE_UNSPECIFIED ||
         !colorRangeName(parameters.color_range).empty()) &&
        (parameters.color_primaries == AVCOL_PRI_UNSPECIFIED ||
         !colorPrimariesName(parameters.color_primaries).empty()) &&
        (parameters.color_trc == AVCOL_TRC_UNSPECIFIED ||
         !colorTransferName(parameters.color_trc).empty()) &&
        (parameters.color_space == AVCOL_SPC_UNSPECIFIED ||
         !colorSpaceName(parameters.color_space).empty()) &&
        (parameters.chroma_location == AVCHROMA_LOC_UNSPECIFIED ||
         !chromaLocationName(parameters.chroma_location).empty());
    return namesSupported && supportedFieldOrder(parameters.field_order) &&
           consistentSampleAspectRatio(stream) &&
           parameters.nb_coded_side_data == 0;
}

bool hasSerializableAudioMetadata(const AVStream& stream) noexcept {
    const AVCodecParameters* parameters = stream.codecpar;
    if (parameters == nullptr || parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
        parameters->codec_id == AV_CODEC_ID_NONE || !validRational(stream.time_base) ||
        parameters->extradata_size < 0 ||
        (parameters->extradata_size > 0 && parameters->extradata == nullptr) ||
        parameters->nb_coded_side_data < 0 ||
        (parameters->nb_coded_side_data > 0 && parameters->coded_side_data == nullptr) ||
        parameters->sample_rate < 0 || parameters->ch_layout.nb_channels < 0) {
        return false;
    }
    return true;
}

bool hasSupportedNativeStreamSet(const AVFormatContext& format,
                                 int videoStreamIndex) noexcept {
    if (format.nb_chapters != 0U || format.nb_programs != 0U) {
        return false;
    }
    std::uint32_t videoStreams = 0U;
    for (unsigned int index = 0; index < format.nb_streams; ++index) {
        const AVStream* stream = format.streams[index];
        if (stream == nullptr || stream->codecpar == nullptr) {
            return false;
        }
        if (static_cast<int>(index) == videoStreamIndex) {
            if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
                (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
                return false;
            }
            ++videoStreams;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!hasSerializableAudioMetadata(*stream)) {
                return false;
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // Multiple video streams, including alternate angles, are not
            // represented by the current single-video native archive.
            return false;
        } else {
            return false;
        }
    }
    return videoStreams == 1U;
}

}  // namespace

std::string ffmpegErrorString(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(errorCode, buffer, sizeof(buffer)) < 0) {
        return "unknown FFmpeg error " + std::to_string(errorCode);
    }
    return buffer;
}

VideoReader::VideoReader(const std::string& path, int threadCount) {
    if (threadCount < 0) {
        throw std::invalid_argument("decoder thread count cannot be negative");
    }

    metadata_.path = path;
    avformat_network_init();

    int result = avformat_open_input(&formatContext_, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        throwFfmpeg("cannot open input '" + path + "'", result);
    }

    result = avformat_find_stream_info(formatContext_, nullptr);
    if (result < 0) {
        release();
        throwFfmpeg("cannot read stream information", result);
    }

    const AVCodec* decoder = nullptr;
    result = av_find_best_stream(formatContext_, AVMEDIA_TYPE_VIDEO, -1, -1,
                                &decoder, 0);
    if (result < 0) {
        release();
        throwFfmpeg("input has no decodable video stream", result);
    }
    videoStreamIndex_ = result;

    AVStream* stream = formatContext_->streams[videoStreamIndex_];
    if (decoder == nullptr) {
        decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    }
    if (decoder == nullptr) {
        release();
        throw std::runtime_error("no FFmpeg decoder is available for this video codec");
    }

    codecContext_ = avcodec_alloc_context3(decoder);
    if (codecContext_ == nullptr) {
        release();
        throw std::bad_alloc();
    }
    result = avcodec_parameters_to_context(codecContext_, stream->codecpar);
    if (result < 0) {
        release();
        throwFfmpeg("cannot configure video decoder", result);
    }

    codecContext_->thread_count = threadCount;  // zero means FFmpeg auto-selects.
    codecContext_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    codecContext_->pkt_timebase = stream->time_base;
    result = avcodec_open2(codecContext_, decoder, nullptr);
    if (result < 0) {
        release();
        throwFfmpeg("cannot initialize video decoder", result);
    }

    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (packet_ == nullptr || frame_ == nullptr) {
        release();
        throw std::bad_alloc();
    }

    metadata_.codec = avcodec_get_name(stream->codecpar->codec_id);
    metadata_.width = codecContext_->width;
    metadata_.height = codecContext_->height;
    metadata_.timeBase = stream->time_base;
    metadata_.colorRange = colorRangeName(stream->codecpar->color_range);
    metadata_.colorPrimaries = colorPrimariesName(stream->codecpar->color_primaries);
    metadata_.colorTransfer = colorTransferName(stream->codecpar->color_trc);
    metadata_.colorSpace = colorSpaceName(stream->codecpar->color_space);
    metadata_.chromaLocation = chromaLocationName(stream->codecpar->chroma_location);
    const AVRational sampleAspectRatio = selectedSampleAspectRatio(*stream);
    metadata_.sampleAspectRatioNumerator =
        static_cast<std::uint32_t>(sampleAspectRatio.num);
    metadata_.sampleAspectRatioDenominator =
        static_cast<std::uint32_t>(sampleAspectRatio.den);
    metadata_.fieldOrder = fieldOrderName(stream->codecpar->field_order);
    metadata_.bitRate = stream->codecpar->bit_rate > 0
                            ? stream->codecpar->bit_rate
                            : formatContext_->bit_rate;
    metadata_.containerFrameHint = stream->nb_frames > 0
                                       ? static_cast<std::uint64_t>(stream->nb_frames)
                                       : 0;
    metadata_.audioStreamCount = 0U;
    for (unsigned int index = 0; index < formatContext_->nb_streams; ++index) {
        const AVStream* candidate = formatContext_->streams[index];
        if (candidate != nullptr && candidate->codecpar != nullptr &&
            candidate->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++metadata_.audioStreamCount;
        }
    }
    AVRational rate = stream->avg_frame_rate;
    if (!validRational(rate)) {
        rate = stream->r_frame_rate;
    }
    metadata_.fps = validRational(rate) ? av_q2d(rate) : 0.0;

    if (formatContext_->duration != AV_NOPTS_VALUE && formatContext_->duration > 0) {
        metadata_.durationSeconds =
            static_cast<double>(formatContext_->duration) / static_cast<double>(AV_TIME_BASE);
    } else if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        metadata_.durationSeconds = av_q2d(stream->time_base) *
                                    static_cast<double>(stream->duration);
    }

    AVPixelFormat pixelFormat = codecContext_->pix_fmt;
    if (pixelFormat == AV_PIX_FMT_NONE && stream->codecpar->format >= 0) {
        pixelFormat = static_cast<AVPixelFormat>(stream->codecpar->format);
    }
    const char* pixelName = av_get_pix_fmt_name(pixelFormat);
    metadata_.pixelFormat = pixelName != nullptr ? pixelName : "unknown";
    metadata_.nativeFrameArchiveEligible =
        hasSupportedNativeStreamSet(*formatContext_, videoStreamIndex_) &&
        hasOnlyNonSemanticTags(formatContext_->metadata, true) &&
        hasOnlyNonSemanticTags(stream->metadata, false) &&
        hasSupportedDisplayMetadata(*stream);
    decoderThreadCount_ = threadCount;
}

VideoReader::~VideoReader() {
    release();
}

void VideoReader::release() noexcept {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codecContext_);
    avformat_close_input(&formatContext_);
}

void VideoReader::drainDecoder(
    const std::function<void(const AVFrame*, std::uint64_t)>& callback,
    std::uint64_t& frameIndex) {
    while (true) {
        const int result = avcodec_receive_frame(codecContext_, frame_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return;
        }
        if (result < 0) {
            throwFfmpeg("video frame decode failed", result);
        }
        callback(frame_, frameIndex);
        ++frameIndex;
        av_frame_unref(frame_);
    }
}

std::uint64_t VideoReader::decode(
    const std::function<void(const AVFrame*, std::uint64_t)>& callback) {
    if (!callback) {
        throw std::invalid_argument("decode callback must be callable");
    }

    std::uint64_t frameIndex = 0;
    while (true) {
        const int readResult = av_read_frame(formatContext_, packet_);
        if (readResult == AVERROR_EOF) {
            break;
        }
        if (readResult < 0) {
            av_packet_unref(packet_);
            throwFfmpeg("failed while reading input packet", readResult);
        }

        if (packet_->stream_index == videoStreamIndex_) {
            int sendResult = avcodec_send_packet(codecContext_, packet_);
            if (sendResult == AVERROR(EAGAIN)) {
                drainDecoder(callback, frameIndex);
                sendResult = avcodec_send_packet(codecContext_, packet_);
            }
            av_packet_unref(packet_);
            if (sendResult < 0) {
                throwFfmpeg("failed to submit packet to video decoder", sendResult);
            }
            drainDecoder(callback, frameIndex);
        } else {
            av_packet_unref(packet_);
        }
    }

    int flushResult = avcodec_send_packet(codecContext_, nullptr);
    if (flushResult == AVERROR(EAGAIN)) {
        drainDecoder(callback, frameIndex);
        flushResult = avcodec_send_packet(codecContext_, nullptr);
    }
    if (flushResult < 0 && flushResult != AVERROR_EOF) {
        throwFfmpeg("failed to flush video decoder", flushResult);
    }
    if (flushResult != AVERROR_EOF) {
        drainDecoder(callback, frameIndex);
    }
    return frameIndex;
}

double frameTimestampSeconds(const AVFrame* frame,
                             const VideoMetadata& metadata,
                             std::uint64_t fallbackIndex) {
    if (frame != nullptr && frame->best_effort_timestamp != AV_NOPTS_VALUE &&
        validRational(metadata.timeBase)) {
        return av_q2d(metadata.timeBase) *
               static_cast<double>(frame->best_effort_timestamp);
    }
    if (metadata.fps > 0.0) {
        return static_cast<double>(fallbackIndex) / metadata.fps;
    }
    return 0.0;
}

}  // namespace frameforge
