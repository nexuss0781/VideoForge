#include "frameforge/frame_codec.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

#include <zlib.h>
#include <zstd.h>

namespace frameforge {
namespace {

std::size_t checkedFrameSize(FrameLayout layout) {
    constexpr std::size_t maxSize = std::numeric_limits<std::size_t>::max();
    if (layout.width == 0U || layout.height == 0U) {
        throw std::invalid_argument("frame width and height must be positive");
    }
    if (layout.planes.empty()) {
        if (layout.channels == 0U) {
            throw std::invalid_argument("interleaved frame channel count must be positive");
        }
        const auto width = static_cast<std::size_t>(layout.width);
        const auto height = static_cast<std::size_t>(layout.height);
        const auto channels = static_cast<std::size_t>(layout.channels);
        if (width > maxSize / channels) {
            throw std::length_error("frame row byte count overflows addressable memory");
        }
        const std::size_t rowBytes = width * channels;
        if (rowBytes > maxSize / height) {
            throw std::length_error("frame byte count overflows addressable memory");
        }
        return rowBytes * height;
    }

    std::size_t totalBytes = 0U;
    for (const FramePlaneLayout& plane : layout.planes) {
        if (plane.rowBytes == 0U || plane.rows == 0U || plane.pixelBytes == 0U ||
            plane.pixelBytes > plane.rowBytes) {
            throw std::invalid_argument("frame plane has invalid row or pixel geometry");
        }
        const auto rowBytes = static_cast<std::size_t>(plane.rowBytes);
        const auto rows = static_cast<std::size_t>(plane.rows);
        if (rowBytes > maxSize / rows) {
            throw std::length_error("frame plane byte count overflows addressable memory");
        }
        const std::size_t planeBytes = rowBytes * rows;
        if (planeBytes > maxSize - totalBytes) {
            throw std::length_error("total frame plane byte count overflows addressable memory");
        }
        totalBytes += planeBytes;
    }
    return totalBytes;
}

std::uint8_t paethPredictor(std::uint8_t left, std::uint8_t above,
                            std::uint8_t upperLeft) noexcept {
    const int a = static_cast<int>(left);
    const int b = static_cast<int>(above);
    const int c = static_cast<int>(upperLeft);
    const int estimate = a + b - c;
    const int distanceLeft = std::abs(estimate - a);
    const int distanceAbove = std::abs(estimate - b);
    const int distanceUpperLeft = std::abs(estimate - c);
    if (distanceLeft <= distanceAbove && distanceLeft <= distanceUpperLeft) {
        return left;
    }
    if (distanceAbove <= distanceUpperLeft) {
        return above;
    }
    return upperLeft;
}

std::vector<std::uint8_t> applySpatialPredictor(
    std::span<const std::uint8_t> source, const FrameLayout& layout) {
    std::vector<std::uint8_t> transformed(source.size());
    const auto processPlane = [&](std::size_t planeOffset, std::size_t rowBytes,
                                  std::size_t rows, std::size_t pixelBytes) {
        for (std::size_t row = 0; row < rows; ++row) {
            const std::size_t rowOffset = planeOffset + row * rowBytes;
            for (std::size_t columnByte = 0; columnByte < rowBytes; ++columnByte) {
                const std::size_t index = rowOffset + columnByte;
                const std::uint8_t left = columnByte >= pixelBytes
                                              ? source[index - pixelBytes]
                                              : 0U;
                const std::uint8_t above = row != 0U ? source[index - rowBytes] : 0U;
                const std::uint8_t upperLeft = row != 0U && columnByte >= pixelBytes
                                                   ? source[index - rowBytes - pixelBytes]
                                                   : 0U;
                const std::uint8_t prediction = paethPredictor(left, above, upperLeft);
                transformed[index] = static_cast<std::uint8_t>(
                    static_cast<unsigned int>(source[index]) -
                    static_cast<unsigned int>(prediction));
            }
        }
    };
    if (layout.planes.empty()) {
        const std::size_t rowBytes = static_cast<std::size_t>(layout.width) * layout.channels;
        processPlane(0U, rowBytes, layout.height, layout.channels);
    } else {
        std::size_t offset = 0U;
        for (const FramePlaneLayout& plane : layout.planes) {
            processPlane(offset, plane.rowBytes, plane.rows, plane.pixelBytes);
            offset += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
        }
    }
    return transformed;
}

std::vector<std::uint8_t> undoSpatialPredictor(
    std::span<const std::uint8_t> residual, const FrameLayout& layout) {
    std::vector<std::uint8_t> restored(residual.size());
    const auto processPlane = [&](std::size_t planeOffset, std::size_t rowBytes,
                                  std::size_t rows, std::size_t pixelBytes) {
        for (std::size_t row = 0; row < rows; ++row) {
            const std::size_t rowOffset = planeOffset + row * rowBytes;
            for (std::size_t columnByte = 0; columnByte < rowBytes; ++columnByte) {
                const std::size_t index = rowOffset + columnByte;
                const std::uint8_t left = columnByte >= pixelBytes
                                              ? restored[index - pixelBytes]
                                              : 0U;
                const std::uint8_t above = row != 0U ? restored[index - rowBytes] : 0U;
                const std::uint8_t upperLeft = row != 0U && columnByte >= pixelBytes
                                                   ? restored[index - rowBytes - pixelBytes]
                                                   : 0U;
                const std::uint8_t prediction = paethPredictor(left, above, upperLeft);
                restored[index] = static_cast<std::uint8_t>(
                    static_cast<unsigned int>(residual[index]) +
                    static_cast<unsigned int>(prediction));
            }
        }
    };
    if (layout.planes.empty()) {
        const std::size_t rowBytes = static_cast<std::size_t>(layout.width) * layout.channels;
        processPlane(0U, rowBytes, layout.height, layout.channels);
    } else {
        std::size_t offset = 0U;
        for (const FramePlaneLayout& plane : layout.planes) {
            processPlane(offset, plane.rowBytes, plane.rows, plane.pixelBytes);
            offset += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
        }
    }
    return restored;
}

std::vector<std::uint8_t> applyTemporalDelta(
    std::span<const std::uint8_t> current,
    std::span<const std::uint8_t> previous) {
    std::vector<std::uint8_t> transformed(current.size());
    for (std::size_t i = 0; i < current.size(); ++i) {
        transformed[i] = static_cast<std::uint8_t>(
            static_cast<unsigned int>(current[i]) -
            static_cast<unsigned int>(previous[i]));
    }
    return transformed;
}

struct PredictorSampleHistogram {
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t sampleCount = 0U;

