#include "block_motion_codec.hpp"

#include "frameforge/frame_codec.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Exact VFS3 native-video framing for yuv420p: 83-byte stream header, a
// 32-byte frame header+checksum, and a 16-byte footer.
constexpr std::uint64_t kVfs3HeaderBytes = 83U;
constexpr std::uint64_t kVfsFrameRecordOverhead = 32U;
constexpr std::uint64_t kVfsFooterBytes = 16U;
// Estimated VFS4 feature declaration; motion records use a four-byte vector
// stream length followed by the compressed vector table and residual payload.
constexpr std::uint64_t kMotionFeatureHeaderBytes = 8U;
constexpr std::uint64_t kMotionVectorLengthBytes = 4U;

struct Options {
    std::filesystem::path rawPath;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t frames = 0U;
    std::uint32_t repeats = 3U;
};

struct AdaptivePacket {
    bool usesBlockMotion = false;
    frameforge::EncodedFrame baseline;
    frameforge::experiment::BlockMotionFrame motion;
};

[[noreturn]] void usageError(const std::string& message) {
    throw std::invalid_argument(message +
        "\nusage: frameforge_block_motion_benchmark --raw FILE --width N "
        "--height N --frames N [--repeats N]");
}

std::uint32_t parsePositive(const std::string& value, const char* option) {
    std::uint32_t parsed = 0U;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed == 0U) {
        usageError(std::string(option) + " must be a positive integer");
    }
    return parsed;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (key == "--help") {
            std::cout << "Codec-only VFS3 versus reversible block-motion benchmark\n"
                         "Options: --raw FILE --width N --height N --frames N "
                         "[--repeats N]\n";
            std::exit(0);
        }
        if (i + 1 >= argc) {
            usageError("missing value for " + std::string(key));
        }
        const std::string value(argv[++i]);
        if (key == "--raw") {
            options.rawPath = value;
        } else if (key == "--width") {
            options.width = parsePositive(value, "--width");
        } else if (key == "--height") {
            options.height = parsePositive(value, "--height");
        } else if (key == "--frames") {
            options.frames = parsePositive(value, "--frames");
        } else if (key == "--repeats") {
            options.repeats = parsePositive(value, "--repeats");
        } else {
            usageError("unknown option " + std::string(key));
        }
    }
    if (options.rawPath.empty() || options.width == 0U || options.height == 0U ||
        options.frames == 0U || options.repeats == 0U) {
        usageError("--raw, --width, --height, and --frames are required");
    }
    if ((options.width % 2U) != 0U || (options.height % 2U) != 0U) {
        usageError("yuv420p width and height must be even");
    }
    return options;
}

std::size_t checkedFrameBytes(std::uint32_t width, std::uint32_t height) {
    const std::size_t w = width;
    const std::size_t h = height;
    if (w > std::numeric_limits<std::size_t>::max() / h) {
        throw std::length_error("frame dimensions overflow addressable memory");
    }
    const std::size_t luma = w * h;
    if (luma > std::numeric_limits<std::size_t>::max() / 3U) {
        throw std::length_error("YUV420 frame size overflows addressable memory");
    }
    return luma * 3U / 2U;
}

std::vector<std::uint8_t> readRaw(const Options& options, std::size_t frameBytes) {
    if (static_cast<std::size_t>(options.frames) >
        std::numeric_limits<std::size_t>::max() / frameBytes) {
        throw std::length_error("raw sequence size overflows addressable memory");
    }
    const std::size_t expectedBytes =
        static_cast<std::size_t>(options.frames) * frameBytes;
    std::error_code error;
    const std::uintmax_t actualBytes =
        std::filesystem::file_size(options.rawPath, error);
    if (error || actualBytes != expectedBytes) {
        throw std::runtime_error("raw file size does not match the requested yuv420p sequence");
    }
    std::vector<std::uint8_t> raw(expectedBytes);
    std::ifstream input(options.rawPath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open raw frame file: " + options.rawPath.string());
    }
    input.read(reinterpret_cast<char*>(raw.data()),
               static_cast<std::streamsize>(raw.size()));
    if (input.gcount() != static_cast<std::streamsize>(raw.size())) {
        throw std::runtime_error("raw frame file ended before all expected bytes were read");
    }
    return raw;
}

