#include "frameforge/frame_codec.hpp"
#include "frameforge/language_transform.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zstd.h>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMathematicalSideInfoBytes = 2U;

struct Options {
    std::uint32_t width = 320U;
    std::uint32_t height = 180U;
    std::uint32_t repeats = 3U;
};

struct BitDepth {
    std::uint32_t bits;
    std::uint8_t storageBytes;
};

enum class MathAlgorithm {
    IntegerPaeth,
    TemporalDelta,
};

const char* algorithmName(MathAlgorithm algorithm) noexcept {
    return algorithm == MathAlgorithm::IntegerPaeth
        ? "integer-paeth" : "integer-temporal-delta";
}

struct MathPacket {
    bool transformed = false;
    std::vector<std::uint8_t> payload;

    [[nodiscard]] std::size_t representedBytes() const noexcept {
        return payload.size() + (transformed ? kMathematicalSideInfoBytes : 0U);
    }
};

[[noreturn]] void usageError(const std::string& message) {
    throw std::invalid_argument(message +
        "\nusage: frameforge_language_transform_benchmark "
        "[--width N] [--height N] [--repeats N]");
}

std::uint32_t parsePositive(std::string_view value, const char* option) {
    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0U) {
        usageError(std::string(option) + " must be a positive integer");
    }
    return parsed;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (key == "--help") {
            std::cout << "Deterministic integer-Paeth versus current frame-codec benchmark\n"
                         "Options: [--width N] [--height N] [--repeats N]\n";
            std::exit(0);
        }
        if (i + 1 >= argc) usageError("missing value for " + std::string(key));
        const std::string_view value(argv[++i]);
        if (key == "--width") options.width = parsePositive(value, "--width");
        else if (key == "--height") options.height = parsePositive(value, "--height");
        else if (key == "--repeats") options.repeats = parsePositive(value, "--repeats");
        else usageError("unknown option " + std::string(key));
    }
    if (static_cast<std::size_t>(options.width) >
        std::numeric_limits<std::size_t>::max() / 4U / options.height) {
        usageError("requested frame size overflows addressable memory");
    }
    return options;
}

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

void storeLittleEndian(std::span<std::uint8_t> bytes, std::size_t offset,
                       std::uint8_t count, std::uint64_t value) {
    for (std::size_t i = 0U; i < count; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

std::vector<std::uint8_t> makePattern(const Options& options, BitDepth depth,
                                      std::string_view pattern, bool mutate) {
    const std::size_t sampleCount = static_cast<std::size_t>(options.width) * options.height;
    const std::size_t bytes = sampleCount * depth.storageBytes;
    std::vector<std::uint8_t> frame(bytes);
    const std::uint64_t maximum = depth.bits == 32U
        ? 0xffffffffULL : ((1ULL << depth.bits) - 1ULL);
    const std::uint64_t mask = maximum;
    const std::uint64_t xDenominator = std::max<std::uint64_t>(1U, options.width - 1U);
    const std::uint64_t yDenominator = std::max<std::uint64_t>(1U, options.height - 1U);
    const long double gradientDenominator = static_cast<long double>(xDenominator) +
                                            static_cast<long double>(yDenominator);

    for (std::size_t i = 0U; i < sampleCount; ++i) {
        const std::uint64_t x = i % options.width;
        const std::uint64_t y = i / options.width;
        std::uint64_t value = 0U;
        if (pattern == "flat") {
            value = maximum / 3U;
        } else if (pattern == "gradient") {
            const long double fraction =
                (static_cast<long double>(x) + static_cast<long double>(y)) /
                gradientDenominator;
            value = static_cast<std::uint64_t>(fraction * static_cast<long double>(maximum));
        } else if (pattern == "edges") {
            const bool high = ((x / 24U) + (y / 16U)) % 2U != 0U;
            value = high ? (maximum * 4U / 5U) : (maximum / 5U);
        } else {
            const std::uint64_t hash = splitmix64(i + 0x7f4a7c15ULL);
            if (pattern == "random") {
                value = hash & mask;
            } else {  // deterministic low-amplitude texture over a smooth field
                const long double fraction =
                    (static_cast<long double>(x) + static_cast<long double>(y)) /
                    gradientDenominator;
                const std::uint64_t base = static_cast<std::uint64_t>(
                    fraction * static_cast<long double>(maximum));
                const std::uint64_t noiseSpan = std::min<std::uint64_t>(maximum, 63U);
                const std::int64_t noise = static_cast<std::int64_t>(hash % (noiseSpan + 1U)) -
                                           static_cast<std::int64_t>(noiseSpan / 2U);
                const std::int64_t textured = static_cast<std::int64_t>(base) + noise;
                value = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
                    textured, 0, static_cast<std::int64_t>(maximum)));
            }
        }
        if (mutate && i % 997U == 0U) value = (value + 1U) & mask;
        storeLittleEndian(frame, i * depth.storageBytes, depth.storageBytes, value);
    }
    return frame;
}

