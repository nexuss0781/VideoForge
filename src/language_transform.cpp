#include "frameforge/language_transform.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace frameforge {
namespace {

struct PlaneGeometry {
    std::size_t offset;
    std::size_t rowBytes;
    std::size_t rows;
    std::size_t pixelBytes;
};

std::size_t validateGeometry(const FrameLayout& layout,
                             IntegerPaethParameters parameters,
                             std::size_t inputSize,
                             std::size_t previousSize) {
    if (layout.width == 0U || layout.height == 0U) {
        throw std::invalid_argument("language transform frame dimensions must be positive");
    }
    if (parameters.componentBytes == 0U || parameters.componentBytes > 4U) {
        throw std::invalid_argument("integer Paeth component width must be in [1, 4] bytes");
    }
    if (parameters.byteOrder != SampleByteOrder::LittleEndian &&
        parameters.byteOrder != SampleByteOrder::BigEndian) {
        throw std::invalid_argument("integer Paeth uses an unknown sample byte order");
    }

    const std::size_t componentBytes = parameters.componentBytes;
    const std::size_t maxSize = std::numeric_limits<std::size_t>::max();
    std::size_t totalBytes = 0U;
    if (layout.planes.empty()) {
        if (layout.channels == 0U || layout.channels % componentBytes != 0U) {
            throw std::invalid_argument(
                "interleaved frame channels must be a positive multiple of component width");
        }
        const std::size_t width = layout.width;
        const std::size_t height = layout.height;
        const std::size_t pixelBytes = layout.channels;
        if (width > maxSize / pixelBytes) {
            throw std::length_error("language transform row size overflows addressable memory");
        }
        const std::size_t rowBytes = width * pixelBytes;
        if (rowBytes > maxSize / height) {
            throw std::length_error("language transform frame size overflows addressable memory");
        }
        totalBytes = rowBytes * height;
    } else {
        for (const FramePlaneLayout& plane : layout.planes) {
            const std::size_t rowBytes = plane.rowBytes;
            const std::size_t rows = plane.rows;
            const std::size_t pixelBytes = plane.pixelBytes;
            if (rowBytes == 0U || rows == 0U || pixelBytes == 0U ||
                pixelBytes > rowBytes || pixelBytes % componentBytes != 0U ||
                rowBytes % pixelBytes != 0U) {
                throw std::invalid_argument(
                    "language transform plane geometry is incompatible with component width");
            }
            if (rowBytes > maxSize / rows) {
                throw std::length_error("language transform plane size overflows addressable memory");
            }
            const std::size_t planeBytes = rowBytes * rows;
            if (planeBytes > maxSize - totalBytes) {
                throw std::length_error("language transform frame size overflows addressable memory");
            }
            totalBytes += planeBytes;
        }
    }

    if (inputSize != totalBytes) {
        throw std::invalid_argument("language transform byte count does not match frame layout");
    }
    if (previousSize != 0U && previousSize != totalBytes) {
        throw std::invalid_argument("previous frame byte count does not match language transform layout");
    }
    return totalBytes;
}

template <typename Visitor>
void visitPlanes(const FrameLayout& layout, Visitor&& visitor) {
    if (layout.planes.empty()) {
        const std::size_t rowBytes = static_cast<std::size_t>(layout.width) * layout.channels;
        visitor(PlaneGeometry{0U, rowBytes, layout.height, layout.channels});
        return;
    }
    std::size_t offset = 0U;
    for (const FramePlaneLayout& plane : layout.planes) {
        visitor(PlaneGeometry{offset, plane.rowBytes, plane.rows, plane.pixelBytes});
        offset += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
    }
}

std::uint64_t sampleMask(std::size_t componentBytes) noexcept {
    if (componentBytes == 4U) {
        return 0xffffffffULL;
    }
    return (1ULL << (componentBytes * 8U)) - 1ULL;
}

std::uint64_t loadSample(std::span<const std::uint8_t> bytes,
                         std::size_t offset,
                         std::size_t componentBytes,
                         SampleByteOrder byteOrder) noexcept {
    std::uint64_t value = 0U;
    if (byteOrder == SampleByteOrder::LittleEndian) {
        for (std::size_t i = 0U; i < componentBytes; ++i) {
            value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
        }
    } else {
        for (std::size_t i = 0U; i < componentBytes; ++i) {
            value = (value << 8U) | bytes[offset + i];
        }
    }
    return value;
}

void storeSample(std::span<std::uint8_t> bytes,
                 std::size_t offset,
                 std::size_t componentBytes,
                 SampleByteOrder byteOrder,
                 std::uint64_t value) noexcept {
    if (byteOrder == SampleByteOrder::LittleEndian) {
        for (std::size_t i = 0U; i < componentBytes; ++i) {
            bytes[offset + i] = static_cast<std::uint8_t>(value & 0xffU);
            value >>= 8U;
        }
    } else {
        for (std::size_t i = componentBytes; i > 0U; --i) {
            bytes[offset + i - 1U] = static_cast<std::uint8_t>(value & 0xffU);
            value >>= 8U;
        }
    }
}

std::uint64_t magnitude(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value < 0 ? -value : value);
}

