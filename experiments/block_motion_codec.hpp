#pragma once

#include "frameforge/frame_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace frameforge::experiment {

// Experimental-only, byte-exact block motion packet. This is deliberately not a
// VFS record and is not wired into production readers. Vectors are per plane,
// measured in that plane's 8-bit sample coordinates; residual bytes and vectors
// are separately Zstandard-compressed.
struct BlockMotionFrame {
    FrameLayout layout;
    std::size_t vectorCount = 0;
    std::vector<std::uint8_t> compressedVectors;
    std::vector<std::uint8_t> compressedResidual;
};

inline constexpr std::size_t kBlockMotionBlockSize = 16;
inline constexpr int kBlockMotionSearchRadius = 4;

[[nodiscard]] BlockMotionFrame encodeBlockMotion(
    std::span<const std::uint8_t> current,
    std::span<const std::uint8_t> previous,
    FrameLayout layout,
    int compressionLevel = 1);

[[nodiscard]] std::vector<std::uint8_t> decodeBlockMotion(
    const BlockMotionFrame& encoded,
    std::span<const std::uint8_t> previous);

}  // namespace frameforge::experiment
