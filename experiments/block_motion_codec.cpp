#include "block_motion_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <zstd.h>

namespace frameforge::experiment {
namespace {

struct PlaneView {
    std::size_t offset;
    std::size_t rowBytes;
    std::size_t rows;
};

std::vector<PlaneView> validateAndGetPlanes(const FrameLayout& layout,
                                            std::size_t& totalBytes) {
    if (layout.width == 0U || layout.height == 0U || layout.planes.empty()) {
        throw std::invalid_argument(
            "block-motion prototype requires positive dimensions and planar layout");
    }
    std::vector<PlaneView> planes;
    planes.reserve(layout.planes.size());
    totalBytes = 0U;
    for (const FramePlaneLayout& plane : layout.planes) {
        if (plane.rowBytes == 0U || plane.rows == 0U || plane.pixelBytes != 1U) {
            throw std::invalid_argument(
                "block-motion prototype supports nonempty 8-bit sample planes only");
        }
        const std::size_t rowBytes = plane.rowBytes;
        const std::size_t rows = plane.rows;
        if (rowBytes > std::numeric_limits<std::size_t>::max() / rows) {
            throw std::length_error("block-motion plane size overflows addressable memory");
        }
        const std::size_t planeBytes = rowBytes * rows;
        if (planeBytes > std::numeric_limits<std::size_t>::max() - totalBytes) {
            throw std::length_error("block-motion frame size overflows addressable memory");
        }
        planes.push_back({totalBytes, rowBytes, rows});
        totalBytes += planeBytes;
    }
    return planes;
}

std::size_t countVectors(const std::vector<PlaneView>& planes) {
    std::size_t count = 0U;
    for (const PlaneView& plane : planes) {
        const std::size_t columns =
            (plane.rowBytes + kBlockMotionBlockSize - 1U) / kBlockMotionBlockSize;
        const std::size_t rows =
            (plane.rows + kBlockMotionBlockSize - 1U) / kBlockMotionBlockSize;
        if (columns != 0U && rows >
                (std::numeric_limits<std::size_t>::max() - count) / columns) {
            throw std::length_error("block-motion vector count overflows addressable memory");
        }
        count += columns * rows;
    }
    return count;
}

std::vector<std::uint8_t> zstdCompress(std::span<const std::uint8_t> input,
                                       int level) {
    const std::size_t bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound) != 0U) {
        throw std::length_error(std::string("block-motion Zstandard bound failed: ") +
                                ZSTD_getErrorName(bound));
    }
    std::vector<std::uint8_t> output(bound);
    const std::size_t compressed =
        ZSTD_compress(output.data(), output.size(), input.data(), input.size(), level);
    if (ZSTD_isError(compressed) != 0U) {
        throw std::runtime_error(std::string("block-motion Zstandard compression failed: ") +
                                 ZSTD_getErrorName(compressed));
    }
    output.resize(compressed);
    return output;
}

std::vector<std::uint8_t> zstdDecompress(std::span<const std::uint8_t> input,
                                         std::size_t expectedSize) {
    std::vector<std::uint8_t> output(expectedSize);
    const std::size_t decompressed = ZSTD_decompress(
        output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(decompressed) != 0U) {
        throw std::runtime_error(std::string("block-motion Zstandard decompression failed: ") +
                                 ZSTD_getErrorName(decompressed));
    }
    if (decompressed != expectedSize) {
        throw std::runtime_error("block-motion decompressed length does not match its layout");
    }
    return output;
}