std::vector<std::uint8_t> compressZstandard(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> output(ZSTD_compressBound(input.size()));
    const std::size_t result = ZSTD_compress(
        output.data(), output.size(), input.data(), input.size(), 1);
    if (ZSTD_isError(result) != 0U) {
        throw std::runtime_error(std::string("Zstandard compression failed: ") +
                                 ZSTD_getErrorName(result));
    }
    output.resize(result);
    return output;
}

std::vector<std::uint8_t> decompressZstandard(std::span<const std::uint8_t> input,
                                               std::size_t expectedSize) {
    std::vector<std::uint8_t> output(expectedSize);
    const std::size_t result = ZSTD_decompress(
        output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(result) != 0U || result != expectedSize) {
        throw std::runtime_error("Zstandard mathematical residual decode failed");
    }
    return output;
}

MathPacket encodeMathematical(std::span<const std::uint8_t> frame,
                              const frameforge::FrameLayout& layout,
                              frameforge::IntegerPaethParameters parameters,
                              std::span<const std::uint8_t> previous,
                              MathAlgorithm algorithm) {
    const auto residual = algorithm == MathAlgorithm::IntegerPaeth
        ? frameforge::encodeIntegerPaethResidual(frame, layout, parameters, previous)
        : frameforge::encodeIntegerTemporalDeltaResidual(
              frame, layout, parameters, previous);
    auto compressed = compressZstandard(residual);
    if (compressed.size() + kMathematicalSideInfoBytes < frame.size()) {
        return {true, std::move(compressed)};
    }
    return {false, {frame.begin(), frame.end()}};
}

std::vector<std::uint8_t> decodeMathematical(
    const MathPacket& packet, std::size_t frameBytes,
    const frameforge::FrameLayout& layout,
    frameforge::IntegerPaethParameters parameters,
    std::span<const std::uint8_t> previous,
    MathAlgorithm algorithm) {
    if (!packet.transformed) return packet.payload;
    const auto residual = decompressZstandard(packet.payload, frameBytes);
    return algorithm == MathAlgorithm::IntegerPaeth
        ? frameforge::decodeIntegerPaethResidual(residual, layout, parameters, previous)
        : frameforge::decodeIntegerTemporalDeltaResidual(
              residual, layout, parameters, previous);
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    if (values.size() % 2U != 0U) return values[middle];
    return (values[middle - 1U] + values[middle]) / 2.0;
}

struct Result {
    std::size_t baselineBytes = 0U;
    std::size_t mathematicalBytes = 0U;
    std::size_t bestBytes = 0U;
    bool mathematicalSelected = false;
    double baselineEncodeMs = 0.0;
    double mathematicalEncodeMs = 0.0;
    double bestEncodeMs = 0.0;
    double baselineDecodeMs = 0.0;
    double mathematicalDecodeMs = 0.0;
    double bestDecodeMs = 0.0;
};

