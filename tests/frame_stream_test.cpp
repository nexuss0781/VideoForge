#include "frameforge/frame_stream.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Callable>
void requireThrows(Callable&& callable, const std::string& message) {
    bool threw = false;
    try {
        callable();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, message);
}

std::string makeStream(const frameforge::FrameStreamMetadata& metadata,
                       const std::vector<std::vector<std::uint8_t>>& frames,
                       const std::vector<std::int64_t>& timestamps,
                       frameforge::CompressionCodec codec =
                           frameforge::CompressionCodec::Zstandard) {
    std::ostringstream output(std::ios::binary);
    frameforge::FrameStreamWriter writer(output, metadata, 1, codec);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        writer.writeFrame(frames[i], timestamps[i]);
    }
    writer.finish();
    return output.str();
}

std::uint64_t loadLittleEndian(const std::string& bytes, std::size_t offset,
                               std::size_t count) {
    std::uint64_t value = 0U;
    for (std::size_t index = 0; index < count; ++index) {
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

void storeLittleEndian32(std::string& bytes, std::size_t offset,
                         std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes[offset + index] = static_cast<char>(value & 0xffU);
        value >>= 8U;
    }
}

std::uint32_t crc32Range(const std::string& bytes, std::size_t offset,
                        std::size_t count) {
    const auto* data = reinterpret_cast<const Bytef*>(bytes.data() + offset);
    return static_cast<std::uint32_t>(crc32(0L, data, static_cast<uInt>(count)));
}

std::string makeLegacyDeflateStream(
    const frameforge::FrameStreamMetadata& metadata,
    const std::vector<std::uint8_t>& pixels, std::int64_t timestamp,
    std::uint16_t legacyVersion) {
    const bool native = legacyVersion == 2U;
    if ((legacyVersion != 1U && legacyVersion != 2U) ||
        native != !metadata.pixelFormat.empty()) {
        throw std::invalid_argument("test legacy stream metadata/version mismatch");
    }
    const std::size_t bodyBytes = native
        ? 24U + 4U + metadata.pixelFormat.size() + metadata.layout.planes.size() * 12U
        : 24U;
    std::string bytes = makeStream(metadata, {pixels}, {timestamp},
                                   frameforge::CompressionCodec::Deflate);
    if (native) {
        const std::size_t metadataExtensionBytes =
            14U + metadata.colorRange.size() + metadata.colorPrimaries.size() +
            metadata.colorTransfer.size() + metadata.colorSpace.size() +
            metadata.chromaLocation.size() + metadata.fieldOrder.size();
        const std::size_t extensionOffset = 8U + bodyBytes;
        bytes.erase(extensionOffset, metadataExtensionBytes);
    }
    bytes[8] = static_cast<char>(legacyVersion & 0xffU);
    bytes[9] = static_cast<char>((legacyVersion >> 8U) & 0xffU);
    const std::size_t headerChecksumOffset = 8U + bodyBytes;
    storeLittleEndian32(bytes, headerChecksumOffset, crc32Range(bytes, 8U, bodyBytes));

    const std::size_t recordOffset = headerChecksumOffset + 4U;
    if (native) {
        bytes.erase(recordOffset + 28U, 5U);
    }
    bytes[recordOffset + 3U] = 0;
    const std::uint64_t payloadBytes = loadLittleEndian(bytes, recordOffset + 20U, 8U);
    const std::size_t recordCrcOffset = recordOffset + 28U +
                                        static_cast<std::size_t>(payloadBytes);
    storeLittleEndian32(bytes, recordCrcOffset,
                        crc32Range(bytes, recordOffset,
                                   28U + static_cast<std::size_t>(payloadBytes)));
    return bytes;
}

void roundTripSequenceAndTimestamps() {
    const frameforge::FrameStreamMetadata metadata{{79U, 43U, 3U, {}}, 1U, 90000U, {}};
    const std::size_t size = static_cast<std::size_t>(metadata.layout.width) *
                             metadata.layout.height * metadata.layout.channels;
    std::mt19937 generator(0x5eedU);
    std::vector<std::vector<std::uint8_t>> frames(5U, std::vector<std::uint8_t>(size));
    for (auto& frame : frames) {
        for (std::uint8_t& value : frame) {
            value = static_cast<std::uint8_t>(generator() & 0xffU);
        }
    }
    for (std::size_t i = 0; i < frames[0].size(); i += 83U) {
        frames[1][i] = static_cast<std::uint8_t>(frames[0][i] + 1U);
        frames[2][i] = static_cast<std::uint8_t>(frames[1][i] ^ 0x43U);
    }
    const std::vector<std::int64_t> timestamps{
        -9000, 0, 3600, 3600, std::numeric_limits<std::int64_t>::max()};
    const std::string bytes = makeStream(metadata, frames, timestamps);
    std::istringstream input(bytes, std::ios::binary);
    frameforge::FrameStreamReader reader(input);
    require(reader.metadata().layout.width == metadata.layout.width &&
                reader.metadata().layout.height == metadata.layout.height &&
                reader.metadata().layout.channels == metadata.layout.channels,
            "stream layout metadata changed during serialization");
    require(reader.metadata().timeBaseNumerator == 1U &&
                reader.metadata().timeBaseDenominator == 90000U,
            "stream timestamp timebase changed during serialization");

    frameforge::DecodedStreamFrame decoded;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        require(reader.readNext(decoded), "stream ended before all frames were read");
        require(decoded.index == i, "decoded frame index is incorrect");
        require(decoded.timestamp == timestamps[i], "decoded timestamp is not exact");
        require(decoded.pixels == frames[i], "stream frame bytes changed during round-trip");
    }
    require(!reader.readNext(decoded), "valid stream footer was not recognized");
    require(!reader.readNext(decoded), "reader did not remain at clean end-of-stream");
    require(reader.frameCount() == frames.size(), "reader frame count is incorrect");
}

