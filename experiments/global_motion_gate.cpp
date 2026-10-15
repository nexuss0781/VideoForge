#include "global_motion_gate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace frameforge::experiment {
namespace {

struct PlaneView {
    std::size_t offset;
    std::size_t rowBytes;
    std::size_t rows;
};

std::vector<PlaneView> validateLayout(const FrameLayout& layout,
                                      std::size_t& totalBytes) {
    if (layout.width == 0U || layout.height == 0U || layout.planes.empty()) {
        throw std::invalid_argument("global-motion experiment requires nonempty planar frames");
    }
    if (layout.planes.front().pixelBytes != 1U ||
        layout.planes.front().rowBytes != layout.width ||
        layout.planes.front().rows != layout.height) {
        throw std::invalid_argument(
            "global-motion experiment requires a full-resolution 8-bit first plane");
    }

    totalBytes = 0U;
    std::vector<PlaneView> planes;
    planes.reserve(layout.planes.size());
    for (const FramePlaneLayout& plane : layout.planes) {
        if (plane.pixelBytes != 1U || plane.rowBytes == 0U || plane.rows == 0U) {
            throw std::invalid_argument(
                "global-motion experiment supports nonempty 8-bit planes only");
        }
        const std::size_t rowBytes = plane.rowBytes;
        const std::size_t rows = plane.rows;
        if (rowBytes > std::numeric_limits<std::size_t>::max() / rows) {
            throw std::length_error("global-motion plane size overflows addressable memory");
        }
        const std::size_t planeBytes = rowBytes * rows;
        if (planeBytes > std::numeric_limits<std::size_t>::max() - totalBytes) {
            throw std::length_error("global-motion frame size overflows addressable memory");
        }
        planes.push_back({totalBytes, rowBytes, rows});
        totalBytes += planeBytes;
    }
    return planes;
}

struct SampleGrid {
    std::size_t firstX;
    std::size_t firstY;
    std::size_t endX;
    std::size_t endY;
    std::size_t count;
    std::size_t step;
};

struct DownsampledLuma {
    std::size_t width;
    std::size_t height;
    std::vector<std::uint8_t> samples;
};

DownsampledLuma downsampleLuma(std::span<const std::uint8_t> frame,
                               std::size_t width,
                               std::size_t height) {
    constexpr std::size_t factor = 8U;
    const std::size_t reducedWidth = width / factor;
    const std::size_t reducedHeight = height / factor;
    if (reducedWidth == 0U || reducedHeight == 0U ||
        reducedWidth > std::numeric_limits<std::size_t>::max() / reducedHeight) {
        throw std::invalid_argument("global-motion downsampled luma dimensions are invalid");
    }
    std::vector<std::uint8_t> reduced(reducedWidth * reducedHeight);
    for (std::size_t reducedY = 0U; reducedY < reducedHeight; ++reducedY) {
        for (std::size_t reducedX = 0U; reducedX < reducedWidth; ++reducedX) {
            std::uint32_t sum = 0U;
            for (std::size_t localY = 0U; localY < factor; ++localY) {
                const std::size_t row = (reducedY * factor + localY) * width +
                                        reducedX * factor;
                for (std::size_t localX = 0U; localX < factor; ++localX) {
                    sum += frame[row + localX];
                }
            }
            reduced[reducedY * reducedWidth + reducedX] =
                static_cast<std::uint8_t>((sum + 32U) / 64U);
        }
    }
    return {reducedWidth, reducedHeight, std::move(reduced)};
}

std::uint64_t downsampledSad(const DownsampledLuma& current,
                             const DownsampledLuma& previous,
                             int dx, int dy) {
    constexpr std::size_t factor = 8U;
    const std::size_t margin =
        static_cast<std::size_t>(kGlobalMotionSearchRadius) / factor;
    const std::size_t endX = std::min(current.width, previous.width) - margin;
    const std::size_t endY = std::min(current.height, previous.height) - margin;
    std::uint64_t sad = 0U;
    for (std::size_t y = margin; y < endY; ++y) {
        const std::ptrdiff_t previousY = static_cast<std::ptrdiff_t>(y) - dy / 8;
        const std::size_t currentRow = y * current.width;
        const std::size_t previousRow = static_cast<std::size_t>(previousY) * previous.width;
        for (std::size_t x = margin; x < endX; ++x) {
            const std::ptrdiff_t previousX = static_cast<std::ptrdiff_t>(x) - dx / 8;
            const int difference =
                static_cast<int>(current.samples[currentRow + x]) -
                static_cast<int>(previous.samples[
                    previousRow + static_cast<std::size_t>(previousX)]);
            sad += static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
        }
    }
    return sad;
}

SampleGrid makeSampleGrid(std::size_t width, std::size_t height,
                          std::size_t step) {
    const std::size_t radius = static_cast<std::size_t>(kGlobalMotionSearchRadius);
    if (width <= radius * 2U || height <= radius * 2U) {
        throw std::invalid_argument(
            "global-motion luma dimensions must exceed twice the search radius");
    }
    const std::size_t first = radius;
    const std::size_t endX = width - radius;
    const std::size_t endY = height - radius;
    const std::size_t columns = (endX - first + step - 1U) / step;
    const std::size_t rows = (endY - first + step - 1U) / step;
    if (columns == 0U || rows == 0U ||
        columns > std::numeric_limits<std::size_t>::max() / rows) {
        throw std::invalid_argument("global-motion sample grid is too small or overflows");
    }
    return {first, first, endX, endY, columns * rows, step};
}

std::uint64_t lumaSad(std::span<const std::uint8_t> current,
                      std::span<const std::uint8_t> previous,
                      std::size_t lumaWidth,
                      const SampleGrid& grid,
                      int dx, int dy) {
    std::uint64_t sad = 0U;
    for (std::size_t y = grid.firstY; y < grid.endY; y += grid.step) {
        const std::size_t currentRow = y * lumaWidth;
        const std::size_t previousY = static_cast<std::size_t>(
            static_cast<std::ptrdiff_t>(y) - dy);
        const std::size_t previousRow = previousY * lumaWidth;
        for (std::size_t x = grid.firstX; x < grid.endX; x += grid.step) {
            const std::size_t previousX = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(x) - dx);
            const int difference = static_cast<int>(current[currentRow + x]) -
                                   static_cast<int>(previous[previousRow + previousX]);
            sad += static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
        }
    }
    return sad;
}

