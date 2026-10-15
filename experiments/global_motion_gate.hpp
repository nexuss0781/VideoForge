#pragma once

#include "block_motion_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace frameforge::experiment {

// This experiment is intentionally not a persisted VFS format or a production
// reader. The detector samples a coarse luma grid, then only invokes the
// existing local block matcher when the global shift has strong evidence.
inline constexpr int kGlobalMotionSearchRadius = 32;
inline constexpr std::size_t kGlobalMotionSampleStep = 16;
inline constexpr double kGlobalMotionMinimumImprovement = 0.25;

struct GlobalMotionEstimate {
    int dx = 0;
    int dy = 0;
    double zeroShiftMeanSad = 0.0;
    double bestShiftMeanSad = 0.0;
    double improvement = 0.0;
    std::size_t sampleCount = 0;
    bool gatePassed = false;
};

struct GatedGlobalMotionFrame {
    GlobalMotionEstimate estimate;
    // Valid only when estimate.gatePassed is true.
    BlockMotionFrame refined;
};

// Uses the first full-resolution 8-bit plane as luma. Candidate shifts are
// searched coarsely and refined around the best coarse offset. All candidates
// use the same interior sample grid so border clipping cannot bias the score.
[[nodiscard]] GlobalMotionEstimate estimateGlobalTranslation(
    std::span<const std::uint8_t> current,
    std::span<const std::uint8_t> previous,
    const FrameLayout& layout);

// Run local block-motion refinement only if the luma-only gate passes.
[[nodiscard]] GatedGlobalMotionFrame encodeGatedGlobalMotion(
    std::span<const std::uint8_t> current,
    std::span<const std::uint8_t> previous,
    FrameLayout layout,
    int compressionLevel = 1);

[[nodiscard]] std::vector<std::uint8_t> decodeGatedGlobalMotion(
    const GatedGlobalMotionFrame& encoded,
    std::span<const std::uint8_t> previous);

// Exposed for the benchmark and focused tests; the same deterministic warp is
// used by the encoder and decoder before local residual reconstruction.
[[nodiscard]] std::vector<std::uint8_t> warpReferenceForGlobalTranslation(
    std::span<const std::uint8_t> previous,
    const FrameLayout& layout,
    int dx,
    int dy);

}  // namespace frameforge::experiment
