#include "frameforge/frame_codec.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
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

void roundTripPatterns() {
    constexpr frameforge::FrameLayout layout{127U, 73U, 3U, {}};
    const std::size_t size = static_cast<std::size_t>(layout.width) * layout.height *
                             layout.channels;
    std::vector<std::vector<std::uint8_t>> cases;

    cases.emplace_back(size, 42U);
    cases.emplace_back(size);
    for (std::uint32_t y = 0; y < layout.height; ++y) {
        for (std::uint32_t x = 0; x < layout.width; ++x) {
            for (std::uint32_t channel = 0; channel < layout.channels; ++channel) {
                const std::size_t index =
                    (static_cast<std::size_t>(y) * layout.width + x) * layout.channels + channel;
                cases.back()[index] = static_cast<std::uint8_t>(
                    (x * 3U + y * 5U + channel * 71U) & 0xffU);
            }
        }
    }
    std::mt19937 generator(0xF04A9EU);
    cases.emplace_back(size);
    for (std::uint8_t& value : cases.back()) {
        value = static_cast<std::uint8_t>(generator() & 0xffU);
    }

    for (const auto& input : cases) {
        const frameforge::EncodedFrame packet = frameforge::encodeLosslessFrame(input, layout);
        const std::vector<std::uint8_t> output = frameforge::decodeLosslessFrame(packet);
        require(output == input, "independent lossless frame round-trip changed bytes");
        require(packet.decodedSize == input.size(), "packet decoded size is incorrect");
        require(packet.payload.size() <= input.size(),
                "codec stored a payload larger than the raw frame");
    }
}

void temporalSequence() {
    constexpr frameforge::FrameLayout layout{96U, 64U, 4U, {}};
    const std::size_t size = static_cast<std::size_t>(layout.width) * layout.height *
                             layout.channels;
    std::mt19937 generator(123456U);
    std::vector<std::uint8_t> previous(size);
    for (std::uint8_t& value : previous) {
        value = static_cast<std::uint8_t>(generator() & 0xffU);
    }

    std::vector<std::uint8_t> current = previous;
    for (std::size_t i = 0; i < current.size(); i += 97U) {
        current[i] = static_cast<std::uint8_t>(current[i] + 1U);
    }

    const frameforge::EncodedFrame packet =
        frameforge::encodeLosslessFrame(current, layout, previous);
    require(packet.prediction == frameforge::PredictionMode::TemporalDelta ||
                packet.prediction == frameforge::PredictionMode::TemporalPaeth,
            "sampled predictor selection missed strong temporal redundancy");
    const std::vector<std::uint8_t> restored =
        frameforge::decodeLosslessFrame(packet, previous);
    require(restored == current, "temporal sequence round-trip changed bytes");
    require(packet.payload.size() < current.size(),
            "small temporal changes did not produce a compact representation");
    requireThrows([&] { (void)frameforge::decodeLosslessFrame(packet); },
                  "temporal packet decoded without its shared previous-frame seed");

    // The decoder's output is the next encoder/decoder shared seed, not hidden
    // encoder-side state, so a multi-frame chain remains independently verifiable.
    std::vector<std::uint8_t> next = restored;
    for (std::size_t i = 3; i < next.size(); i += 131U) {
        next[i] = static_cast<std::uint8_t>(next[i] ^ 0x31U);
    }
    const frameforge::EncodedFrame nextPacket =
        frameforge::encodeLosslessFrame(next, layout, restored);
    require(frameforge::decodeLosslessFrame(nextPacket, restored) == next,
            "second temporal frame did not round-trip from the shared decoded seed");
}