int scaledOffset(std::size_t planeDimension,
                 std::size_t lumaDimension,
                 int offset) {
    const std::uint64_t magnitude = static_cast<std::uint64_t>(
        offset < 0 ? -offset : offset);
    const std::uint64_t numerator = magnitude * planeDimension + lumaDimension / 2U;
    const int rounded = static_cast<int>(numerator / lumaDimension);
    return offset < 0 ? -rounded : rounded;
}

std::ptrdiff_t signedPlaneOffset(std::size_t planeDimension,
                                 std::size_t lumaDimension,
                                 int offset) {
    return static_cast<std::ptrdiff_t>(
        scaledOffset(planeDimension, lumaDimension, offset));
}

}  // namespace

GlobalMotionEstimate estimateGlobalTranslation(
    std::span<const std::uint8_t> current,
    std::span<const std::uint8_t> previous,
    const FrameLayout& layout) {
    std::size_t expectedBytes = 0U;
    (void)validateLayout(layout, expectedBytes);
    if (current.size() != expectedBytes || previous.size() != expectedBytes) {
        throw std::invalid_argument(
            "global-motion input frame size does not match its layout");
    }

    const std::size_t width = layout.planes.front().rowBytes;
    const std::size_t height = layout.planes.front().rows;
    const SampleGrid searchGrid = makeSampleGrid(
        width, height, kGlobalMotionSampleStep);
    const DownsampledLuma currentCoarse = downsampleLuma(current, width, height);
    const DownsampledLuma previousCoarse = downsampleLuma(previous, width, height);
    int bestDx = 0;
    int bestDy = 0;
    std::uint64_t bestSad = std::numeric_limits<std::uint64_t>::max();

    // The coarse stage bounds the search; the fine stage resolves single-pixel
    // offsets only in the neighbourhood of that coarse winner.
    constexpr int coarseStep = 8;
    for (int dy = -kGlobalMotionSearchRadius;
         dy <= kGlobalMotionSearchRadius; dy += coarseStep) {
        for (int dx = -kGlobalMotionSearchRadius;
             dx <= kGlobalMotionSearchRadius; dx += coarseStep) {
            const std::uint64_t sad = downsampledSad(
                currentCoarse, previousCoarse, dx, dy);
            if (sad < bestSad ||
                (sad == bestSad && std::abs(dx) + std::abs(dy) <
                                       std::abs(bestDx) + std::abs(bestDy))) {
                bestSad = sad;
                bestDx = dx;
                bestDy = dy;
            }
        }
    }

    const int coarseDx = bestDx;
    const int coarseDy = bestDy;
    const int fineRadius = coarseStep - 1;
    bestSad = std::numeric_limits<std::uint64_t>::max();
    for (int dy = std::max(-kGlobalMotionSearchRadius, coarseDy - fineRadius);
         dy <= std::min(kGlobalMotionSearchRadius, coarseDy + fineRadius); ++dy) {
        for (int dx = std::max(-kGlobalMotionSearchRadius, coarseDx - fineRadius);
             dx <= std::min(kGlobalMotionSearchRadius, coarseDx + fineRadius); ++dx) {
            const std::uint64_t sad = lumaSad(
                current, previous, width, searchGrid, dx, dy);
            if (sad < bestSad ||
                (sad == bestSad && std::abs(dx) + std::abs(dy) <
                                       std::abs(bestDx) + std::abs(bestDy))) {
                bestSad = sad;
                bestDx = dx;
                bestDy = dy;
            }
        }
    }

    const SampleGrid scoringGrid = makeSampleGrid(width, height, 4U);
    const std::uint64_t zeroSad = lumaSad(
        current, previous, width, scoringGrid, 0, 0);
    bestSad = lumaSad(current, previous, width, scoringGrid, bestDx, bestDy);
    const double zeroMean = static_cast<double>(zeroSad) /
                            static_cast<double>(scoringGrid.count);
    const double bestMean = static_cast<double>(bestSad) /
                            static_cast<double>(scoringGrid.count);
    const double improvement = zeroSad == 0U
        ? 0.0
        : static_cast<double>(zeroSad - std::min(zeroSad, bestSad)) /
              static_cast<double>(zeroSad);
    const bool nonzeroShift = bestDx != 0 || bestDy != 0;
    const bool enoughSamples = scoringGrid.count >= 16U;
    const bool gatePassed = nonzeroShift && enoughSamples &&
                            improvement >= kGlobalMotionMinimumImprovement;
    return {bestDx, bestDy, zeroMean, bestMean, improvement,
            scoringGrid.count, gatePassed};
}

