#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace frameforge {

// Describes one tightly packed plane within an opaque frame buffer. Pixel bytes
// are concatenated plane-by-plane, with no row padding between rows or planes.
struct FramePlaneLayout {
    std::uint32_t rowBytes = 0;
    std::uint32_t rows = 0;
    std::uint32_t pixelBytes = 1;

    friend bool operator==(const FramePlaneLayout&, const FramePlaneLayout&) = default;
};

// Describes a tightly packed interleaved frame, or (when planes is non-empty)
// a byte-exact concatenation of independently packed planes. The codec does not
// interpret colorspace, timestamps, or video metadata.
struct FrameLayout {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    std::vector<FramePlaneLayout> planes;
};

enum class PredictionMode : std::uint8_t {
    None = 0,
    Paeth = 1,
    TemporalDelta = 2,
    TemporalPaeth = 3,
};

// Identifies the payload coding method. Persisted VFS3 records store this value
// explicitly; VFS1/VFS2 records imply Raw or DEFLATE from their legacy flag.
enum class CompressionCodec : std::uint8_t {
    Raw = 0,
    Deflate = 1,
    Zstandard = 2,
};

// A frame's encoded representation. This is an in-memory packet, not yet a
// persisted video/container format. Store the packet fields losslessly if
// composing a container around this core.
struct EncodedFrame {
    FrameLayout layout;
    PredictionMode prediction = PredictionMode::None;
    CompressionCodec codec = CompressionCodec::Raw;
    std::uint64_t decodedSize = 0;
    std::vector<std::uint8_t> payload;
};

// Encode exactly one tightly packed frame. If previousFrame is non-empty, it
// must have the same byte count and temporal prediction candidates are tested.
// compressionLevel is in [0, 9]. Zstandard uses level 1 for both 0 and 1;
// DEFLATE follows zlib's native level semantics. Raw is a decode-only codec.
[[nodiscard]] EncodedFrame encodeLosslessFrame(
    std::span<const std::uint8_t> frame,
    FrameLayout layout,
    std::span<const std::uint8_t> previousFrame = {},
    int compressionLevel = 1,
    CompressionCodec codec = CompressionCodec::Deflate);

// Decode a packet produced by encodeLosslessFrame. A previous frame is required
// for temporal modes and must match this packet's decoded byte count.
[[nodiscard]] std::vector<std::uint8_t> decodeLosslessFrame(
    const EncodedFrame& encoded,
    std::span<const std::uint8_t> previousFrame = {});

[[nodiscard]] const char* predictionModeName(PredictionMode mode) noexcept;

}  // namespace frameforge
