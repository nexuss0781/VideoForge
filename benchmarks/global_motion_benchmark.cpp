#include "global_motion_gate.hpp"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kVfs3HeaderBytes = 83U;
constexpr std::uint64_t kVfsRecordBytes = 32U;
constexpr std::uint64_t kVfsFooterBytes = 16U;
constexpr std::uint64_t kGlobalMotionFeatureBytes = 8U;
constexpr std::uint64_t kGlobalMotionMetadataBytes = 6U;

struct Options {
    std::filesystem::path rawPath;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t frames = 0U;
    std::uint32_t repeats = 3U;
};

struct CandidatePacket {
    bool useMotion = false;
    bool gatePassed = false;
    frameforge::EncodedFrame baseline;
    frameforge::experiment::GatedGlobalMotionFrame motion;
};

[[noreturn]] void usageError(const std::string& message) {
    throw std::invalid_argument(message +
        "\nusage: frameforge_global_motion_benchmark --raw FILE --width N "
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
            std::cout << "Codec-only VFS3 versus gated global-translation benchmark\n"
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
    if (options.width <= 64U || options.height <= 64U) {
        usageError("global-motion experiment requires both dimensions greater than 64");
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
    const std::size_t expectedBytes = static_cast<std::size_t>(options.frames) * frameBytes;
    std::error_code error;
    const std::uintmax_t actualBytes = std::filesystem::file_size(options.rawPath, error);
    if (error || actualBytes != expectedBytes) {
        throw std::runtime_error("raw file size does not match the requested yuv420p sequence");
    }
    std::vector<std::uint8_t> raw(expectedBytes);
    std::ifstream input(options.rawPath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open raw frame file: " + options.rawPath.string());
    }
    input.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
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

std::uint64_t baseArchiveBytes(std::uint32_t frameCount,
                               const std::vector<frameforge::EncodedFrame>& packets) {
    std::uint64_t total = kVfs3HeaderBytes + kVfsFooterBytes +
                          static_cast<std::uint64_t>(frameCount) * kVfsRecordBytes;
    for (const auto& packet : packets) {
        total += static_cast<std::uint64_t>(packet.payload.size());
    }
    return total;
}

std::uint64_t candidateArchiveBytes(std::uint32_t frameCount,
                                    const std::vector<CandidatePacket>& packets,
                                    std::uint64_t& selectedFrames,
                                    std::uint64_t& gateFrames) {
    std::uint64_t total = kVfs3HeaderBytes + kVfsFooterBytes +
                          static_cast<std::uint64_t>(frameCount) * kVfsRecordBytes;
    selectedFrames = 0U;
    gateFrames = 0U;
    for (const CandidatePacket& packet : packets) {
        if (packet.gatePassed) {
            ++gateFrames;
        }
        if (!packet.useMotion) {
            total += static_cast<std::uint64_t>(packet.baseline.payload.size());
            continue;
        }
        ++selectedFrames;
        total += kGlobalMotionMetadataBytes;
        total += static_cast<std::uint64_t>(packet.motion.refined.compressedVectors.size());
        total += static_cast<std::uint64_t>(packet.motion.refined.compressedResidual.size());
    }
    if (selectedFrames != 0U) {
        total += kGlobalMotionFeatureBytes;
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
                current, layout, previous, 1, frameforge::CompressionCodec::Zstandard));
        }
        timings.push_back(std::chrono::duration<double>(Clock::now() - start).count());
        if (repeat + 1U == options.repeats) {
            finalPackets = std::move(packets);
        }
    }
    return finalPackets;
}