std::vector<std::uint8_t> warpReferenceForGlobalTranslation(
    std::span<const std::uint8_t> previous,
    const FrameLayout& layout,
    int dx,
    int dy) {
    if (dx < -kGlobalMotionSearchRadius || dx > kGlobalMotionSearchRadius ||
        dy < -kGlobalMotionSearchRadius || dy > kGlobalMotionSearchRadius) {
        throw std::invalid_argument("global-motion offset exceeds the bounded search range");
    }
    std::size_t expectedBytes = 0U;
    const std::vector<PlaneView> planes = validateLayout(layout, expectedBytes);
    if (previous.size() != expectedBytes) {
        throw std::invalid_argument(
            "global-motion reference frame size does not match its layout");
    }
    std::vector<std::uint8_t> warped(expectedBytes);
    for (std::size_t planeIndex = 0U; planeIndex < planes.size(); ++planeIndex) {
        const PlaneView& plane = planes[planeIndex];
        const std::size_t lumaScaleWidth = layout.planes.front().rowBytes;
        const std::size_t lumaScaleHeight = layout.planes.front().rows;
        const std::ptrdiff_t planeDx = signedPlaneOffset(
            plane.rowBytes, lumaScaleWidth, dx);
        const std::ptrdiff_t planeDy = signedPlaneOffset(
            plane.rows, lumaScaleHeight, dy);
        for (std::size_t y = 0U; y < plane.rows; ++y) {
            const std::ptrdiff_t sourceYRaw = static_cast<std::ptrdiff_t>(y) - planeDy;
            const std::size_t sourceY = static_cast<std::size_t>(std::clamp(
                sourceYRaw, static_cast<std::ptrdiff_t>(0),
                static_cast<std::ptrdiff_t>(plane.rows - 1U)));
            for (std::size_t x = 0U; x < plane.rowBytes; ++x) {
                const std::ptrdiff_t sourceXRaw = static_cast<std::ptrdiff_t>(x) - planeDx;
                const std::size_t sourceX = static_cast<std::size_t>(std::clamp(
                    sourceXRaw, static_cast<std::ptrdiff_t>(0),
                    static_cast<std::ptrdiff_t>(plane.rowBytes - 1U)));
                warped[plane.offset + y * plane.rowBytes + x] =
                    previous[plane.offset + sourceY * plane.rowBytes + sourceX];
            }
        }
    }
    return warped;
}

GatedGlobalMotionFrame encodeGatedGlobalMotion(
    std::span<const std::uint8_t> current,
    std::span<const std::uint8_t> previous,
    FrameLayout layout,
    int compressionLevel) {
    if (compressionLevel < 0 || compressionLevel > 9) {
        throw std::invalid_argument("compression level must be between 0 and 9");
    }
    const GlobalMotionEstimate estimate =
        estimateGlobalTranslation(current, previous, layout);
    GatedGlobalMotionFrame result;
    result.estimate = estimate;
    if (estimate.gatePassed) {
        const std::vector<std::uint8_t> warped = warpReferenceForGlobalTranslation(
            previous, layout, estimate.dx, estimate.dy);
        result.refined = encodeBlockMotion(current, warped, std::move(layout),
                                           compressionLevel);
    }
    return result;
}

std::vector<std::uint8_t> decodeGatedGlobalMotion(
    const GatedGlobalMotionFrame& encoded,
    std::span<const std::uint8_t> previous) {
    if (!encoded.estimate.gatePassed) {
        throw std::invalid_argument(
            "cannot decode a global-motion packet when the luma gate did not pass");
    }
    const std::vector<std::uint8_t> warped = warpReferenceForGlobalTranslation(
        previous, encoded.refined.layout, encoded.estimate.dx, encoded.estimate.dy);
    return decodeBlockMotion(encoded.refined, warped);
}

}  // namespace frameforge::experiment