    void add(std::uint8_t value) noexcept {
        ++counts[value];
        ++sampleCount;
    }

    [[nodiscard]] double entropy() const noexcept {
        if (sampleCount == 0U) {
            return 0.0;
        }
        double weightedLogCount = 0.0;
        for (const std::uint64_t count : counts) {
            if (count != 0U) {
                const double value = static_cast<double>(count);
                weightedLogCount += value * std::log2(value);
            }
        }
        const double total = static_cast<double>(sampleCount);
        return std::log2(total) - weightedLogCount / total;
    }
};

std::uint8_t byteDifference(std::uint8_t left, std::uint8_t right) noexcept {
    return static_cast<std::uint8_t>(static_cast<unsigned int>(left) -
                                     static_cast<unsigned int>(right));
}

std::array<PredictorSampleHistogram, 3> samplePredictors(
    std::span<const std::uint8_t> frame,
    std::span<const std::uint8_t> previousFrame, const FrameLayout& layout) {
    constexpr std::size_t samplesPerRow = 64U;
    std::array<PredictorSampleHistogram, 3> scores{};

    const auto processPlane = [&](std::size_t planeOffset, std::size_t rowBytes,
                                  std::size_t rows, std::size_t pixelBytes) {
        const std::size_t rowSampleCount = std::min(samplesPerRow, rowBytes);
        for (std::size_t row = 0; row < rows; ++row) {
            const std::size_t rowOffset = planeOffset + row * rowBytes;
            for (std::size_t sample = 0; sample < rowSampleCount; ++sample) {
                const std::size_t column = sample * rowBytes / rowSampleCount;
                const std::size_t index = rowOffset + column;
                const bool hasLeft = column >= pixelBytes;
                const bool hasAbove = row != 0U;
                const std::uint8_t left = hasLeft ? frame[index - pixelBytes] : 0U;
                const std::uint8_t above = hasAbove ? frame[index - rowBytes] : 0U;
                const std::uint8_t upperLeft = hasLeft && hasAbove
                    ? frame[index - rowBytes - pixelBytes]
                    : 0U;
                scores[0].add(byteDifference(
                    frame[index], paethPredictor(left, above, upperLeft)));

                if (!previousFrame.empty()) {
                    const auto deltaAt = [&](std::size_t offset) {
                        return byteDifference(frame[offset], previousFrame[offset]);
                    };
                    const std::uint8_t delta = deltaAt(index);
                    scores[1].add(delta);
                    const std::uint8_t deltaLeft = hasLeft
                        ? deltaAt(index - pixelBytes)
                        : 0U;
                    const std::uint8_t deltaAbove = hasAbove
                        ? deltaAt(index - rowBytes)
                        : 0U;
                    const std::uint8_t deltaUpperLeft = hasLeft && hasAbove
                        ? deltaAt(index - rowBytes - pixelBytes)
                        : 0U;
                    scores[2].add(byteDifference(
                        delta, paethPredictor(deltaLeft, deltaAbove,
                                              deltaUpperLeft)));
                }
            }
        }
    };

    if (layout.planes.empty()) {
        const std::size_t rowBytes =
            static_cast<std::size_t>(layout.width) * layout.channels;
        processPlane(0U, rowBytes, layout.height, layout.channels);
    } else {
        std::size_t offset = 0U;
        for (const FramePlaneLayout& plane : layout.planes) {
            processPlane(offset, plane.rowBytes, plane.rows, plane.pixelBytes);
            offset += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
        }
    }
    return scores;
}

std::vector<std::uint8_t> applyTemporalPaeth(
    std::span<const std::uint8_t> current,
    std::span<const std::uint8_t> previous, const FrameLayout& layout) {
    std::vector<std::uint8_t> transformed(current.size());
    const auto processPlane = [&](std::size_t planeOffset, std::size_t rowBytes,
                                  std::size_t rows, std::size_t pixelBytes) {
        for (std::size_t row = 0; row < rows; ++row) {
            const std::size_t rowOffset = planeOffset + row * rowBytes;
            for (std::size_t column = 0; column < rowBytes; ++column) {
                const std::size_t index = rowOffset + column;
                const auto deltaAt = [&](std::size_t offset) {
                    return byteDifference(current[offset], previous[offset]);
                };
                const bool hasLeft = column >= pixelBytes;
                const bool hasAbove = row != 0U;
                const std::uint8_t left = hasLeft
                    ? deltaAt(index - pixelBytes)
                    : 0U;
                const std::uint8_t above = hasAbove
                    ? deltaAt(index - rowBytes)
                    : 0U;
                const std::uint8_t upperLeft = hasLeft && hasAbove
                    ? deltaAt(index - rowBytes - pixelBytes)
                    : 0U;
                transformed[index] = byteDifference(
                    deltaAt(index), paethPredictor(left, above, upperLeft));
            }
        }
    };

    if (layout.planes.empty()) {
        const std::size_t rowBytes =
            static_cast<std::size_t>(layout.width) * layout.channels;
        processPlane(0U, rowBytes, layout.height, layout.channels);
    } else {
        std::size_t offset = 0U;
        for (const FramePlaneLayout& plane : layout.planes) {
            processPlane(offset, plane.rowBytes, plane.rows, plane.pixelBytes);
            offset += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
        }
    }
    return transformed;
}

std::vector<std::uint8_t> undoTemporalDelta(
    std::span<const std::uint8_t> delta,
    std::span<const std::uint8_t> previous) {
    std::vector<std::uint8_t> restored(delta.size());
    for (std::size_t i = 0; i < delta.size(); ++i) {
        restored[i] = static_cast<std::uint8_t>(
            static_cast<unsigned int>(delta[i]) +
            static_cast<unsigned int>(previous[i]));
    }
    return restored;
}

std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> input,
                                  int compressionLevel) {
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max())) {
        throw std::length_error("frame is too large for the configured zlib ABI");
    }
    const uLong sourceLength = static_cast<uLong>(input.size());
    const uLongf bound = compressBound(sourceLength);
    if (bound > static_cast<uLongf>(std::numeric_limits<std::size_t>::max())) {
        throw std::length_error("compressed frame bound overflows addressable memory");
    }
    std::vector<std::uint8_t> output(static_cast<std::size_t>(bound));
    uLongf outputLength = bound;
    const Bytef* source = reinterpret_cast<const Bytef*>(input.data());
    const int result = compress2(output.data(), &outputLength, source, sourceLength,
                                 compressionLevel);
    if (result != Z_OK) {
        throw std::runtime_error("zlib frame compression failed with status " +
                                 std::to_string(result));
    }
    output.resize(static_cast<std::size_t>(outputLength));
    return output;
}

