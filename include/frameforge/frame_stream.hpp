#pragma once

#include "frameforge/frame_codec.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <span>
#include <utility>
#include <vector>

namespace frameforge {

// Timestamps are integer ticks in timeBaseNumerator/timeBaseDenominator seconds.
// For example, {1, 1000} means one millisecond per tick.
struct FrameStreamMetadata {
    FrameStreamMetadata() = default;
    FrameStreamMetadata(FrameLayout frameLayout, std::uint32_t numerator,
                       std::uint32_t denominator, std::string format)
        : layout(std::move(frameLayout)), timeBaseNumerator(numerator),
          timeBaseDenominator(denominator), pixelFormat(std::move(format)) {}

    FrameLayout layout;
    std::uint32_t timeBaseNumerator = 1;
    std::uint32_t timeBaseDenominator = 1000;
    // Empty for packed-byte streams. Native video streams describe tightly
    // packed decoded planes in layout.planes; VFS2 is the legacy version.
    std::string pixelFormat;
    // VFS4 stores supported display metadata as FFmpeg's stable textual names.
    // Empty enum names mean unspecified; sample aspect ratio defaults to 1:1.
    std::string colorRange;
    std::string colorPrimaries;
    std::string colorTransfer;
    std::string colorSpace;
    std::string chromaLocation;
    std::uint32_t sampleAspectRatioNumerator = 1;
    std::uint32_t sampleAspectRatioDenominator = 1;
    std::string fieldOrder;
};

struct FrameDisplayState {
    bool interlaced = false;
    bool topFieldFirst = false;
    std::int32_t repeatPicture = 0;
};

struct DecodedStreamFrame {
    std::uint64_t index = 0;
    std::int64_t timestamp = 0;
    std::vector<std::uint8_t> pixels;
    FrameDisplayState displayState;
};

// Writes VFS3 packed-byte streams or VFS4 native-video streams with explicit
// per-frame codec identifiers and supported display metadata. Call finish()
// explicitly to write the frame-count footer; destroying an unfinished writer
// leaves an intentionally invalid/truncated stream. Audio and arbitrary side
// data are not represented.
class FrameStreamWriter final {
public:
    FrameStreamWriter(std::ostream& output, FrameStreamMetadata metadata,
                      int compressionLevel = 1,
                      CompressionCodec compressionCodec = CompressionCodec::Zstandard,
                      bool includesAudioExtension = false);

    FrameStreamWriter(const FrameStreamWriter&) = delete;
    FrameStreamWriter& operator=(const FrameStreamWriter&) = delete;

    void writeFrame(std::span<const std::uint8_t> pixels,
                    std::int64_t timestamp,
                    FrameDisplayState displayState = {});
    void finish();

    [[nodiscard]] std::uint64_t frameCount() const noexcept { return frameCount_; }
    [[nodiscard]] const FrameStreamMetadata& metadata() const noexcept { return metadata_; }
    [[nodiscard]] std::uint16_t formatVersion() const noexcept { return formatVersion_; }

private:
    std::ostream& output_;
    FrameStreamMetadata metadata_;
    int compressionLevel_;
    CompressionCodec compressionCodec_;
    std::uint16_t formatVersion_ = 0;
    std::uint64_t frameCount_ = 0;
    std::vector<std::uint8_t> previousFrame_;
    bool finished_ = false;
};

// Reads and verifies VFS1/VFS2/VFS3/VFS4 streams. Legacy VFS1/VFS2 DEFLATE and raw
// payloads remain readable. Frame payload checksums are validated before
// decoding. A configurable size cap prevents hostile headers from triggering
// unbounded frame allocations (default: 512 MiB per packed frame).
class FrameStreamReader final {
public:
    explicit FrameStreamReader(std::istream& input,
        std::uint64_t maxDecodedFrameBytes = 512ULL * 1024ULL * 1024ULL);

    FrameStreamReader(const FrameStreamReader&) = delete;
    FrameStreamReader& operator=(const FrameStreamReader&) = delete;

    // Returns false only after a valid footer is read. Truncation, corruption,
    // unsupported versions, malformed records, and size-limit violations throw
    // standard exceptions (including runtime_error and length_error).
    [[nodiscard]] bool readNext(DecodedStreamFrame& frame);

    [[nodiscard]] std::uint64_t frameCount() const noexcept { return frameCount_; }
    [[nodiscard]] const FrameStreamMetadata& metadata() const noexcept { return metadata_; }
    [[nodiscard]] std::uint16_t formatVersion() const noexcept { return formatVersion_; }

private:
    std::istream& input_;
    FrameStreamMetadata metadata_;
    std::uint16_t formatVersion_ = 0;
    std::size_t decodedFrameBytes_ = 0;
    std::uint64_t frameCount_ = 0;
    std::vector<std::uint8_t> previousFrame_;
    bool finished_ = false;
};

}  // namespace frameforge