void roundTripNativePlaneMetadata() {
    const frameforge::FrameStreamMetadata metadata{
        frameforge::FrameLayout{4U, 2U, 1U,
            {{4U, 2U, 1U}, {2U, 1U, 1U}, {2U, 1U, 1U}}},
        1U, 24U, "yuv420p"};
    const std::vector<std::vector<std::uint8_t>> frames{
        {16U, 32U, 48U, 64U, 80U, 96U, 112U, 128U, 144U, 160U, 176U, 192U},
        {17U, 32U, 49U, 64U, 80U, 97U, 112U, 129U, 144U, 161U, 176U, 193U}};
    const std::vector<std::int64_t> timestamps{0, 1};
    const std::string bytes = makeStream(metadata, frames, timestamps);
    std::istringstream input(bytes, std::ios::binary);
    frameforge::FrameStreamReader reader(input);
    require(reader.metadata().pixelFormat == "yuv420p",
            "native pixel-format metadata changed during serialization");
    require(reader.metadata().layout.width == 4U &&
                reader.metadata().layout.height == 2U &&
                reader.metadata().layout.planes == metadata.layout.planes,
            "native plane geometry changed during serialization");
    frameforge::DecodedStreamFrame decoded;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        require(reader.readNext(decoded), "native stream ended before all frames were read");
        require(decoded.timestamp == timestamps[index],
                "native stream timestamp is not exact");
        require(decoded.pixels == frames[index],
                "native plane bytes changed during stream round-trip");
    }
    require(!reader.readNext(decoded), "native stream footer was not recognized");
}

