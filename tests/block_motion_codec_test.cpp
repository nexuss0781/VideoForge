#include "block_motion_codec.hpp"

#include "frameforge/frame_codec.hpp"

#include <cstdint>
#include <algorithm>
#include <exception>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Callable>
void requireThrows(Callable&& callable, const std::string& message) {
    bool threw = false;
    try {
        callable();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, message);
}

frameforge::FrameLayout yuv420Layout(std::uint32_t width,
                                     std::uint32_t height) {
    return {width, height, 1U,
            {{width, height, 1U}, {width / 2U, height / 2U, 1U},
             {width / 2U, height / 2U, 1U}}};
}

std::vector<std::uint8_t> translatedFrame(
    const std::vector<std::uint8_t>& source,
    const frameforge::FrameLayout& layout,
    int dx, int dy) {
    std::vector<std::uint8_t> translated(source.size());
    std::size_t planeOffset = 0U;
    for (const frameforge::FramePlaneLayout& plane : layout.planes) {
        for (std::size_t y = 0; y < plane.rows; ++y) {
            for (std::size_t x = 0; x < plane.rowBytes; ++x) {
                const std::size_t sourceX = static_cast<std::size_t>(
                    std::clamp(static_cast<int>(x) - dx, 0,
                               static_cast<int>(plane.rowBytes) - 1));
                const std::size_t sourceY = static_cast<std::size_t>(
                    std::clamp(static_cast<int>(y) - dy, 0,
                               static_cast<int>(plane.rows) - 1));
                translated[planeOffset + y * plane.rowBytes + x] =
                    source[planeOffset + sourceY * plane.rowBytes + sourceX];
            }
        }
        planeOffset += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
    }
    return translated;
}

void exactTranslationAndSizeAdvantage() {
    const frameforge::FrameLayout layout = yuv420Layout(128U, 64U);
    const std::size_t frameBytes =
        128U * 64U + 2U * (64U * 32U);
    std::mt19937 random(0x6d6f7469U);
    std::vector<std::uint8_t> previous(frameBytes);
    for (std::uint8_t& value : previous) {
        value = static_cast<std::uint8_t>(random() & 0xffU);
    }
    const std::vector<std::uint8_t> current =
        translatedFrame(previous, layout, 2, 1);

    const frameforge::EncodedFrame baseline = frameforge::encodeLosslessFrame(
        current, layout, previous, 1, frameforge::CompressionCodec::Zstandard);
    const frameforge::experiment::BlockMotionFrame motion =
        frameforge::experiment::encodeBlockMotion(current, previous, layout);
    const std::vector<std::uint8_t> restored =
        frameforge::experiment::decodeBlockMotion(motion, previous);
    require(restored == current,
            "block-motion translation reconstruction changed decoded bytes");
    require(motion.vectorCount == 48U,
            "block-motion vector count does not match the YUV420 plane geometry");
    const std::size_t motionPayloadBytes = 4U + motion.compressedVectors.size() +
                                          motion.compressedResidual.size();
    require(motionPayloadBytes < baseline.payload.size(),
            "translated random-texture frame did not beat the current temporal predictor");
}

void smallAndDegeneratePlanes() {
    const frameforge::FrameLayout layout{
        7U, 5U, 1U, {{7U, 5U, 1U}, {3U, 2U, 1U}}};
    std::vector<std::uint8_t> previous(41U, 19U);
    std::vector<std::uint8_t> current = previous;
    current[0] = 20U;
    current.back() = 18U;
    const auto encoded = frameforge::experiment::encodeBlockMotion(
        current, previous, layout);
    require(frameforge::experiment::decodeBlockMotion(encoded, previous) == current,
            "small partial blocks did not round-trip exactly");
}

void rejectsMalformedInputs() {
    const frameforge::FrameLayout layout = yuv420Layout(16U, 16U);
    const std::vector<std::uint8_t> frame(16U * 16U + 2U * 8U * 8U, 0U);
    requireThrows([&] {
        (void)frameforge::experiment::encodeBlockMotion(
            frame, std::span<const std::uint8_t>(frame).first(frame.size() - 1U),
            layout);
    }, "block-motion encoder accepted a mismatched reference frame");
    requireThrows([&] {
        (void)frameforge::experiment::encodeBlockMotion(frame, frame, layout, 10);
    }, "block-motion encoder accepted an invalid compression level");

    auto malformed = frameforge::experiment::encodeBlockMotion(frame, frame, layout);
    ++malformed.vectorCount;
    requireThrows([&] {
        (void)frameforge::experiment::decodeBlockMotion(malformed, frame);
    }, "block-motion decoder accepted a vector count inconsistent with the layout");
}

}  // namespace

int main() {
    try {
        exactTranslationAndSizeAdvantage();
        smallAndDegeneratePlanes();
        rejectsMalformedInputs();
    } catch (const std::exception& error) {
        std::cerr << "block-motion experiment test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "block-motion experiment tests passed\n";
    return 0;
}