std::span<const std::uint8_t> frameAt(const std::vector<std::uint8_t>& raw,
                                      std::size_t frameBytes,
                                      std::size_t index) {
    return std::span<const std::uint8_t>(raw).subspan(index * frameBytes, frameBytes);
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    if ((values.size() % 2U) != 0U) {
        return values[middle];
    }
    return (values[middle - 1U] + values[middle]) / 2.0;
}

std::uint64_t archiveSize(std::uint64_t headerBytes,
                          const std::vector<frameforge::EncodedFrame>& packets) {
    std::uint64_t total = headerBytes + kVfsFooterBytes;
    if (packets.size() >
        (std::numeric_limits<std::uint64_t>::max() - total) /
            kVfsFrameRecordOverhead) {
        throw std::length_error("archive frame headers overflow the VFS size field");
    }
    total += static_cast<std::uint64_t>(packets.size()) * kVfsFrameRecordOverhead;
    for (const auto& packet : packets) {
        if (packet.payload.size() > std::numeric_limits<std::uint64_t>::max() - total) {
            throw std::length_error("archive payload total overflows the VFS size field");
        }
        total += static_cast<std::uint64_t>(packet.payload.size());
    }
    return total;
}

std::uint64_t archiveSize(std::uint64_t headerBytes,
                          const std::vector<AdaptivePacket>& packets,
                          std::uint64_t& vectorMetadataBytes,
                          std::uint64_t& compressedVectorBytes,
                          std::uint64_t& motionFrameCount) {
    std::uint64_t total = headerBytes + kVfsFooterBytes;
    if (packets.size() >
        (std::numeric_limits<std::uint64_t>::max() - total) /
            kVfsFrameRecordOverhead) {
        throw std::length_error("archive frame headers overflow the VFS size field");
    }
    total += static_cast<std::uint64_t>(packets.size()) * kVfsFrameRecordOverhead;
    vectorMetadataBytes = 0U;
    compressedVectorBytes = 0U;
    motionFrameCount = 0U;
    for (const AdaptivePacket& packet : packets) {
        std::uint64_t payloadBytes = 0U;
        if (packet.usesBlockMotion) {
            ++motionFrameCount;
            const std::uint64_t vectorBytes = static_cast<std::uint64_t>(
                packet.motion.compressedVectors.size());
            const std::uint64_t residualBytes = static_cast<std::uint64_t>(
                packet.motion.compressedResidual.size());
            if (vectorBytes > std::numeric_limits<std::uint64_t>::max() -
                                  kMotionVectorLengthBytes - residualBytes) {
                throw std::length_error("motion payload size overflows the VFS size field");
            }
            payloadBytes = kMotionVectorLengthBytes + vectorBytes + residualBytes;
            vectorMetadataBytes += kMotionVectorLengthBytes + vectorBytes;
            compressedVectorBytes += vectorBytes;
        } else {
            payloadBytes = static_cast<std::uint64_t>(packet.baseline.payload.size());
        }
        if (payloadBytes > std::numeric_limits<std::uint64_t>::max() - total) {
            throw std::length_error("archive payload total overflows the VFS size field");
        }
        total += payloadBytes;
    }
    if (motionFrameCount != 0U) {
        if (kMotionFeatureHeaderBytes > std::numeric_limits<std::uint64_t>::max() - total) {
            throw std::length_error("motion feature header overflows the VFS size field");
        }
        total += kMotionFeatureHeaderBytes;
    }
    return total;
}

std::vector<frameforge::EncodedFrame> encodeBaseline(
    const Options& options, const std::vector<std::uint8_t>& raw,
    std::size_t frameBytes, const frameforge::FrameLayout& layout,
    std::vector<double>& timings) {
    std::vector<frameforge::EncodedFrame> finalPackets;
    for (std::uint32_t repeat = 0U; repeat < options.repeats; ++repeat) {
        std::vector<frameforge::EncodedFrame> packets;
        packets.reserve(options.frames);
        const auto start = Clock::now();
        for (std::size_t index = 0U; index < options.frames; ++index) {
            const auto current = frameAt(raw, frameBytes, index);
            const auto previous = index == 0U
                ? std::span<const std::uint8_t>{}
                : frameAt(raw, frameBytes, index - 1U);
            packets.push_back(frameforge::encodeLosslessFrame(
                current, layout, previous, 1,
                frameforge::CompressionCodec::Zstandard));
        }
        timings.push_back(std::chrono::duration<double>(Clock::now() - start).count());
        if (repeat + 1U == options.repeats) {
            finalPackets = std::move(packets);
        }
    }
    return finalPackets;
}