std::vector<std::uint8_t> inflate(std::span<const std::uint8_t> input,
                                  std::size_t expectedSize) {
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max()) ||
        expectedSize > static_cast<std::size_t>(std::numeric_limits<uLongf>::max())) {
        throw std::length_error("encoded frame is too large for the configured zlib ABI");
    }
    std::vector<std::uint8_t> output(expectedSize);
    uLongf outputLength = static_cast<uLongf>(expectedSize);
    const Bytef* source = reinterpret_cast<const Bytef*>(input.data());
    const int result = uncompress(output.data(), &outputLength, source,
                                  static_cast<uLong>(input.size()));
    if (result != Z_OK) {
        throw std::runtime_error("zlib frame decompression failed with status " +
                                 std::to_string(result));
    }
    if (outputLength != static_cast<uLongf>(expectedSize)) {
        throw std::runtime_error("decompressed frame length does not match its layout");
    }
    return output;
}

std::vector<std::uint8_t> zstandardCompress(std::span<const std::uint8_t> input,
                                            int compressionLevel) {
    const std::size_t bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound) != 0U) {
        throw std::length_error(std::string("Zstandard frame bound failed: ") +
                                ZSTD_getErrorName(bound));
    }
    std::vector<std::uint8_t> output(bound);
    const int zstdLevel = compressionLevel == 0 ? 1 : compressionLevel;
    const std::size_t result = ZSTD_compress(
        output.data(), output.size(), input.data(), input.size(), zstdLevel);
    if (ZSTD_isError(result) != 0U) {
        throw std::runtime_error(std::string("Zstandard frame compression failed: ") +
                                 ZSTD_getErrorName(result));
    }
    output.resize(result);
    return output;
}

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input,
                                  int compressionLevel,
                                  CompressionCodec codec) {
    switch (codec) {
        case CompressionCodec::Deflate:
            return deflate(input, compressionLevel);
        case CompressionCodec::Zstandard:
            return zstandardCompress(input, compressionLevel);
        case CompressionCodec::Raw:
            break;
    }
    throw std::invalid_argument("raw frame coding is selected automatically, not as a compressor");
}