std::uint64_t paethPredictor(std::uint64_t left,
                             std::uint64_t above,
                             std::uint64_t upperLeft) noexcept {
    // Supported samples are at most 32 bits, keeping the estimate and
    // distances safely inside signed 64-bit arithmetic.
    const std::int64_t a = static_cast<std::int64_t>(left);
    const std::int64_t b = static_cast<std::int64_t>(above);
    const std::int64_t c = static_cast<std::int64_t>(upperLeft);
    const std::int64_t estimate = a + b - c;
    const std::uint64_t distanceLeft = magnitude(estimate - a);
    const std::uint64_t distanceAbove = magnitude(estimate - b);
    const std::uint64_t distanceUpperLeft = magnitude(estimate - c);
    if (distanceLeft <= distanceAbove && distanceLeft <= distanceUpperLeft) {
        return left;
    }
    if (distanceAbove <= distanceUpperLeft) {
        return above;
    }
    return upperLeft;
}

std::uint8_t paethPredictorByte(std::uint8_t left, std::uint8_t above,
                                std::uint8_t upperLeft) noexcept {
    return static_cast<std::uint8_t>(paethPredictor(left, above, upperLeft));
}

std::uint64_t modularDifference(std::uint64_t value,
                                std::uint64_t reference,
                                std::uint64_t mask) noexcept {
    return (value - reference) & mask;
}

}  // namespace