std::vector<CandidatePacket> encodeGated(
    const Options& options, const std::vector<std::uint8_t>& raw,
    std::size_t frameBytes, const frameforge::FrameLayout& layout,
    std::vector<double>& timings) {
    std::vector<CandidatePacket> finalPackets;
    for (std::uint32_t repeat = 0U; repeat < options.repeats; ++repeat) {
        std::vector<CandidatePacket> packets;
        packets.reserve(options.frames);
        std::uint64_t selected = 0U;
        const auto start = Clock::now();
        for (std::size_t index = 0U; index < options.frames; ++index) {
            const auto current = frameAt(raw, frameBytes, index);
            const auto previous = index == 0U
                ? std::span<const std::uint8_t>{}
                : frameAt(raw, frameBytes, index - 1U);
            CandidatePacket packet;
            packet.baseline = frameforge::encodeLosslessFrame(
                current, layout, previous, 1, frameforge::CompressionCodec::Zstandard);
            if (index != 0U) {
                packet.motion = frameforge::experiment::encodeGatedGlobalMotion(
                    current, previous, layout, 1);
                packet.gatePassed = packet.motion.estimate.gatePassed;
                if (packet.motion.estimate.gatePassed) {
                    const std::uint64_t motionBytes = kGlobalMotionMetadataBytes +
                        static_cast<std::uint64_t>(
                            packet.motion.refined.compressedVectors.size()) +
                        static_cast<std::uint64_t>(
                            packet.motion.refined.compressedResidual.size());
                    const std::uint64_t featureCost = selected == 0U
                        ? kGlobalMotionFeatureBytes
                        : 0U;
                    if (motionBytes + featureCost < packet.baseline.payload.size()) {
                        packet.useMotion = true;
                        packet.baseline = {};
                        ++selected;
                    } else {
                        packet.motion = {};
                    }
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

double timeGatedDecode(const Options& options,
                       const std::vector<CandidatePacket>& packets) {
    std::vector<std::uint8_t> previous;
    const auto start = Clock::now();
    for (std::size_t index = 0U; index < options.frames; ++index) {
        const CandidatePacket& packet = packets[index];
        previous = packet.useMotion
            ? frameforge::experiment::decodeGatedGlobalMotion(packet.motion, previous)
            : frameforge::decodeLosslessFrame(packet.baseline, previous);
    }
    return std::chrono::duration<double>(Clock::now() - start).count();
}

template <typename PacketAt>
bool verifyDecoded(const Options& options, const std::vector<std::uint8_t>& raw,
                   std::size_t frameBytes, PacketAt&& packetAt) {
    std::vector<std::uint8_t> previous;
    for (std::size_t index = 0U; index < options.frames; ++index) {
        previous = packetAt(index, previous);
        const auto expected = frameAt(raw, frameBytes, index);
        if (!std::equal(previous.begin(), previous.end(), expected.begin(), expected.end())) {
            return false;
        }
    }
    return true;
}

void printJson(const Options& options, std::uint64_t baselineBytes,
               std::uint64_t candidateBytes, std::uint64_t selectedFrames,
               std::uint64_t gateFrames, double baselineEncodeSeconds,
               double candidateEncodeSeconds, double baselineDecodeSeconds,
               double candidateDecodeSeconds, bool baselineExact,
               bool candidateExact) {
    std::cout << std::fixed << std::setprecision(9)
              << "{\"width\":" << options.width
              << ",\"height\":" << options.height
              << ",\"frames\":" << options.frames
              << ",\"baseline_vfs3_native_bytes\":" << baselineBytes
              << ",\"gated_candidate_archive_bytes\":" << candidateBytes
              << ",\"size_delta_bytes\":"
              << (static_cast<std::int64_t>(candidateBytes) -
                  static_cast<std::int64_t>(baselineBytes))
              << ",\"gate_pass_frames\":" << gateFrames
              << ",\"selected_motion_frames\":" << selectedFrames
              << ",\"baseline_encode_seconds\":" << baselineEncodeSeconds
              << ",\"gated_encode_seconds\":" << candidateEncodeSeconds
              << ",\"baseline_decode_seconds\":" << baselineDecodeSeconds
              << ",\"gated_decode_seconds\":" << candidateDecodeSeconds
              << ",\"baseline_exact\":" << (baselineExact ? "true" : "false")
              << ",\"gated_exact\":" << (candidateExact ? "true" : "false")
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

        std::vector<double> baselineEncodeTimes;
        std::vector<double> gatedEncodeTimes;
        baselineEncodeTimes.reserve(options.repeats);
        gatedEncodeTimes.reserve(options.repeats);
        const auto baselinePackets = encodeBaseline(
            options, raw, frameBytes, layout, baselineEncodeTimes);
        const auto gatedPackets = encodeGated(
            options, raw, frameBytes, layout, gatedEncodeTimes);
        const std::uint64_t baselineBytes = baseArchiveBytes(options.frames, baselinePackets);
        std::uint64_t selectedFrames = 0U;
        std::uint64_t gateFrames = 0U;
        const std::uint64_t gatedBytes = candidateArchiveBytes(
            options.frames, gatedPackets, selectedFrames, gateFrames);

        std::vector<double> baselineDecodeTimes;
        std::vector<double> gatedDecodeTimes;
        baselineDecodeTimes.reserve(options.repeats);
        gatedDecodeTimes.reserve(options.repeats);
        for (std::uint32_t repeat = 0U; repeat < options.repeats; ++repeat) {
            baselineDecodeTimes.push_back(timeBaselineDecode(options, baselinePackets));
            gatedDecodeTimes.push_back(timeGatedDecode(options, gatedPackets));
        }
        const bool baselineExact = verifyDecoded(
            options, raw, frameBytes,
            [&](std::size_t index, std::span<const std::uint8_t> previous) {
                return frameforge::decodeLosslessFrame(baselinePackets[index], previous);
            });
        const bool gatedExact = verifyDecoded(
            options, raw, frameBytes,
            [&](std::size_t index, std::span<const std::uint8_t> previous) {
                const CandidatePacket& packet = gatedPackets[index];
                return packet.useMotion
                    ? frameforge::experiment::decodeGatedGlobalMotion(packet.motion, previous)
                    : frameforge::decodeLosslessFrame(packet.baseline, previous);
            });
        if (!baselineExact || !gatedExact) {
            throw std::runtime_error("one or more codec paths failed byte-exact validation");
        }
        printJson(options, baselineBytes, gatedBytes, selectedFrames, gateFrames,
                  median(std::move(baselineEncodeTimes)),
                  median(std::move(gatedEncodeTimes)),
                  median(std::move(baselineDecodeTimes)),
                  median(std::move(gatedDecodeTimes)), baselineExact, gatedExact);
    } catch (const std::exception& error) {
        std::cerr << "global-motion benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
