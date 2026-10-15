#include "global_motion_gate.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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
    std::size_t offset = 0U;
    const int lumaWidth = static_cast<int>(layout.planes.front().rowBytes);
    const int lumaHeight = static_cast<int>(layout.planes.front().rows);
    for (const auto& plane : layout.planes) {
        const int planeDx = static_cast<int>(std::lround(
            static_cast<double>(dx) * plane.rowBytes / lumaWidth));
        const int planeDy = static_cast<int>(std::lround(
            static_cast<double>(dy) * plane.rows / lumaHeight));
        for (std::size_t y = 0U; y < plane.rows; ++y) {
            for (std::size_t x = 0U; x < plane.rowBytes; ++x) {
                const int sourceX = std::clamp(static_cast<int>(x) - planeDx, 0,
                                               static_cast<int>(plane.rowBytes) - 1);
                const int sourceY = std::clamp(static_cast<int>(y) - planeDy, 0,
                                               static_cast<int>(plane.rows) - 1);
                translated[offset + y * plane.rowBytes + x] =
                    source[offset + static_cast<std::size_t>(sourceY) * plane.rowBytes +
                           static_cast<std::size_t>(sourceX)];
            }
        }
        offset += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
    }
    return translated;
}

void detectsTranslationAndRoundTripsExactly() {
    const auto layout = yuv420Layout(256U, 128U);
    std::mt19937 random(0x474d4f54U);
    std::vector<std::uint8_t> previous(256U * 128U * 3U / 2U);
    for (std::uint8_t& sample : previous) {
        sample = static_cast<std::uint8_t>(random() & 0xffU);
    }
    const std::vector<std::uint8_t> current = translatedFrame(previous, layout, 6, -4);
    const auto estimate = frameforge::experiment::estimateGlobalTranslation(
        current, previous, layout);
    std::uint64_t knownShiftSad = 0U;
    for (std::size_t y = 32U; y < 96U; y += 16U) {
        for (std::size_t x = 32U; x < 224U; x += 16U) {
            const int difference = static_cast<int>(current[y * 256U + x]) -
                static_cast<int>(previous[(y + 4U) * 256U + (x - 6U)]);
            knownShiftSad += static_cast<std::uint64_t>(
                difference < 0 ? -difference : difference);
        }
    }
    require(estimate.gatePassed, "coherent translation did not pass the luma gate");
    require(estimate.dx == 6 && estimate.dy == -4,
            "coarse-to-fine luma estimator returned (" +
                std::to_string(estimate.dx) + "," + std::to_string(estimate.dy) +
                ") instead of (6,-4); known SAD=" +
                std::to_string(knownShiftSad) + ", estimate SAD=" +
                std::to_string(estimate.bestShiftMeanSad));

    const auto encoded = frameforge::experiment::encodeGatedGlobalMotion(
        current, previous, layout);
    require(encoded.estimate.gatePassed,
            "translation gate was inconsistent between estimate and encoder");
    const auto restored = frameforge::experiment::decodeGatedGlobalMotion(
        encoded, previous);
    require(restored == current,
            "gated global-plus-local motion reconstruction changed frame bytes");
}

void rejectsStaticAndIndependentNoise() {
    const auto layout = yuv420Layout(256U, 128U);
    const std::size_t frameBytes = 256U * 128U * 3U / 2U;
    std::mt19937 random(0x6e6f6973U);
    std::vector<std::uint8_t> first(frameBytes);
    std::vector<std::uint8_t> second(frameBytes);
    for (std::uint8_t& sample : first) {
        sample = static_cast<std::uint8_t>(random() & 0xffU);
    }
    for (std::uint8_t& sample : second) {
        sample = static_cast<std::uint8_t>(random() & 0xffU);
    }

    const auto staticEstimate = frameforge::experiment::estimateGlobalTranslation(
        first, first, layout);
    require(!staticEstimate.gatePassed && staticEstimate.dx == 0 &&
                staticEstimate.dy == 0,
            "identical frames incorrectly triggered block-motion refinement");
    const auto noiseEstimate = frameforge::experiment::estimateGlobalTranslation(
        second, first, layout);
    require(!noiseEstimate.gatePassed,
            "independent temporal noise incorrectly passed the benefit gate");
    const auto noMotion = frameforge::experiment::encodeGatedGlobalMotion(
        first, first, layout);
    require(noMotion.refined.vectorCount == 0U,
            "local refinement ran despite the luma gate rejecting the frame");
}

}  // namespace

int main() {
    try {
        detectsTranslationAndRoundTripsExactly();
        rejectsStaticAndIndependentNoise();
    } catch (const std::exception& error) {
        std::cerr << "global-motion gate test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "global-motion gate tests passed\n";
    return 0;
}