Result measureCase(const Options& options, BitDepth depth, std::string_view pattern,
                   bool temporal, MathAlgorithm algorithm) {
    const std::uint32_t channels = depth.storageBytes;
    const frameforge::FrameLayout layout{options.width, options.height, channels, {}};
    const std::vector<std::uint8_t> previousFrame = temporal
        ? makePattern(options, depth, pattern, false) : std::vector<std::uint8_t>{};
    const std::vector<std::uint8_t> frame = temporal
        ? makePattern(options, depth, pattern, true)
        : makePattern(options, depth, pattern, false);
    const frameforge::IntegerPaethParameters parameters{
        depth.storageBytes, frameforge::SampleByteOrder::LittleEndian};
    const std::span<const std::uint8_t> previous(previousFrame);
    std::vector<double> baselineEncodeTimes;
    std::vector<double> mathematicalEncodeTimes;
    std::vector<double> baselineDecodeTimes;
    std::vector<double> mathematicalDecodeTimes;
    frameforge::EncodedFrame baselinePacket;
    MathPacket mathPacket;

    for (std::uint32_t repeat = 0U; repeat < options.repeats; ++repeat) {
        auto start = Clock::now();
        baselinePacket = frameforge::encodeLosslessFrame(
            frame, layout, previous, 1, frameforge::CompressionCodec::Zstandard);
        baselineEncodeTimes.push_back(std::chrono::duration<double, std::milli>(
            Clock::now() - start).count());

        start = Clock::now();
        mathPacket = encodeMathematical(frame, layout, parameters, previous, algorithm);
        mathematicalEncodeTimes.push_back(std::chrono::duration<double, std::milli>(
            Clock::now() - start).count());

        start = Clock::now();
        const auto baselineDecoded = frameforge::decodeLosslessFrame(baselinePacket, previous);
        baselineDecodeTimes.push_back(std::chrono::duration<double, std::milli>(
            Clock::now() - start).count());
        if (baselineDecoded != frame) {
            throw std::runtime_error("existing codec benchmark output was not byte-exact");
        }

        start = Clock::now();
        const auto mathDecoded = decodeMathematical(
            mathPacket, frame.size(), layout, parameters, previous, algorithm);
        mathematicalDecodeTimes.push_back(std::chrono::duration<double, std::milli>(
            Clock::now() - start).count());
        if (mathDecoded != frame) {
            throw std::runtime_error("mathematical benchmark output was not byte-exact");
        }
    }

    const double baselineEncodeMs = median(std::move(baselineEncodeTimes));
    const double mathematicalEncodeMs = median(std::move(mathematicalEncodeTimes));
    const double baselineDecodeMs = median(std::move(baselineDecodeTimes));
    const double mathematicalDecodeMs = median(std::move(mathematicalDecodeTimes));
    const std::size_t baselineBytes = baselinePacket.payload.size();
    const std::size_t mathematicalBytes = mathPacket.representedBytes();
    const bool mathematicalSelected = mathematicalBytes < baselineBytes;
    return {baselineBytes,
            mathematicalBytes,
            mathematicalSelected ? mathematicalBytes : baselineBytes,
            mathematicalSelected,
            baselineEncodeMs,
            mathematicalEncodeMs,
            baselineEncodeMs + mathematicalEncodeMs,
            baselineDecodeMs,
            mathematicalDecodeMs,
            mathematicalSelected ? mathematicalDecodeMs : baselineDecodeMs};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        constexpr BitDepth depths[]{{8U, 1U}, {10U, 2U}, {12U, 2U},
                                    {16U, 2U}, {24U, 3U}, {32U, 4U}};
        constexpr std::string_view patterns[]{"flat", "gradient", "edges", "texture", "random"};
        std::cout << "mode,algorithm,bits,pattern,width,height,raw_bytes,legacy_bytes,math_candidate_bytes,"
                     "best_bytes,math_selected,candidate_delta_percent,best_delta_percent,"
                     "legacy_encode_ms,math_encode_ms,best_encode_ms,"
                     "legacy_decode_ms,math_decode_ms,best_decode_ms\n";
        std::cout << std::fixed << std::setprecision(3);
        for (const BitDepth depth : depths) {
            for (const std::string_view pattern : patterns) {
                for (const bool temporal : {false, true}) {
                    for (const MathAlgorithm algorithm : {
                             MathAlgorithm::IntegerPaeth, MathAlgorithm::TemporalDelta}) {
                        if (!temporal && algorithm == MathAlgorithm::TemporalDelta) continue;
                        const Result result = measureCase(
                            options, depth, pattern, temporal, algorithm);
                        const std::size_t rawBytes = static_cast<std::size_t>(options.width) *
                            options.height * depth.storageBytes;
                        const double candidateDelta = 100.0 *
                            (static_cast<double>(result.mathematicalBytes) -
                             static_cast<double>(result.baselineBytes)) /
                            static_cast<double>(std::max<std::size_t>(1U, result.baselineBytes));
                        const double bestDelta = 100.0 *
                            (static_cast<double>(result.bestBytes) -
                             static_cast<double>(result.baselineBytes)) /
                            static_cast<double>(std::max<std::size_t>(1U, result.baselineBytes));
                        std::cout << (temporal ? "temporal" : "spatial") << ','
                                  << algorithmName(algorithm) << ',' << depth.bits << ','
                                  << pattern << ',' << options.width << ',' << options.height << ','
                                  << rawBytes << ',' << result.baselineBytes << ','
                                  << result.mathematicalBytes << ',' << result.bestBytes << ','
                                  << (result.mathematicalSelected ? 1 : 0) << ','
                                  << candidateDelta << ',' << bestDelta << ','
                                  << result.baselineEncodeMs << ',' << result.mathematicalEncodeMs << ','
                                  << result.bestEncodeMs << ',' << result.baselineDecodeMs << ','
                                  << result.mathematicalDecodeMs << ',' << result.bestDecodeMs << '\n';
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "language transform benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