std::vector<AdaptivePacket> encodeAdaptive(
    const Options& options, const std::vector<std::uint8_t>& raw,
    std::size_t frameBytes, const frameforge::FrameLayout& layout,
    std::vector<double>& timings) {
    std::vector<AdaptivePacket> finalPackets;
    for (std::uint32_t repeat = 0U; repeat < options.repeats; ++repeat) {
        std::vector<AdaptivePacket> packets;
        packets.reserve(options.frames);
        const auto start = Clock::now();
        for (std::size_t index = 0U; index < options.frames; ++index) {
            const auto current = frameAt(raw, frameBytes, index);
            const auto previous = index == 0U
                ? std::span<const std::uint8_t>{}
                : frameAt(raw, frameBytes, index - 1U);
            AdaptivePacket packet;
            packet.baseline = frameforge::encodeLosslessFrame(
                current, layout, previous, 1,
                frameforge::CompressionCodec::Zstandard);
            if (index != 0U) {
                packet.motion = frameforge::experiment::encodeBlockMotion(
                    current, previous, layout, 1);
                const std::size_t motionBytes = kMotionVectorLengthBytes +
                    packet.motion.compressedVectors.size() +
                    packet.motion.compressedResidual.size();
                if (motionBytes < packet.baseline.payload.size()) {
                    packet.usesBlockMotion = true;
                    packet.baseline = {};
                } else {
                    packet.motion = {};
                }
            }
            packets.push_back(std::move(packet));
        }
        timings.push_back(std::chrono::duration<double>(Clock::now() - start).count());
        if (repeat + 1U == options.repeats) {
            finalPackets = std::move(packets);
        }
    }
    return finalPackets;
}

double timeBaselineDecode(const Options& options,
                         const std::vector<frameforge::EncodedFrame>& packets) {
    std::vector<std::uint8_t> previous;
    const auto start = Clock::now();
    for (std::size_t index = 0U; index < options.frames; ++index) {
        previous = frameforge::decodeLosslessFrame(packets[index], previous);
    }
    return std::chrono::duration<double>(Clock::now() - start).count();
}