std::vector<std::uint8_t> encodeIntegerPaethResidual(
    std::span<const std::uint8_t> frame,
    const FrameLayout& layout,
    IntegerPaethParameters parameters,
    std::span<const std::uint8_t> previousFrame) {
    (void)validateGeometry(layout, parameters, frame.size(), previousFrame.size());
    std::vector<std::uint8_t> residual(frame.size());
    const bool temporal = !previousFrame.empty();
    std::vector<std::uint8_t> temporalDeltas;
    std::span<const std::uint8_t> predictionInput = frame;
    if (temporal) {
        temporalDeltas.resize(frame.size());
        const std::size_t componentBytes = parameters.componentBytes;
        const std::uint64_t mask = sampleMask(componentBytes);
        for (std::size_t offset = 0U; offset < frame.size(); offset += componentBytes) {
            const std::uint64_t current = loadSample(
                frame, offset, componentBytes, parameters.byteOrder);
            const std::uint64_t previous = loadSample(
                previousFrame, offset, componentBytes, parameters.byteOrder);
            storeSample(temporalDeltas, offset, componentBytes, parameters.byteOrder,
                        modularDifference(current, previous, mask));
        }
        predictionInput = temporalDeltas;
    }

    if (parameters.componentBytes == 1U) {
        const auto deltaAt = [&](std::size_t offset) noexcept { return predictionInput[offset]; };
        visitPlanes(layout, [&](const PlaneGeometry& plane) {
            for (std::size_t row = 0U; row < plane.rows; ++row) {
                const std::size_t rowOffset = plane.offset + row * plane.rowBytes;
                for (std::size_t column = 0U; column < plane.rowBytes; ++column) {
                    const std::size_t index = rowOffset + column;
                    const bool hasLeft = column >= plane.pixelBytes;
                    const bool hasAbove = row != 0U;
                    const std::uint8_t left = hasLeft ? deltaAt(index - plane.pixelBytes) : 0U;
                    const std::uint8_t above = hasAbove ? deltaAt(index - plane.rowBytes) : 0U;
                    const std::uint8_t upperLeft = hasLeft && hasAbove
                        ? deltaAt(index - plane.rowBytes - plane.pixelBytes)
                        : 0U;
                    residual[index] = static_cast<std::uint8_t>(
                        deltaAt(index) - paethPredictorByte(left, above, upperLeft));
                }
            }
        });
        return residual;
    }

    const std::size_t componentBytes = parameters.componentBytes;
    const std::uint64_t mask = sampleMask(componentBytes);
    const auto deltaAt = [&](std::size_t offset) noexcept {
        return loadSample(predictionInput, offset, componentBytes, parameters.byteOrder);
    };

    visitPlanes(layout, [&](const PlaneGeometry& plane) {
        for (std::size_t row = 0U; row < plane.rows; ++row) {
            const std::size_t rowOffset = plane.offset + row * plane.rowBytes;
            for (std::size_t column = 0U; column < plane.rowBytes;
                 column += componentBytes) {
                const std::size_t index = rowOffset + column;
                const bool hasLeft = column >= plane.pixelBytes;
                const bool hasAbove = row != 0U;
                const std::uint64_t left = hasLeft ? deltaAt(index - plane.pixelBytes) : 0U;
                const std::uint64_t above = hasAbove ? deltaAt(index - plane.rowBytes) : 0U;
                const std::uint64_t upperLeft = hasLeft && hasAbove
                    ? deltaAt(index - plane.rowBytes - plane.pixelBytes)
                    : 0U;
                const std::uint64_t predicted = paethPredictor(left, above, upperLeft);
                const std::uint64_t value = modularDifference(deltaAt(index), predicted, mask);
                storeSample(residual, index, componentBytes, parameters.byteOrder, value);
            }
        }
    });
    return residual;
}

std::vector<std::uint8_t> decodeIntegerPaethResidual(
    std::span<const std::uint8_t> residual,
    const FrameLayout& layout,
    IntegerPaethParameters parameters,
    std::span<const std::uint8_t> previousFrame) {
    (void)validateGeometry(layout, parameters, residual.size(), previousFrame.size());
    std::vector<std::uint8_t> restored(residual.size());
    const bool temporal = !previousFrame.empty();

    if (parameters.componentBytes == 1U) {
        const auto restoredDeltaAt = [&](std::size_t offset) noexcept { return restored[offset]; };
        visitPlanes(layout, [&](const PlaneGeometry& plane) {
            for (std::size_t row = 0U; row < plane.rows; ++row) {
                const std::size_t rowOffset = plane.offset + row * plane.rowBytes;
                for (std::size_t column = 0U; column < plane.rowBytes; ++column) {
                    const std::size_t index = rowOffset + column;
                    const bool hasLeft = column >= plane.pixelBytes;
                    const bool hasAbove = row != 0U;
                    const std::uint8_t left = hasLeft
                        ? restoredDeltaAt(index - plane.pixelBytes) : 0U;
                    const std::uint8_t above = hasAbove
                        ? restoredDeltaAt(index - plane.rowBytes) : 0U;
                    const std::uint8_t upperLeft = hasLeft && hasAbove
                        ? restoredDeltaAt(index - plane.rowBytes - plane.pixelBytes)
                        : 0U;
                    const std::uint8_t delta = static_cast<std::uint8_t>(
                        residual[index] + paethPredictorByte(left, above, upperLeft));
                    restored[index] = delta;
                }
            }
        });
        if (temporal) {
            for (std::size_t i = 0U; i < restored.size(); ++i) {
                restored[i] = static_cast<std::uint8_t>(restored[i] + previousFrame[i]);
            }
        }
        return restored;
    }

    const std::size_t componentBytes = parameters.componentBytes;
    const std::uint64_t mask = sampleMask(componentBytes);
    const auto restoredDeltaAt = [&](std::size_t offset) noexcept {
        return loadSample(restored, offset, componentBytes, parameters.byteOrder);
    };

    visitPlanes(layout, [&](const PlaneGeometry& plane) {
        for (std::size_t row = 0U; row < plane.rows; ++row) {
            const std::size_t rowOffset = plane.offset + row * plane.rowBytes;
            for (std::size_t column = 0U; column < plane.rowBytes;
                 column += componentBytes) {
                const std::size_t index = rowOffset + column;
                const bool hasLeft = column >= plane.pixelBytes;
                const bool hasAbove = row != 0U;
                const std::uint64_t left = hasLeft
                    ? restoredDeltaAt(index - plane.pixelBytes) : 0U;
                const std::uint64_t above = hasAbove
                    ? restoredDeltaAt(index - plane.rowBytes) : 0U;
                const std::uint64_t upperLeft = hasLeft && hasAbove
                    ? restoredDeltaAt(index - plane.rowBytes - plane.pixelBytes)
                    : 0U;
                const std::uint64_t predicted = paethPredictor(left, above, upperLeft);
                const std::uint64_t encodedValue = loadSample(
                    residual, index, componentBytes, parameters.byteOrder);
                const std::uint64_t delta = (encodedValue + predicted) & mask;
                storeSample(restored, index, componentBytes, parameters.byteOrder, delta);
            }
        }
    });
    if (temporal) {
        for (std::size_t offset = 0U; offset < restored.size(); offset += componentBytes) {
            const std::uint64_t delta = loadSample(
                restored, offset, componentBytes, parameters.byteOrder);
            const std::uint64_t previous = loadSample(
                previousFrame, offset, componentBytes, parameters.byteOrder);
            storeSample(restored, offset, componentBytes, parameters.byteOrder,
                        (delta + previous) & mask);
        }
    }
    return restored;
}