std::vector<std::uint8_t> zstandardDecompress(
    std::span<const std::uint8_t> input, std::size_t expectedSize) {
    std::vector<std::uint8_t> output(expectedSize);
    const std::size_t result = ZSTD_decompress(
        output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(result) != 0U) {
        throw std::runtime_error(std::string("Zstandard frame decompression failed: ") +
                                 ZSTD_getErrorName(result));
    }
    if (result != expectedSize) {
        throw std::runtime_error("decompressed frame length does not match its layout");
    }
    return output;
}

struct Candidate {
    PredictionMode mode = PredictionMode::None;
    CompressionCodec codec = CompressionCodec::Raw;
    std::vector<std::uint8_t> bytes;
};

}  // namespace

EncodedFrame encodeLosslessFrame(std::span<const std::uint8_t> frame,
                                 FrameLayout layout,
                                 std::span<const std::uint8_t> previousFrame,
                                 int compressionLevel,
                                 CompressionCodec codec) {
    if (compressionLevel < 0 || compressionLevel > 9) {
        throw std::invalid_argument("compression level must be between 0 and 9");
    }
    if (codec != CompressionCodec::Deflate && codec != CompressionCodec::Zstandard) {
        throw std::invalid_argument("frame encoder requires DEFLATE or Zstandard");
    }
    const std::size_t expectedSize = checkedFrameSize(layout);
    if (frame.size() != expectedSize) {
        throw std::invalid_argument("input frame byte count does not match its layout");
    }
    if (!previousFrame.empty() && previousFrame.size() != expectedSize) {
        throw std::invalid_argument("previous frame byte count does not match the layout");
    }
    Candidate best;
    best.bytes.assign(frame.begin(), frame.end());

    std::vector<std::uint8_t> compressedRaw = compress(frame, compressionLevel, codec);
    if (compressedRaw.size() < best.bytes.size()) {
        best.codec = codec;
        best.bytes = std::move(compressedRaw);
    }

    const std::array<PredictorSampleHistogram, 3> scores =
        samplePredictors(frame, previousFrame, layout);
    std::size_t selectedPredictor = 0U;
    double selectedEntropy = scores[0].entropy();
    for (std::size_t candidate = 1U; candidate < scores.size(); ++candidate) {
        if (scores[candidate].sampleCount != 0U &&
            scores[candidate].entropy() < selectedEntropy) {
            selectedPredictor = candidate;
            selectedEntropy = scores[candidate].entropy();
        }
    }

    PredictionMode selectedMode = PredictionMode::Paeth;
    std::vector<std::uint8_t> predictorBytes;
    if (selectedPredictor == 1U) {
        selectedMode = PredictionMode::TemporalDelta;
        predictorBytes = applyTemporalDelta(frame, previousFrame);
    } else if (selectedPredictor == 2U) {
        selectedMode = PredictionMode::TemporalPaeth;
        predictorBytes = applyTemporalPaeth(frame, previousFrame, layout);
    } else {
        predictorBytes = applySpatialPredictor(frame, layout);
    }
    std::vector<std::uint8_t> compressedPredictor =
        compress(predictorBytes, compressionLevel, codec);
    if (compressedPredictor.size() < best.bytes.size()) {
        best.mode = selectedMode;
        best.codec = codec;
        best.bytes = std::move(compressedPredictor);
    }

    return EncodedFrame{layout, best.mode, best.codec,
                        static_cast<std::uint64_t>(expectedSize),
                        std::move(best.bytes)};
}