std::uint64_t sampledSad(std::span<const std::uint8_t> current,
                         std::span<const std::uint8_t> previous,
                         const PlaneView& plane,
                         std::size_t x, std::size_t y,
                         std::size_t blockWidth, std::size_t blockHeight,
                         int dx, int dy, std::uint64_t stopAfter) {
    const std::size_t sourceX = static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(x) + dx);
    const std::size_t sourceY = static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(y) + dy);
    const std::size_t sampleXStep = std::max<std::size_t>(1U, blockWidth / 4U);
    const std::size_t sampleYStep = std::max<std::size_t>(1U, blockHeight / 4U);
    std::uint64_t sad = 0U;
    for (std::size_t localY = 0; localY < blockHeight; localY += sampleYStep) {
        const std::size_t currentRow = plane.offset + (y + localY) * plane.rowBytes + x;
        const std::size_t previousRow =
            plane.offset + (sourceY + localY) * plane.rowBytes + sourceX;
        for (std::size_t localX = 0; localX < blockWidth; localX += sampleXStep) {
            const int difference = static_cast<int>(current[currentRow + localX]) -
                                   static_cast<int>(previous[previousRow + localX]);
            sad += static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
            if (sad > stopAfter) {
                return sad;
            }
        }
    }
    return sad;
}

}  // namespace

BlockMotionFrame encodeBlockMotion(std::span<const std::uint8_t> current,
                                   std::span<const std::uint8_t> previous,
                                   FrameLayout layout,
                                   int compressionLevel) {
    if (compressionLevel < 0 || compressionLevel > 9) {
        throw std::invalid_argument("compression level must be between 0 and 9");
    }
    std::size_t expectedBytes = 0U;
    const std::vector<PlaneView> planes = validateAndGetPlanes(layout, expectedBytes);
    if (current.size() != expectedBytes || previous.size() != expectedBytes) {
        throw std::invalid_argument("block-motion input frame size does not match its layout");
    }

    const std::size_t vectorCount = countVectors(planes);
    if (vectorCount > std::numeric_limits<std::size_t>::max() / 2U) {
        throw std::length_error("block-motion vector table overflows addressable memory");
    }
    std::vector<std::uint8_t> vectorTable;
    vectorTable.reserve(vectorCount * 2U);
    std::vector<std::uint8_t> residual(expectedBytes);

    for (const PlaneView& plane : planes) {
        for (std::size_t y = 0; y < plane.rows; y += kBlockMotionBlockSize) {
            const std::size_t blockHeight =
                std::min(kBlockMotionBlockSize, plane.rows - y);
            for (std::size_t x = 0; x < plane.rowBytes; x += kBlockMotionBlockSize) {
                const std::size_t blockWidth =
                    std::min(kBlockMotionBlockSize, plane.rowBytes - x);
                int bestDx = 0;
                int bestDy = 0;
                std::uint64_t bestSad = sampledSad(
                    current, previous, plane, x, y, blockWidth, blockHeight,
                    0, 0, std::numeric_limits<std::uint64_t>::max());

                for (int dy = -kBlockMotionSearchRadius;
                     dy <= kBlockMotionSearchRadius; ++dy) {
                    const std::ptrdiff_t sourceY =
                        static_cast<std::ptrdiff_t>(y) + dy;
                    if (sourceY < 0 ||
                        static_cast<std::size_t>(sourceY) + blockHeight > plane.rows) {
                        continue;
                    }
                    for (int dx = -kBlockMotionSearchRadius;
                         dx <= kBlockMotionSearchRadius; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const std::ptrdiff_t sourceX =
                            static_cast<std::ptrdiff_t>(x) + dx;
                        if (sourceX < 0 ||
                            static_cast<std::size_t>(sourceX) + blockWidth > plane.rowBytes) {
                            continue;
                        }
                        const std::uint64_t sad = sampledSad(
                            current, previous, plane, x, y, blockWidth, blockHeight,
                            dx, dy, bestSad);
                        if (sad < bestSad) {
                            bestSad = sad;
                            bestDx = dx;
                            bestDy = dy;
                        }
                    }
                }
                vectorTable.push_back(static_cast<std::uint8_t>(
                    bestDx + kBlockMotionSearchRadius));
                vectorTable.push_back(static_cast<std::uint8_t>(
                    bestDy + kBlockMotionSearchRadius));

                const std::size_t sourceX = static_cast<std::size_t>(
                    static_cast<std::ptrdiff_t>(x) + bestDx);
                const std::size_t sourceY = static_cast<std::size_t>(
                    static_cast<std::ptrdiff_t>(y) + bestDy);
                for (std::size_t localY = 0; localY < blockHeight; ++localY) {
                    const std::size_t destinationRow =
                        plane.offset + (y + localY) * plane.rowBytes + x;
                    const std::size_t sourceRow =
                        plane.offset + (sourceY + localY) * plane.rowBytes + sourceX;
                    for (std::size_t localX = 0; localX < blockWidth; ++localX) {
                        residual[destinationRow + localX] = static_cast<std::uint8_t>(
                            static_cast<unsigned int>(current[destinationRow + localX]) -
                            static_cast<unsigned int>(previous[sourceRow + localX]));
                    }
                }
            }
        }
    }

    return BlockMotionFrame{std::move(layout), vectorCount,
                            zstdCompress(vectorTable, compressionLevel),
                            zstdCompress(residual, compressionLevel)};
}

