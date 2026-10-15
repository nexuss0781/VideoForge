#include "frameforge/audio_remux.hpp"

#include "frameforge/video_reader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
}

namespace frameforge {
namespace {

[[noreturn]] void throwFfmpeg(const std::string& operation, int errorCode) {
    throw std::runtime_error(operation + ": " + ffmpegErrorString(errorCode));
}

class InputFormat final {
public:
    explicit InputFormat(const std::filesystem::path& path) {
        int result = avformat_open_input(&context_, path.string().c_str(), nullptr, nullptr);
        if (result < 0) {
            throwFfmpeg("cannot open input '" + path.string() + "'", result);
        }
        result = avformat_find_stream_info(context_, nullptr);
        if (result < 0) {
            release();
            throwFfmpeg("cannot read source stream information", result);
        }
    }
    ~InputFormat() { release(); }
    InputFormat(const InputFormat&) = delete;
    InputFormat& operator=(const InputFormat&) = delete;
    [[nodiscard]] AVFormatContext* get() const noexcept { return context_; }
private:
    void release() noexcept { avformat_close_input(&context_); }
    AVFormatContext* context_ = nullptr;
};

AudioStreamInfo describeAudioStream(const AVStream& stream,
                                    std::uint32_t archiveIndex) {
    const AVCodecParameters& parameters = *stream.codecpar;
    if (parameters.codec_type != AVMEDIA_TYPE_AUDIO ||
        parameters.codec_id == AV_CODEC_ID_NONE || stream.time_base.num <= 0 ||
        stream.time_base.den <= 0 || parameters.extradata_size < 0 ||
        (parameters.extradata_size > 0 && parameters.extradata == nullptr) ||
        parameters.nb_coded_side_data < 0 ||
        (parameters.nb_coded_side_data > 0 && parameters.coded_side_data == nullptr)) {
        throw std::runtime_error("audio stream metadata cannot be represented safely in VFS5");
    }

    AudioStreamInfo info;
    info.archiveStreamIndex = archiveIndex;
    info.codecId = static_cast<std::int32_t>(parameters.codec_id);
    info.codecTag = parameters.codec_tag;
    info.format = parameters.format;
    info.profile = parameters.profile;
    info.level = parameters.level;
    info.bitRate = parameters.bit_rate;
    info.bitsPerCodedSample = parameters.bits_per_coded_sample;
    info.bitsPerRawSample = parameters.bits_per_raw_sample;
    info.sampleRate = parameters.sample_rate;
    info.blockAlign = parameters.block_align;
    info.frameSize = parameters.frame_size;
    info.initialPadding = parameters.initial_padding;
    info.trailingPadding = parameters.trailing_padding;
    info.seekPreroll = parameters.seek_preroll;
    info.channelCount = parameters.ch_layout.nb_channels;
    info.timeBaseNumerator = stream.time_base.num;
    info.timeBaseDenominator = stream.time_base.den;
    info.disposition = static_cast<std::uint32_t>(stream.disposition);
    info.containerStreamId = stream.id;
    const char* codecName = avcodec_get_name(parameters.codec_id);
    info.codecName = codecName != nullptr ? codecName : "unknown";
    if (parameters.ch_layout.nb_channels > 0) {
        std::array<char, 1024> description{};
        const int result = av_channel_layout_describe(&parameters.ch_layout,
                                                       description.data(),
                                                       description.size());
        if (result < 0) {
            throwFfmpeg("cannot describe audio channel layout", result);
        }
        info.channelLayout = description.data();
    }
    if (parameters.extradata_size > 0) {
        info.extradata.assign(parameters.extradata,
                              parameters.extradata + parameters.extradata_size);
    }
    info.codedSideData.reserve(static_cast<std::size_t>(parameters.nb_coded_side_data));
    for (int index = 0; index < parameters.nb_coded_side_data; ++index) {
        const AVPacketSideData& source = parameters.coded_side_data[index];
        AudioSideData side;
        side.type = static_cast<std::uint32_t>(source.type);
        if (source.size > 0U) {
            if (source.data == nullptr) {
                throw std::runtime_error("audio codec side data has a null payload");
            }
            side.bytes.assign(source.data, source.data + source.size);
        }
        info.codedSideData.push_back(std::move(side));
    }
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(stream.metadata, "", entry,
                                AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        const std::string key = entry->key != nullptr ? entry->key : "";
        const std::string value = entry->value != nullptr ? entry->value : "";
        if (key.empty() || !info.metadata.emplace(key, value).second) {
            throw std::runtime_error("audio stream contains duplicate or empty metadata keys");
        }
    }
    return info;
}

std::unique_ptr<AVCodecParameters, void (*)(AVCodecParameters*)>
makeCodecParameters(const AudioStreamInfo& info) {
    const char* expectedCodecName = info.codecId > 0
        ? avcodec_get_name(static_cast<AVCodecID>(info.codecId)) : nullptr;
    if (expectedCodecName == nullptr || info.codecName != expectedCodecName ||
        info.timeBaseNumerator <= 0 || info.timeBaseDenominator <= 0 ||
        info.sampleRate < 0 || info.channelCount < 0) {
        throw std::runtime_error("archived audio codec parameters are inconsistent");
    }
    AVCodecParameters* raw = avcodec_parameters_alloc();
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    std::unique_ptr<AVCodecParameters, void (*)(AVCodecParameters*)> parameters(
        raw, [](AVCodecParameters* value) { avcodec_parameters_free(&value); });
    raw->codec_type = AVMEDIA_TYPE_AUDIO;
    raw->codec_id = static_cast<AVCodecID>(info.codecId);
    raw->codec_tag = info.codecTag;
    raw->format = info.format;
    raw->profile = info.profile;
    raw->level = info.level;
    raw->bit_rate = info.bitRate;
    raw->bits_per_coded_sample = info.bitsPerCodedSample;
    raw->bits_per_raw_sample = info.bitsPerRawSample;
    raw->sample_rate = info.sampleRate;
    raw->block_align = info.blockAlign;
    raw->frame_size = info.frameSize;
    raw->initial_padding = info.initialPadding;
    raw->trailing_padding = info.trailingPadding;
    raw->seek_preroll = info.seekPreroll;
    if (!info.channelLayout.empty()) {
        const int result = av_channel_layout_from_string(&raw->ch_layout,
                                                          info.channelLayout.c_str());
        if (result < 0) {
            throwFfmpeg("cannot restore archived audio channel layout", result);
        }
    } else if (info.channelCount > 0) {
        av_channel_layout_default(&raw->ch_layout, info.channelCount);
    }
    if (raw->ch_layout.nb_channels != info.channelCount) {
        throw std::runtime_error("archived audio channel count conflicts with its channel layout");
    }
    if (!info.extradata.empty()) {
        if (info.extradata.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) {
            throw std::length_error("archived audio codec extradata is too large for FFmpeg");
        }
        raw->extradata = static_cast<std::uint8_t*>(av_mallocz(
            info.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (raw->extradata == nullptr) {
            throw std::bad_alloc();
        }
        std::memcpy(raw->extradata, info.extradata.data(), info.extradata.size());
        raw->extradata_size = static_cast<int>(info.extradata.size());
    }
    for (const AudioSideData& side : info.codedSideData) {
        if (side.type >= static_cast<std::uint32_t>(AV_PKT_DATA_NB)) {
            throw std::runtime_error("VFS audio codec side data uses an unknown type");
        }
        AVPacketSideData* item = av_packet_side_data_new(
            &raw->coded_side_data, &raw->nb_coded_side_data,
            static_cast<AVPacketSideDataType>(side.type), side.bytes.size(), 0);
        if (item == nullptr) {
            throw std::bad_alloc();
        }
        if (!side.bytes.empty()) {
            std::memcpy(item->data, side.bytes.data(), side.bytes.size());
        }
    }
    return parameters;
}

std::int64_t mapTimestampExactly(std::int64_t value, AVRational source,
                                 AVRational destination, const char* description) {
    if (value == AV_NOPTS_VALUE) {
        return AV_NOPTS_VALUE;
    }
    const std::int64_t mapped = av_rescale_q(value, source, destination);
    if (av_compare_ts(value, source, mapped, destination) != 0) {
        throw std::runtime_error(std::string("NUT output cannot preserve audio/video ") +
                                 description + " exactly");
    }
    return mapped;
}

AVPacket* makePacket(const AudioPacketRecord& record) {
    if (record.payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("archived audio packet is too large for FFmpeg");
    }
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        throw std::bad_alloc();
    }
    const int result = av_new_packet(packet, static_cast<int>(record.payload.size()));
    if (result < 0) {
        av_packet_free(&packet);
        throwFfmpeg("cannot allocate restored audio packet", result);
    }
    if (!record.payload.empty()) {
        std::memcpy(packet->data, record.payload.data(), record.payload.size());
    }
    packet->pts = record.pts;
    packet->dts = record.dts;
    packet->duration = record.duration;
    packet->flags = std::bit_cast<int>(record.flags);
    for (const AudioSideData& side : record.sideData) {
        if (side.type >= static_cast<std::uint32_t>(AV_PKT_DATA_NB)) {
            av_packet_free(&packet);
            throw std::runtime_error("archived audio packet side data uses an unknown type");
        }
        std::uint8_t* data = av_packet_new_side_data(
            packet, static_cast<AVPacketSideDataType>(side.type), side.bytes.size());
        if (data == nullptr) {
            av_packet_free(&packet);
            throw std::bad_alloc();
        }
        if (!side.bytes.empty()) {
            std::memcpy(data, side.bytes.data(), side.bytes.size());
        }
    }
    return packet;
}

void copyMetadata(AVDictionary** destination,
                  const std::map<std::string, std::string>& metadata) {
    for (const auto& [key, value] : metadata) {
        const int result = av_dict_set(destination, key.c_str(), value.c_str(), 0);
        if (result < 0) {
            throwFfmpeg("cannot restore audio stream metadata", result);
        }
    }
}

}  // namespace

AudioInputStreams inspectAudioStreams(const std::filesystem::path& source) {
    InputFormat input(source);
    AVFormatContext* format = input.get();
    AudioInputStreams result;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        AVStream* stream = format->streams[index];
        if (stream != nullptr && stream->codecpar != nullptr &&
            stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            const std::uint32_t archiveIndex = static_cast<std::uint32_t>(result.streams.size());
            result.streams.push_back(describeAudioStream(*stream, archiveIndex));
            result.inputStreamIndices.push_back(static_cast<int>(index));
        }
    }
    return result;
}

bool audioStreamsCanUseNut(const std::vector<AudioStreamInfo>& streams) {
    if (streams.empty()) {
        return false;
    }
    const AVOutputFormat* nut = av_guess_format("nut", nullptr, nullptr);
    if (nut == nullptr) {
        return false;
    }
    for (const AudioStreamInfo& stream : streams) {
        if (stream.codecId <= 0 ||
            avformat_query_codec(nut, static_cast<AVCodecID>(stream.codecId),
                                 FF_COMPLIANCE_NORMAL) != 1) {
            return false;
        }
        if (!stream.channelLayout.empty()) {
            AVChannelLayout parsed{};
            if (av_channel_layout_from_string(&parsed, stream.channelLayout.c_str()) < 0 ||
                parsed.nb_channels != stream.channelCount) {
                av_channel_layout_uninit(&parsed);
                return false;
            }
            av_channel_layout_uninit(&parsed);
        } else if (stream.channelCount != 0) {
            return false;
        }
        for (const AudioSideData& side : stream.codedSideData) {
            if (side.type >= static_cast<std::uint32_t>(AV_PKT_DATA_NB)) {
                return false;
            }
        }
    }
    return true;
}

void writeAudioExtension(const std::filesystem::path& source,
                         const AudioInputStreams& streams,
                         std::ostream& output) {
    if (streams.streams.empty() ||
        streams.inputStreamIndices.size() != streams.streams.size()) {
        throw std::invalid_argument("audio stream description is empty or inconsistent");
    }
    InputFormat input(source);
    AVFormatContext* format = input.get();
    std::vector<int> archiveIndexByInput(format->nb_streams, -1);
    for (std::size_t i = 0; i < streams.inputStreamIndices.size(); ++i) {
        const int streamIndex = streams.inputStreamIndices[i];
        if (streamIndex < 0 || static_cast<unsigned int>(streamIndex) >= format->nb_streams ||
            format->streams[static_cast<unsigned int>(streamIndex)]->codecpar->codec_type !=
                AVMEDIA_TYPE_AUDIO) {
            throw std::runtime_error("source audio stream layout changed during VFS5 packing");
        }
        if (describeAudioStream(
                *format->streams[static_cast<unsigned int>(streamIndex)],
                static_cast<std::uint32_t>(i)) != streams.streams[i]) {
            throw std::runtime_error("source audio metadata changed during VFS5 packing");
        }
        archiveIndexByInput[static_cast<std::size_t>(streamIndex)] = static_cast<int>(i);
    }

    AudioStreamWriter writer(output, streams.streams);
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        throw std::bad_alloc();
    }
    try {
        while (true) {
            const int result = av_read_frame(format, packet);
            if (result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                throwFfmpeg("failed while reading source audio packets", result);
            }
            if (packet->stream_index >= 0 &&
                static_cast<std::size_t>(packet->stream_index) < archiveIndexByInput.size()) {
                const int archiveIndex = archiveIndexByInput[
                    static_cast<std::size_t>(packet->stream_index)];
                if (archiveIndex >= 0) {
                    writer.writePacket(static_cast<std::uint32_t>(archiveIndex), *packet);
                }
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        writer.finish();
    } catch (...) {
        av_packet_free(&packet);
        throw;
    }
}

void muxNutVideoAndAudio(const std::filesystem::path& videoOnlyNut,
                         std::istream& audioExtension,
                         const std::filesystem::path& outputNut) {
    AudioStreamReader audioReader(audioExtension);
    const std::vector<AudioStreamInfo>& audioStreams = audioReader.streams();
    InputFormat videoInput(videoOnlyNut);
    AVFormatContext* input = videoInput.get();
    int videoInputIndex = -1;
    for (unsigned int index = 0; index < input->nb_streams; ++index) {
        if (input->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (videoInputIndex >= 0) {
                throw std::runtime_error("temporary VFS video stream unexpectedly contains multiple video tracks");
            }
            videoInputIndex = static_cast<int>(index);
        } else {
            throw std::runtime_error("temporary VFS video stream contains an unexpected non-video track");
        }
    }
    if (videoInputIndex < 0 || audioStreams.empty()) {
        throw std::runtime_error("VFS5 video/audio stream inventory is incomplete");
    }
    AVStream* inputVideoStream = input->streams[videoInputIndex];

    AVFormatContext* output = nullptr;
    int result = avformat_alloc_output_context2(&output, nullptr, "nut",
                                                 outputNut.string().c_str());
    if (result < 0 || output == nullptr) {
        throwFfmpeg("cannot create NUT audio/video container",
                    result < 0 ? result : AVERROR_UNKNOWN);
    }
    AVPacket* videoPacket = av_packet_alloc();
    AVPacket* audioPacket = av_packet_alloc();
    auto cleanup = [&]() noexcept {
        av_packet_free(&videoPacket);
        av_packet_free(&audioPacket);
        if (output != nullptr) {
            if (output->pb != nullptr) {
                avio_closep(&output->pb);
            }
            avformat_free_context(output);
            output = nullptr;
        }
    };
    if (videoPacket == nullptr || audioPacket == nullptr) {
        cleanup();
        throw std::bad_alloc();
    }
    try {
        AVStream* outputVideoStream = avformat_new_stream(output, nullptr);
        if (outputVideoStream == nullptr) {
            throw std::bad_alloc();
        }
        result = avcodec_parameters_copy(outputVideoStream->codecpar,
                                          inputVideoStream->codecpar);
        if (result < 0) {
            throwFfmpeg("cannot copy lossless video parameters into NUT", result);
        }
        outputVideoStream->time_base = inputVideoStream->time_base;
        outputVideoStream->sample_aspect_ratio = inputVideoStream->sample_aspect_ratio;
        outputVideoStream->disposition = inputVideoStream->disposition;
        outputVideoStream->id = inputVideoStream->id;
        result = av_dict_copy(&outputVideoStream->metadata, inputVideoStream->metadata, 0);
        if (result < 0) {
            throwFfmpeg("cannot copy video stream metadata into NUT", result);
        }

        std::vector<AVStream*> outputAudioStreams;
        outputAudioStreams.reserve(audioStreams.size());
        for (const AudioStreamInfo& info : audioStreams) {
            AVStream* stream = avformat_new_stream(output, nullptr);
            if (stream == nullptr) {
                throw std::bad_alloc();
            }
            const auto parameters = makeCodecParameters(info);
            result = avcodec_parameters_copy(stream->codecpar, parameters.get());
            if (result < 0) {
                throwFfmpeg("cannot restore archived audio codec parameters", result);
            }
            stream->time_base = AVRational{info.timeBaseNumerator,
                                            info.timeBaseDenominator};
            stream->disposition = static_cast<int>(info.disposition);
            stream->id = info.containerStreamId;
            copyMetadata(&stream->metadata, info.metadata);
            outputAudioStreams.push_back(stream);
        }

        if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
            result = avio_open(&output->pb, outputNut.string().c_str(), AVIO_FLAG_WRITE);
            if (result < 0) {
                throwFfmpeg("cannot open restored NUT output", result);
            }
        }
        result = avformat_write_header(output, nullptr);
        if (result < 0) {
            throwFfmpeg("cannot write NUT audio/video header", result);
        }

        const AVRational inputVideoTimeBase = inputVideoStream->time_base;
        const AVRational outputVideoTimeBase = outputVideoStream->time_base;
        bool haveVideoPacket = false;
        bool haveAudioPacket = false;
        bool audioEnded = false;
        const auto readVideoPacket = [&]() {
            while (true) {
                const int readResult = av_read_frame(input, videoPacket);
                if (readResult == AVERROR_EOF) {
                    return false;
                }
                if (readResult < 0) {
                    throwFfmpeg("failed reading temporary lossless video packets", readResult);
                }
                if (videoPacket->stream_index == videoInputIndex) {
                    return true;
                }
                av_packet_unref(videoPacket);
            }
        };
        const auto readAudioPacket = [&]() {
            AudioPacketRecord record;
            if (!audioReader.readNext(record)) {
                audioEnded = true;
                return false;
            }
            AVPacket* decoded = makePacket(record);
            av_packet_move_ref(audioPacket, decoded);
            av_packet_free(&decoded);
            audioPacket->stream_index = static_cast<int>(record.archiveStreamIndex) + 1;
            return true;
        };
        haveVideoPacket = readVideoPacket();
        haveAudioPacket = readAudioPacket();
        while (haveVideoPacket || haveAudioPacket) {
            bool takeAudio = false;
            if (!haveVideoPacket) {
                takeAudio = true;
            } else if (haveAudioPacket) {
                const std::uint32_t audioIndex =
                    static_cast<std::uint32_t>(audioPacket->stream_index - 1);
                const AVRational audioTimeBase{
                    audioStreams[audioIndex].timeBaseNumerator,
                    audioStreams[audioIndex].timeBaseDenominator};
                const std::int64_t audioDts = audioPacket->dts != AV_NOPTS_VALUE
                    ? audioPacket->dts : audioPacket->pts;
                const std::int64_t videoDts = videoPacket->dts != AV_NOPTS_VALUE
                    ? videoPacket->dts : videoPacket->pts;
                if (audioDts == AV_NOPTS_VALUE || videoDts == AV_NOPTS_VALUE) {
                    takeAudio = audioDts == AV_NOPTS_VALUE && videoDts != AV_NOPTS_VALUE;
                } else {
                    takeAudio = av_compare_ts(audioDts, audioTimeBase,
                                              videoDts, inputVideoTimeBase) <= 0;
                }
            }

            if (takeAudio) {
                const std::uint32_t archiveIndex =
                    static_cast<std::uint32_t>(audioPacket->stream_index - 1);
                const AVRational sourceTimeBase{
                    audioStreams[archiveIndex].timeBaseNumerator,
                    audioStreams[archiveIndex].timeBaseDenominator};
                AVStream* destination = outputAudioStreams[archiveIndex];
                const AVRational destinationTimeBase = destination->time_base;
                audioPacket->pts = mapTimestampExactly(audioPacket->pts, sourceTimeBase,
                                                       destinationTimeBase, "audio PTS");
                audioPacket->dts = mapTimestampExactly(audioPacket->dts, sourceTimeBase,
                                                       destinationTimeBase, "audio DTS");
                audioPacket->duration = mapTimestampExactly(audioPacket->duration,
                    sourceTimeBase, destinationTimeBase, "audio packet duration");
                audioPacket->stream_index = static_cast<int>(archiveIndex) + 1;
                audioPacket->pos = -1;
                result = av_interleaved_write_frame(output, audioPacket);
                if (result < 0) {
                    throwFfmpeg("cannot remux archived compressed audio packet", result);
                }
                haveAudioPacket = readAudioPacket();
            } else {
                videoPacket->pts = mapTimestampExactly(videoPacket->pts,
                    inputVideoTimeBase, outputVideoTimeBase, "video PTS");
                videoPacket->dts = mapTimestampExactly(videoPacket->dts,
                    inputVideoTimeBase, outputVideoTimeBase, "video DTS");
                videoPacket->duration = mapTimestampExactly(videoPacket->duration,
                    inputVideoTimeBase, outputVideoTimeBase, "video packet duration");
                videoPacket->stream_index = 0;
                videoPacket->pos = -1;
                result = av_interleaved_write_frame(output, videoPacket);
                if (result < 0) {
                    throwFfmpeg("cannot remux lossless video packet", result);
                }
                haveVideoPacket = readVideoPacket();
            }
        }
        if (!audioEnded) {
            AudioPacketRecord extra;
            if (audioReader.readNext(extra)) {
                throw std::runtime_error("VFS audio packet iterator ended inconsistently");
            }
        }
        result = av_write_trailer(output);
        if (result < 0) {
            throwFfmpeg("cannot finalize NUT audio/video output", result);
        }
        if (output->pb != nullptr) {
            result = avio_closep(&output->pb);
            if (result < 0) {
                throwFfmpeg("cannot close restored NUT output", result);
            }
        }
        cleanup();
    } catch (...) {
        cleanup();
        throw;
    }
}

}  // namespace frameforge
