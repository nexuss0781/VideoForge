#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
}

namespace frameforge {

struct AudioSideData {
    std::uint32_t type = 0;
    std::vector<std::uint8_t> bytes;

    friend bool operator==(const AudioSideData&, const AudioSideData&) = default;
};

// Codec/container-independent snapshot of the audio stream parameters required
// to remux compressed packets without decoding or re-encoding them.
struct AudioStreamInfo {
    std::uint32_t archiveStreamIndex = 0;
    std::int32_t codecId = 0;
    std::uint32_t codecTag = 0;
    std::int32_t format = -1;
    std::int32_t profile = -1;
    std::int32_t level = -1;
    std::int64_t bitRate = 0;
    std::int32_t bitsPerCodedSample = 0;
    std::int32_t bitsPerRawSample = 0;
    std::int32_t sampleRate = 0;
    std::int32_t blockAlign = 0;
    std::int32_t frameSize = 0;
    std::int32_t initialPadding = 0;
    std::int32_t trailingPadding = 0;
    std::int32_t seekPreroll = 0;
    std::int32_t channelCount = 0;
    std::int32_t timeBaseNumerator = 0;
    std::int32_t timeBaseDenominator = 1;
    std::uint32_t disposition = 0;
    std::int32_t containerStreamId = -1;
    std::string codecName;
    std::string channelLayout;
    std::vector<std::uint8_t> extradata;
    std::vector<AudioSideData> codedSideData;
    std::map<std::string, std::string> metadata;

    friend bool operator==(const AudioStreamInfo&, const AudioStreamInfo&) = default;
};

struct AudioPacketRecord {
    std::uint32_t archiveStreamIndex = 0;
    std::int64_t pts = INT64_MIN;
    std::int64_t dts = INT64_MIN;
    std::int64_t duration = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint8_t> payload;
    std::vector<AudioSideData> sideData;
};

// Versioned, checksummed extension stored after a VFS5 video frame stream.
// Packet payloads are copied verbatim; packet timestamps remain in each source
// audio stream's own time base.
class AudioStreamWriter final {
public:
    AudioStreamWriter(std::ostream& output, std::vector<AudioStreamInfo> streams);
    AudioStreamWriter(const AudioStreamWriter&) = delete;
    AudioStreamWriter& operator=(const AudioStreamWriter&) = delete;

    void writePacket(std::uint32_t archiveStreamIndex, const AVPacket& packet);
    void finish();
    [[nodiscard]] std::uint64_t packetCount() const noexcept { return packetCount_; }

private:
    std::ostream& output_;
    std::vector<AudioStreamInfo> streams_;
    std::uint64_t packetCount_ = 0;
    bool finished_ = false;
};

class AudioStreamReader final {
public:
    explicit AudioStreamReader(std::istream& input,
                               std::uint64_t maxPacketBytes =
                                   512ULL * 1024ULL * 1024ULL);
    AudioStreamReader(const AudioStreamReader&) = delete;
    AudioStreamReader& operator=(const AudioStreamReader&) = delete;

    [[nodiscard]] const std::vector<AudioStreamInfo>& streams() const noexcept {
        return streams_;
    }
    // Returns false only after a valid footer and end-of-extension are verified.
    [[nodiscard]] bool readNext(AudioPacketRecord& packet);
    [[nodiscard]] std::uint64_t packetCount() const noexcept { return packetCount_; }

private:
    std::istream& input_;
    std::uint64_t maxPacketBytes_;
    std::vector<AudioStreamInfo> streams_;
    std::uint64_t packetCount_ = 0;
    bool finished_ = false;
};

}  // namespace frameforge