std::vector<std::uint8_t> decodeBlockMotion(
    const BlockMotionFrame& encoded,
    std::span<const std::uint8_t> previous) {
    std::size_t expectedBytes = 0U;
    const std::vector<PlaneView> planes =
        validateAndGetPlanes(encoded.layout, expectedBytes);
    if (previous.size() != expectedBytes) {
        throw std::invalid_argument("block-motion reference frame size does not match its layout");
    }
    const std::size_t expectedVectorCount = countVectors(planes);
    if (encoded.vectorCount != expectedVectorCount ||
        expectedVectorCount > std::numeric_limits<std::size_t>::max() / 2U) {
        throw std::invalid_argument("block-motion vector count does not match its layout");
    }
    const std::vector<std::uint8_t> vectorTable =
        zstdDecompress(encoded.compressedVectors, expectedVectorCount * 2U);
    const std::vector<std::uint8_t> residual =
        zstdDecompress(encoded.compressedResidual, expectedBytes);
    std::vector<std::uint8_t> restored(expectedBytes);
    std::size_t vectorIndex = 0U;

    for (const PlaneView& plane : planes) {
        for (std::size_t y = 0; y < plane.rows; y += kBlockMotionBlockSize) {
            const std::size_t blockHeight =
                std::min(kBlockMotionBlockSize, plane.rows - y);
            for (std::size_t x = 0; x < plane.rowBytes; x += kBlockMotionBlockSize) {
                const std::size_t blockWidth =
                    std::min(kBlockMotionBlockSize, plane.rowBytes - x);
                const int dx = static_cast<int>(vectorTable[vectorIndex++]) -
                              kBlockMotionSearchRadius;
                const int dy = static_cast<int>(vectorTable[vectorIndex++]) -
                              kBlockMotionSearchRadius;
                if (dx < -kBlockMotionSearchRadius ||
                    dx > kBlockMotionSearchRadius ||
                    dy < -kBlockMotionSearchRadius ||
                    dy > kBlockMotionSearchRadius) {
                    throw std::runtime_error("block-motion vector is outside the search range");
                }
                const std::ptrdiff_t sourceX =
                    static_cast<std::ptrdiff_t>(x) + dx;
                const std::ptrdiff_t sourceY =
                    static_cast<std::ptrdiff_t>(y) + dy;
                if (sourceX < 0 || sourceY < 0 ||
                    static_cast<std::size_t>(sourceX) + blockWidth > plane.rowBytes ||
                    static_cast<std::size_t>(sourceY) + blockHeight > plane.rows) {
                    throw std::runtime_error("block-motion vector references outside its plane");
                }
                for (std::size_t localY = 0; localY < blockHeight; ++localY) {
                    const std::size_t destinationRow =
                        plane.offset + (y + localY) * plane.rowBytes + x;
                    const std::size_t sourceRow = plane.offset +
                        (static_cast<std::size_t>(sourceY) + localY) * plane.rowBytes +
                        static_cast<std::size_t>(sourceX);
                    for (std::size_t localX = 0; localX < blockWidth; ++localX) {
                        restored[destinationRow + localX] = static_cast<std::uint8_t>(
                            static_cast<unsigned int>(residual[destinationRow + localX]) +
                            static_cast<unsigned int>(previous[sourceRow + localX]));
                    }
                }
            }
        }
    }
    return restored;
}

}  // namespace frameforge::experiment