double timeAdaptiveDecode(const Options& options,
                         const std::vector<AdaptivePacket>& packets) {
    std::vector<std::uint8_t> previous;
    const auto start = Clock::now();
    for (std::size_t index = 0U; index < options.frames; ++index) {
        const auto& packet = packets[index];
        if (packet.usesBlockMotion) {
            previous = frameforge::experiment::decodeBlockMotion(packet.motion, previous);
        } else {
            previous = frameforge::decodeLosslessFrame(packet.baseline, previous);
        }
    }
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool verifyBaseline(const Options& options, const std::vector<std::uint8_t>& raw,
                    std::size_t frameBytes,
                    const std::vector<frameforge::EncodedFrame>& packets) {
    std::vector<std::uint8_t> previous;
    for (std::size_t index = 0U; index < options.frames; ++index) {
        previous = frameforge::decodeLosslessFrame(packets[index], previous);
        const auto expected = frameAt(raw, frameBytes, index);
        if (!std::equal(previous.begin(), previous.end(), expected.begin(), expected.end())) {
            return false;
        }
    }
    return true;
}

bool verifyAdaptive(const Options& options, const std::vector<std::uint8_t>& raw,
                    std::size_t frameBytes,
                    const std::vector<AdaptivePacket>& packets) {
    std::vector<std::uint8_t> previous;
    for (std::size_t index = 0U; index < options.frames; ++index) {
        const AdaptivePacket& packet = packets[index];
        if (packet.usesBlockMotion) {
            previous = frameforge::experiment::decodeBlockMotion(packet.motion, previous);
        } else {
            previous = frameforge::decodeLosslessFrame(packet.baseline, previous);
        }
        const auto expected = frameAt(raw, frameBytes, index);
        if (!std::equal(previous.begin(), previous.end(), expected.begin(), expected.end())) {
            return false;
        }
    }
    return true;
}

void printJson(const Options& options,
               std::uint64_t baselineBytes,
               std::uint64_t adaptiveBytes,
               std::uint64_t vectorMetadataBytes,
               std::uint64_t compressedVectorBytes,
               std::uint64_t motionFrameCount,
               double baselinePackSeconds,
               double adaptivePackSeconds,
               double baselineUnpackSeconds,
               double adaptiveUnpackSeconds,
               bool baselineExact,
               bool adaptiveExact) {
    std::cout << std::fixed << std::setprecision(9)
              << "{\"width\":" << options.width
              << ",\"height\":" << options.height
              << ",\"frames\":" << options.frames
              << ",\"baseline_vfs3_native_bytes\":" << baselineBytes
              << ",\"adaptive_motion_archive_bytes\":" << adaptiveBytes
              << ",\"adaptive_size_delta_bytes\":"
              << (static_cast<std::int64_t>(adaptiveBytes) -
                  static_cast<std::int64_t>(baselineBytes))
              << ",\"selected_motion_frames\":" << motionFrameCount
              << ",\"vector_metadata_bytes\":" << vectorMetadataBytes
              << ",\"compressed_vector_bytes\":" << compressedVectorBytes
              << ",\"v4_feature_header_bytes\":"
              << (motionFrameCount == 0U ? 0U : kMotionFeatureHeaderBytes)
              << ",\"baseline_pack_seconds\":" << baselinePackSeconds
              << ",\"adaptive_pack_seconds\":" << adaptivePackSeconds
              << ",\"baseline_unpack_seconds\":" << baselineUnpackSeconds
              << ",\"adaptive_unpack_seconds\":" << adaptiveUnpackSeconds
              << ",\"baseline_exact\":" << (baselineExact ? "true" : "false")
              << ",\"adaptive_exact\":" << (adaptiveExact ? "true" : "false")
              << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const std::size_t frameBytes = checkedFrameBytes(options.width, options.height);
        const std::vector<std::uint8_t> raw = readRaw(options, frameBytes);
        const frameforge::FrameLayout layout{
            options.width, options.height, 1U,
            {{options.width, options.height, 1U},
             {options.width / 2U, options.height / 2U, 1U},
             {options.width / 2U, options.height / 2U, 1U}}};

        std::vector<double> baselinePackTimes;
        std::vector<double> adaptivePackTimes;
        baselinePackTimes.reserve(options.repeats);
        adaptivePackTimes.reserve(options.repeats);
        const auto baselinePackets = encodeBaseline(
            options, raw, frameBytes, layout, baselinePackTimes);
        const auto adaptivePackets = encodeAdaptive(
            options, raw, frameBytes, layout, adaptivePackTimes);
        const std::uint64_t baselineBytes = archiveSize(
            kVfs3HeaderBytes, baselinePackets);
        std::uint64_t vectorMetadataBytes = 0U;
        std::uint64_t compressedVectorBytes = 0U;
        std::uint64_t motionFrameCount = 0U;
        const std::uint64_t adaptiveBytes = archiveSize(
            kVfs3HeaderBytes, adaptivePackets, vectorMetadataBytes,
            compressedVectorBytes, motionFrameCount);

        std::vector<double> baselineUnpackTimes;
        std::vector<double> adaptiveUnpackTimes;
        baselineUnpackTimes.reserve(options.repeats);
        adaptiveUnpackTimes.reserve(options.repeats);
        for (std::uint32_t repeat = 0U; repeat < options.repeats; ++repeat) {
            baselineUnpackTimes.push_back(timeBaselineDecode(options, baselinePackets));
            adaptiveUnpackTimes.push_back(timeAdaptiveDecode(options, adaptivePackets));
        }
        const bool baselineExact = verifyBaseline(
            options, raw, frameBytes, baselinePackets);
        const bool adaptiveExact = verifyAdaptive(
            options, raw, frameBytes, adaptivePackets);
        if (!baselineExact || !adaptiveExact) {
            throw std::runtime_error("one or more codec paths failed byte-exact validation");
        }
        printJson(options, baselineBytes, adaptiveBytes, vectorMetadataBytes,
                  compressedVectorBytes, motionFrameCount,
                  median(std::move(baselinePackTimes)),
                  median(std::move(adaptivePackTimes)),
                  median(std::move(baselineUnpackTimes)),
                  median(std::move(adaptiveUnpackTimes)),
                  baselineExact, adaptiveExact);
    } catch (const std::exception& error) {
        std::cerr << "block-motion benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