void roundTripVfs4DisplayMetadata() {
    frameforge::FrameStreamMetadata metadata{
        {8U, 4U, 1U, {{8U, 4U, 1U}}}, 1U, 25U, "gray"};
    metadata.colorRange = "tv";
    metadata.colorPrimaries = "bt709";
    metadata.colorTransfer = "bt709";
    metadata.colorSpace = "bt709";
    metadata.chromaLocation = "left";
    metadata.sampleAspectRatioNumerator = 4U;
    metadata.sampleAspectRatioDenominator = 3U;
    metadata.fieldOrder = "tt";
    const std::vector<std::uint8_t> pixels(32U, 0x7aU);
    std::ostringstream output(std::ios::binary);
    frameforge::FrameStreamWriter writer(output, metadata);
    writer.writeFrame(pixels, 12, {true, true, 2});
    writer.finish();

    const std::string bytes = output.str();
    require(static_cast<unsigned char>(bytes[8]) == 4U,
            "native display metadata did not select VFS4");
    std::istringstream input(bytes, std::ios::binary);
    frameforge::FrameStreamReader reader(input);
    const auto& restoredMetadata = reader.metadata();
    require(restoredMetadata.colorRange == metadata.colorRange &&
                restoredMetadata.colorPrimaries == metadata.colorPrimaries &&
                restoredMetadata.colorTransfer == metadata.colorTransfer &&
                restoredMetadata.colorSpace == metadata.colorSpace &&
                restoredMetadata.chromaLocation == metadata.chromaLocation &&
                restoredMetadata.sampleAspectRatioNumerator == 4U &&
                restoredMetadata.sampleAspectRatioDenominator == 3U &&
                restoredMetadata.fieldOrder == metadata.fieldOrder,
            "VFS4 display metadata changed during serialization");
    frameforge::DecodedStreamFrame decoded;
    require(reader.readNext(decoded), "VFS4 stream ended before its frame");
    require(decoded.pixels == pixels && decoded.timestamp == 12 &&
                decoded.displayState.interlaced &&
                decoded.displayState.topFieldFirst &&
                decoded.displayState.repeatPicture == 2,
            "VFS4 frame display state changed during serialization");
    require(!reader.readNext(decoded), "VFS4 stream footer was not recognized");
}

void roundTripVfs5VideoPrefix() {
    const frameforge::FrameStreamMetadata metadata{
        {8U, 4U, 1U, {{8U, 4U, 1U}}}, 1U, 25U, "gray"};
    const std::vector<std::uint8_t> pixels(32U, 0x41U);
    std::ostringstream output(std::ios::binary);
    frameforge::FrameStreamWriter writer(output, metadata, 1,
        frameforge::CompressionCodec::Zstandard, true);
    writer.writeFrame(pixels, 17);
    writer.finish();
    output << "audio-extension";

    const std::string bytes = output.str();
    require(static_cast<unsigned char>(bytes[8]) == 5U &&
                static_cast<unsigned char>(bytes[9]) == 0U,
            "audio-enabled native stream did not select VFS5");
    std::istringstream input(bytes, std::ios::binary);
    frameforge::FrameStreamReader reader(input);
    require(reader.formatVersion() == 5U,
            "VFS5 version was not exposed by the reader");
    frameforge::DecodedStreamFrame decoded;
    require(reader.readNext(decoded) && decoded.pixels == pixels && decoded.timestamp == 17,
            "VFS5 frame prefix did not round-trip");
    require(!reader.readNext(decoded), "VFS5 frame footer was not recognized");
    require(input.peek() == 'a', "VFS5 frame reader consumed or rejected the audio trailer");
}

void legacyDeflateCompatibility() {
    const frameforge::FrameStreamMetadata packed{{32U, 16U, 1U, {}}, 1U, 25U, {}};
    const std::vector<std::uint8_t> packedPixels(32U * 16U, 0x7bU);
    const std::string vfs1 = makeLegacyDeflateStream(packed, packedPixels, 17, 1U);
    {
        std::istringstream input(vfs1, std::ios::binary);
        frameforge::FrameStreamReader reader(input);
        frameforge::DecodedStreamFrame decoded;
        require(reader.readNext(decoded) && decoded.pixels == packedPixels &&
                    decoded.timestamp == 17,
                "legacy VFS1 DEFLATE record did not decode exactly");
        require(!reader.readNext(decoded), "legacy VFS1 footer was not recognized");
    }

    const frameforge::FrameStreamMetadata native{
        {16U, 8U, 1U, {{16U, 8U, 1U}}}, 1U, 24U, "gray"};
    const std::vector<std::uint8_t> nativePixels(16U * 8U, 0x93U);
    const std::string vfs2 = makeLegacyDeflateStream(native, nativePixels, -3, 2U);
    {
        std::istringstream input(vfs2, std::ios::binary);
        frameforge::FrameStreamReader reader(input);
        frameforge::DecodedStreamFrame decoded;
        require(reader.readNext(decoded) && decoded.pixels == nativePixels &&
                    decoded.timestamp == -3 && reader.metadata().pixelFormat == "gray",
                "legacy VFS2 DEFLATE record did not decode exactly");
        require(!reader.readNext(decoded), "legacy VFS2 footer was not recognized");
    }
}