std::vector<std::uint8_t> decodeLosslessFrame(
    const EncodedFrame& encoded,
    std::span<const std::uint8_t> previousFrame) {
    const std::size_t expectedSize = checkedFrameSize(encoded.layout);
    if (encoded.decodedSize != static_cast<std::uint64_t>(expectedSize)) {
        throw std::invalid_argument("encoded frame decoded-size field does not match its layout");
    }
    if (!previousFrame.empty() && previousFrame.size() != expectedSize) {
        throw std::invalid_argument("previous frame byte count does not match the encoded layout");
    }
    const bool temporal = encoded.prediction == PredictionMode::TemporalDelta ||
                          encoded.prediction == PredictionMode::TemporalPaeth;
    if (encoded.prediction != PredictionMode::None &&
        encoded.prediction != PredictionMode::Paeth && !temporal) {
        throw std::invalid_argument("encoded frame uses an unknown prediction mode");
    }
    if (temporal && previousFrame.size() != expectedSize) {
        throw std::invalid_argument("temporal frame prediction requires a matching previous frame");
    }

    std::vector<std::uint8_t> residual;
    switch (encoded.codec) {
        case CompressionCodec::Raw:
            residual = encoded.payload;
            break;
        case CompressionCodec::Deflate:
            residual = inflate(encoded.payload, expectedSize);
            break;
        case CompressionCodec::Zstandard:
            residual = zstandardDecompress(encoded.payload, expectedSize);
            break;
        default:
            throw std::invalid_argument("encoded frame uses an unknown compression codec");
    }
    if (residual.size() != expectedSize) {
        throw std::invalid_argument("encoded frame payload length does not match its layout");
    }

    switch (encoded.prediction) {
        case PredictionMode::None:
            return residual;
        case PredictionMode::Paeth:
            return undoSpatialPredictor(residual, encoded.layout);
        case PredictionMode::TemporalDelta:
            return undoTemporalDelta(residual, previousFrame);
        case PredictionMode::TemporalPaeth: {
            std::vector<std::uint8_t> delta =
                undoSpatialPredictor(residual, encoded.layout);
            return undoTemporalDelta(delta, previousFrame);
        }
    }
    throw std::invalid_argument("encoded frame uses an unknown prediction mode");
}

const char* predictionModeName(PredictionMode mode) noexcept {
    switch (mode) {
        case PredictionMode::None: return "none";
        case PredictionMode::Paeth: return "paeth";
        case PredictionMode::TemporalDelta: return "temporal-delta";
        case PredictionMode::TemporalPaeth: return "temporal-paeth";
    }
    return "unknown";
}

}  // namespace frameforge