std::vector<std::uint8_t> encodeIntegerTemporalDeltaResidual(
    std::span<const std::uint8_t> frame,
    const FrameLayout& layout,
    IntegerPaethParameters parameters,
    std::span<const std::uint8_t> previousFrame) {
    (void)validateGeometry(layout, parameters, frame.size(), previousFrame.size());
    if (previousFrame.empty()) {
        throw std::invalid_argument("integer temporal delta requires a previous frame");
    }
    std::vector<std::uint8_t> residual(frame.size());
    const std::size_t componentBytes = parameters.componentBytes;
    if (componentBytes == 1U) {
        for (std::size_t i = 0U; i < frame.size(); ++i) {
            residual[i] = static_cast<std::uint8_t>(frame[i] - previousFrame[i]);
        }
        return residual;
    }
    const std::uint64_t mask = sampleMask(componentBytes);
    for (std::size_t offset = 0U; offset < frame.size(); offset += componentBytes) {
        const std::uint64_t current = loadSample(
            frame, offset, componentBytes, parameters.byteOrder);
        const std::uint64_t previous = loadSample(
            previousFrame, offset, componentBytes, parameters.byteOrder);
        storeSample(residual, offset, componentBytes, parameters.byteOrder,
                    modularDifference(current, previous, mask));
    }
    return residual;
}

std::vector<std::uint8_t> decodeIntegerTemporalDeltaResidual(
    std::span<const std::uint8_t> residual,
    const FrameLayout& layout,
    IntegerPaethParameters parameters,
    std::span<const std::uint8_t> previousFrame) {
    (void)validateGeometry(layout, parameters, residual.size(), previousFrame.size());
    if (previousFrame.empty()) {
        throw std::invalid_argument("integer temporal delta requires a previous frame");
    }
    std::vector<std::uint8_t> restored(residual.size());
    const std::size_t componentBytes = parameters.componentBytes;
    if (componentBytes == 1U) {
        for (std::size_t i = 0U; i < residual.size(); ++i) {
            restored[i] = static_cast<std::uint8_t>(residual[i] + previousFrame[i]);
        }
        return restored;
    }
    const std::uint64_t mask = sampleMask(componentBytes);
    for (std::size_t offset = 0U; offset < residual.size(); offset += componentBytes) {
        const std::uint64_t delta = loadSample(
            residual, offset, componentBytes, parameters.byteOrder);
        const std::uint64_t previous = loadSample(
            previousFrame, offset, componentBytes, parameters.byteOrder);
        storeSample(restored, offset, componentBytes, parameters.byteOrder,
                    (delta + previous) & mask);
    }
    return restored;
}

}  // namespace frameforge