void emptyAndDeterministicStreams() {
    const frameforge::FrameStreamMetadata metadata{{2U, 2U, 1U, {}}, 1U, 25U, {}};
    const std::string first = makeStream(metadata, {}, {});
    const std::string second = makeStream(metadata, {}, {});
    require(first == second, "empty stream serialization is not deterministic");
    std::istringstream input(first, std::ios::binary);
    frameforge::FrameStreamReader reader(input);
    frameforge::DecodedStreamFrame frame;
    require(!reader.readNext(frame), "empty stream did not terminate at its footer");
    require(reader.frameCount() == 0U, "empty stream has a nonzero frame count");
}

void rejectCorruptAndTruncatedStreams() {
    const frameforge::FrameStreamMetadata metadata{{8U, 4U, 1U, {}}, 1U, 30U, {}};
    const std::vector<std::vector<std::uint8_t>> frames{
        std::vector<std::uint8_t>(32U, 0x5aU),
        std::vector<std::uint8_t>(32U, 0x5bU)};
    const std::vector<std::int64_t> timestamps{0, 1};
    const std::string valid = makeStream(metadata, frames, timestamps);

    std::string corruptedHeader = valid;
    corruptedHeader[12] ^= 0x40;
    requireThrows([&] {
        std::istringstream input(corruptedHeader, std::ios::binary);
        frameforge::FrameStreamReader reader(input);
        (void)reader;
    }, "header checksum corruption was accepted");

    std::string corruptedPayload = valid;
    // Header (36 bytes), record header (28 bytes), then one payload byte.
    corruptedPayload[64] ^= 0x01;
    requireThrows([&] {
        std::istringstream input(corruptedPayload, std::ios::binary);
        frameforge::FrameStreamReader reader(input);
        frameforge::DecodedStreamFrame frame;
        (void)reader.readNext(frame);
    }, "frame payload checksum corruption was accepted");

    std::string missingFooter = valid;
    missingFooter.resize(missingFooter.size() - 16U);
    requireThrows([&] {
        std::istringstream input(missingFooter, std::ios::binary);
        frameforge::FrameStreamReader reader(input);
        frameforge::DecodedStreamFrame frame;
        while (reader.readNext(frame)) {}
    }, "stream without a complete footer was accepted");

    requireThrows([&] {
        std::istringstream input(valid, std::ios::binary);
        frameforge::FrameStreamReader reader(input, 16U);
        (void)reader;
    }, "frame larger than configured allocation cap was accepted");

    std::string trailing = valid + "x";
    requireThrows([&] {
        std::istringstream input(trailing, std::ios::binary);
        frameforge::FrameStreamReader reader(input);
        frameforge::DecodedStreamFrame frame;
        while (reader.readNext(frame)) {}
    }, "trailing bytes after the footer were accepted");
}

void rejectInvalidWriterInputs() {
    std::ostringstream output(std::ios::binary);
    requireThrows([&] {
        frameforge::FrameStreamWriter writer(output, {{1U, 1U, 1U, {}}, 0U, 1U, {}});
        (void)writer;
    }, "zero timestamp timebase was accepted");

    std::ostringstream output2(std::ios::binary);
    frameforge::FrameStreamWriter writer(output2, {{2U, 2U, 1U, {}}, 1U, 25U, {}});
    const std::vector<std::uint8_t> wrongSize(3U, 0U);
    requireThrows([&] { writer.writeFrame(wrongSize, 0); },
                  "frame with the wrong byte count was accepted");
    const std::vector<std::uint8_t> valid(4U, 0U);
    writer.writeFrame(valid, 0);
    writer.finish();
    requireThrows([&] { writer.writeFrame(valid, 1); },
                  "frame write after finish was accepted");
    requireThrows([&] { writer.finish(); }, "duplicate finish was accepted");
}

}  // namespace

int main() {
    try {
        roundTripSequenceAndTimestamps();
        roundTripNativePlaneMetadata();
        roundTripVfs4DisplayMetadata();
        roundTripVfs5VideoPrefix();
        legacyDeflateCompatibility();
        emptyAndDeterministicStreams();
        rejectCorruptAndTruncatedStreams();
        rejectInvalidWriterInputs();
    } catch (const std::exception& error) {
        std::cerr << "frame stream test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "frame stream tests passed\n";
    return 0;
}