void zstandardRoundTripAndRawFallback() {
    constexpr frameforge::FrameLayout layout{256U, 128U, 1U, {}};
    std::vector<std::uint8_t> compressible(
        static_cast<std::size_t>(layout.width) * layout.height, 0x37U);
    const frameforge::EncodedFrame packet = frameforge::encodeLosslessFrame(
        compressible, layout, {}, 1, frameforge::CompressionCodec::Zstandard);
    require(packet.codec == frameforge::CompressionCodec::Zstandard,
            "compressible frame did not select its explicit Zstandard codec");
    require(packet.payload.size() < compressible.size(),
            "Zstandard did not compact a highly repetitive frame");
    require(frameforge::decodeLosslessFrame(packet) == compressible,
            "Zstandard frame round-trip changed bytes");

    std::mt19937 generator(0x5a17U);
    std::vector<std::uint8_t> incompressible(compressible.size());
    for (std::uint8_t& value : incompressible) {
        value = static_cast<std::uint8_t>(generator() & 0xffU);
    }
    const frameforge::EncodedFrame rawPacket = frameforge::encodeLosslessFrame(
        incompressible, layout, {}, 1, frameforge::CompressionCodec::Zstandard);
    require(rawPacket.codec == frameforge::CompressionCodec::Raw,
            "incompressible frame did not fall back to the raw codec");
    require(frameforge::decodeLosslessFrame(rawPacket) == incompressible,
            "raw fallback round-trip changed bytes");

    const frameforge::EncodedFrame legacyPacket =
        frameforge::encodeLosslessFrame(compressible, layout);
    require(legacyPacket.codec == frameforge::CompressionCodec::Deflate,
            "default frame codec no longer preserves legacy DEFLATE behavior");
    require(frameforge::decodeLosslessFrame(legacyPacket) == compressible,
            "legacy DEFLATE packet round-trip changed bytes");
}

void smallLayouts() {
    std::mt19937 generator(67890U);
    for (const std::uint32_t width : {1U, 2U, 5U, 19U}) {
        for (const std::uint32_t height : {1U, 3U, 11U}) {
            for (const std::uint32_t channels : {1U, 2U, 3U, 4U}) {
                const frameforge::FrameLayout layout{width, height, channels, {}};
                const std::size_t size = static_cast<std::size_t>(width) * height * channels;
                std::vector<std::uint8_t> input(size);
                for (std::uint8_t& value : input) {
                    value = static_cast<std::uint8_t>(generator() & 0xffU);
                }
                const frameforge::EncodedFrame packet =
                    frameforge::encodeLosslessFrame(input, layout);
                require(frameforge::decodeLosslessFrame(packet) == input,
                        "small or degenerate layout did not round-trip exactly");
            }
        }
    }
}

void rejectInvalidInputs() {
    constexpr frameforge::FrameLayout layout{8U, 8U, 3U, {}};
    const std::vector<std::uint8_t> frame(8U * 8U * 3U, 0U);
    requireThrows([&] {
        (void)frameforge::encodeLosslessFrame(frame, {0U, 8U, 3U, {}});
    }, "zero-width frame was accepted");
    requireThrows([&] {
        (void)frameforge::encodeLosslessFrame(std::span<const std::uint8_t>(frame).first(10U), layout);
    }, "mismatched frame byte count was accepted");
    requireThrows([&] {
        (void)frameforge::encodeLosslessFrame(frame, layout, {}, 10);
    }, "out-of-range compression level was accepted");
    requireThrows([&] {
        (void)frameforge::encodeLosslessFrame(frame, layout,
                                              std::span<const std::uint8_t>(frame).first(12U));
    }, "mismatched previous-frame size was accepted");

    frameforge::EncodedFrame badPacket = frameforge::encodeLosslessFrame(frame, layout);
    badPacket.decodedSize += 1U;
    requireThrows([&] { (void)frameforge::decodeLosslessFrame(badPacket); },
                  "packet with inconsistent decoded size was accepted");

    badPacket = frameforge::encodeLosslessFrame(frame, layout);
    badPacket.prediction = static_cast<frameforge::PredictionMode>(0xffU);
    requireThrows([&] { (void)frameforge::decodeLosslessFrame(badPacket); },
                  "packet with an unknown prediction mode was accepted");

    requireThrows([&] {
        (void)frameforge::encodeLosslessFrame(
            frame, {std::numeric_limits<std::uint32_t>::max(),
                    std::numeric_limits<std::uint32_t>::max(),
                    std::numeric_limits<std::uint32_t>::max(), {}});
    }, "overflowing frame geometry was accepted");
}

}  // namespace

int main() {
    try {
        roundTripPatterns();
        temporalSequence();
        zstandardRoundTripAndRawFallback();
        smallLayouts();
        rejectInvalidInputs();
    } catch (const std::exception& error) {
        std::cerr << "frame codec test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "frame codec tests passed\n";
    return 0;
}
