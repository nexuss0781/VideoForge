#pragma once

#include "frameforge/frame_codec.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace frameforge {

// Byte order used to interpret each fixed-width unsigned component sample.
enum class SampleByteOrder : std::uint8_t {
    LittleEndian = 0,
    BigEndian = 1,
};

// Parameters for the reversible integer-sample Paeth transform, version 1.
// componentBytes is the storage width of one component (1..4 bytes); the
// sample value itself may use fewer than 8*componentBytes significant bits.
struct IntegerPaethParameters {
    std::uint8_t componentBytes = 1;
    SampleByteOrder byteOrder = SampleByteOrder::LittleEndian;
};

inline constexpr std::uint8_t kIntegerPaethTransformVersion = 1;
inline constexpr std::uint8_t kIntegerTemporalDeltaTransformVersion = 1;

// Return a fixed-width residual representation of one frame. The output is the
// same byte length as the input; it is a reversible mathematical transform, not
// a compressor. If previousFrame is non-empty, the transform first takes a
// modular sample-wise temporal difference against that frame, then spatially
// predicts those differences. The decoder must receive the identical previous
// frame. Input components are interpreted independently within each pixel.
[[nodiscard]] std::vector<std::uint8_t> encodeIntegerPaethResidual(
    std::span<const std::uint8_t> frame,
    const FrameLayout& layout,
    IntegerPaethParameters parameters = {},
    std::span<const std::uint8_t> previousFrame = {});

// Exact inverse of encodeIntegerPaethResidual. Pass the same layout,
// parameters, and (when used during encoding) previous frame.
[[nodiscard]] std::vector<std::uint8_t> decodeIntegerPaethResidual(
    std::span<const std::uint8_t> residual,
    const FrameLayout& layout,
    IntegerPaethParameters parameters = {},
    std::span<const std::uint8_t> previousFrame = {});

// Encode a sample-wise modular temporal difference, without a spatial predictor.
// A matching previous frame is mandatory. This separate transform is useful
// when changes are sparse and avoids the serial spatial dependencies of Paeth
// during decoding.
[[nodiscard]] std::vector<std::uint8_t> encodeIntegerTemporalDeltaResidual(
    std::span<const std::uint8_t> frame,
    const FrameLayout& layout,
    IntegerPaethParameters parameters,
    std::span<const std::uint8_t> previousFrame);

// Exact inverse of encodeIntegerTemporalDeltaResidual.
[[nodiscard]] std::vector<std::uint8_t> decodeIntegerTemporalDeltaResidual(
    std::span<const std::uint8_t> residual,
    const FrameLayout& layout,
    IntegerPaethParameters parameters,
    std::span<const std::uint8_t> previousFrame);

}  // namespace frameforge
