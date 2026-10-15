#include "frameforge/language_transform.hpp"

#include <algorithm>
#include <cstdint>
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

void store(std::span<std::uint8_t> bytes, std::size_t offset,
           std::size_t sampleBytes, frameforge::SampleByteOrder order,
           std::uint64_t value) {
    if (order == frameforge::SampleByteOrder::LittleEndian) {
        for (std::size_t i = 0; i < sampleBytes; ++i) {
            bytes[offset + i] = static_cast<std::uint8_t>(value & 0xffU);
            value >>= 8U;
        }
    } else {
        for (std::size_t i = sampleBytes; i > 0U; --i) {
            bytes[offset + i - 1U] = static_cast<std::uint8_t>(value & 0xffU);
            value >>= 8U;
        }
    }
}

std::uint64_t paeth(std::uint64_t left, std::uint64_t above,
                    std::uint64_t upperLeft) {
    const std::int64_t a = static_cast<std::int64_t>(left);
    const std::int64_t b = static_cast<std::int64_t>(above);
    const std::int64_t c = static_cast<std::int64_t>(upperLeft);
    const std::int64_t p = a + b - c;
    const auto distance = [p](std::int64_t value) {
        const std::int64_t delta = p - value;
        return static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
    };
    const std::uint64_t dl = distance(a);
    const std::uint64_t da = distance(b);
    const std::uint64_t du = distance(c);
    if (dl <= da && dl <= du) return left;
    if (da <= du) return above;
    return upperLeft;
}

std::vector<std::uint8_t> referenceBytePaeth(
    std::span<const std::uint8_t> frame, const frameforge::FrameLayout& layout) {
    std::vector<std::uint8_t> result(frame.size());
    std::vector<frameforge::FramePlaneLayout> planes = layout.planes;
    if (planes.empty()) {
        planes.push_back({layout.width * layout.channels, layout.height, layout.channels});
    }
    std::size_t planeOffset = 0U;
    for (const auto& plane : planes) {
        for (std::size_t row = 0; row < plane.rows; ++row) {
            const std::size_t rowOffset = planeOffset + row * plane.rowBytes;
            for (std::size_t column = 0; column < plane.rowBytes; ++column) {
                const std::size_t index = rowOffset + column;
                const std::uint8_t left = column >= plane.pixelBytes
                    ? frame[index - plane.pixelBytes] : 0U;
                const std::uint8_t above = row != 0U
                    ? frame[index - plane.rowBytes] : 0U;
                const std::uint8_t upperLeft = row != 0U && column >= plane.pixelBytes
                    ? frame[index - plane.rowBytes - plane.pixelBytes] : 0U;
                const std::uint8_t prediction = static_cast<std::uint8_t>(paeth(
                    left, above, upperLeft));
                result[index] = static_cast<std::uint8_t>(frame[index] - prediction);
            }
        }
        planeOffset += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
    }
    return result;
}

void oneByteEquivalence() {
    std::mt19937 generator(0x51a7U);
    for (const frameforge::FrameLayout& layout : {
             frameforge::FrameLayout{1U, 1U, 1U, {}},
             frameforge::FrameLayout{19U, 7U, 3U, {}},
             frameforge::FrameLayout{33U, 11U, 4U, {}},
             frameforge::FrameLayout{8U, 4U, 1U,
                 {{8U, 4U, 1U}, {4U, 2U, 1U}}}}) {
        std::size_t size = 0U;
        if (layout.planes.empty()) {
            size = static_cast<std::size_t>(layout.width) * layout.height * layout.channels;
        } else {
            for (const auto& plane : layout.planes) {
                size += static_cast<std::size_t>(plane.rowBytes) * plane.rows;
            }
        }
        std::vector<std::uint8_t> frame(size);
        for (auto& value : frame) value = static_cast<std::uint8_t>(generator());
        const auto actual = frameforge::encodeIntegerPaethResidual(
            frame, layout, {1U, frameforge::SampleByteOrder::LittleEndian});
        require(actual == referenceBytePaeth(frame, layout),
                "1-byte integer Paeth diverged from the established byte Paeth rule");
        require(frameforge::decodeIntegerPaethResidual(
                    actual, layout, {1U, frameforge::SampleByteOrder::LittleEndian}) == frame,
                "one-byte spatial Paeth failed exact inversion");
    }
}

void precisionAndByteOrderProperties() {
    std::mt19937_64 generator(0x9d3a41ULL);
    for (std::uint8_t componentBytes = 1U; componentBytes <= 4U; ++componentBytes) {
        for (const auto order : {frameforge::SampleByteOrder::LittleEndian,
                                 frameforge::SampleByteOrder::BigEndian}) {
            const std::uint32_t channels = static_cast<std::uint32_t>(componentBytes) * 3U;
            const frameforge::FrameLayout layout{37U, 23U, channels, {}};
            const std::size_t size = static_cast<std::size_t>(layout.width) *
                                     layout.height * channels;
            std::vector<std::uint8_t> current(size);
            std::vector<std::uint8_t> previous(size);
            for (auto& value : current) value = static_cast<std::uint8_t>(generator());
            for (auto& value : previous) value = static_cast<std::uint8_t>(generator());
            const frameforge::IntegerPaethParameters parameters{componentBytes, order};
            const auto spatial = frameforge::encodeIntegerPaethResidual(
                current, layout, parameters);
            require(spatial.size() == current.size(),
                    "mathematical transform unexpectedly changed fixed-width residual length");
            require(frameforge::decodeIntegerPaethResidual(
                        spatial, layout, parameters) == current,
                    "spatial transform changed arbitrary sample bytes");
            const auto temporal = frameforge::encodeIntegerPaethResidual(
                current, layout, parameters, previous);
            require(frameforge::decodeIntegerPaethResidual(
                        temporal, layout, parameters, previous) == current,
                    "temporal sample-domain transform changed arbitrary sample bytes");
            const auto temporalDelta = frameforge::encodeIntegerTemporalDeltaResidual(
                current, layout, parameters, previous);
            require(frameforge::decodeIntegerTemporalDeltaResidual(
                        temporalDelta, layout, parameters, previous) == current,
                    "sample-wise temporal-difference transform changed arbitrary bytes");
        }
    }
}

void bitDepthPatterns() {
    constexpr std::uint32_t width = 41U;
    constexpr std::uint32_t height = 17U;
    for (const std::uint32_t bits : {8U, 10U, 12U, 16U, 24U, 32U}) {
        const std::uint8_t componentBytes = static_cast<std::uint8_t>((bits + 7U) / 8U);
        const std::uint32_t channels = componentBytes;
        const frameforge::FrameLayout layout{width, height, channels, {}};
        const std::size_t count = static_cast<std::size_t>(width) * height;
        const std::size_t size = count * componentBytes;
        const std::uint64_t maximum = bits == 32U ? 0xffffffffULL :
            ((1ULL << bits) - 1ULL);
        std::vector<std::uint8_t> frame(size);
        std::vector<std::uint8_t> previous(size);
        const std::uint64_t mask = bits == 32U ? 0xffffffffULL : maximum;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t x = i % width;
            const std::size_t y = i / width;
            const std::uint64_t smooth = (x * 31U + y * 17U) & mask;
            const std::uint64_t value = (i % 13U == 0U) ? maximum : smooth;
            store(frame, i * componentBytes, componentBytes,
                  frameforge::SampleByteOrder::LittleEndian, value);
            const std::uint64_t older = (value + 9U) & mask;
            store(previous, i * componentBytes, componentBytes,
                  frameforge::SampleByteOrder::LittleEndian, older);
        }
        const frameforge::IntegerPaethParameters parameters{
            componentBytes, frameforge::SampleByteOrder::LittleEndian};
        const auto spatial = frameforge::encodeIntegerPaethResidual(frame, layout, parameters);
        require(frameforge::decodeIntegerPaethResidual(spatial, layout, parameters) == frame,
                "bit-depth patterned spatial frame failed exact round-trip");
        const auto temporal = frameforge::encodeIntegerPaethResidual(
            frame, layout, parameters, previous);
        require(frameforge::decodeIntegerPaethResidual(
                    temporal, layout, parameters, previous) == frame,
                "bit-depth patterned temporal frame failed exact round-trip");
    }
}

void randomizedGeometryProperties() {
    std::mt19937 generator(0x82c41U);
    const auto bounded = [&generator](std::uint32_t upperExclusive) {
        return static_cast<std::uint32_t>(generator() % upperExclusive);
    };
    for (std::uint32_t trial = 0U; trial < 128U; ++trial) {
        const std::uint8_t componentBytes = static_cast<std::uint8_t>(1U + generator() % 4U);
        const auto order = (trial % 2U == 0U)
            ? frameforge::SampleByteOrder::LittleEndian
            : frameforge::SampleByteOrder::BigEndian;
        const std::uint32_t width = 1U + bounded(31U);
        const std::uint32_t height = 1U + bounded(19U);
        const std::uint32_t channels = static_cast<std::uint32_t>(componentBytes) *
                                       (1U + bounded(4U));
        const frameforge::FrameLayout packed{width, height, channels, {}};
        const std::size_t packedSize = static_cast<std::size_t>(width) * height * channels;
        std::vector<std::uint8_t> current(packedSize);
        std::vector<std::uint8_t> previous(packedSize);
        for (auto& value : current) value = static_cast<std::uint8_t>(generator());
        for (auto& value : previous) value = static_cast<std::uint8_t>(generator());
        const frameforge::IntegerPaethParameters parameters{componentBytes, order};
        const auto residual = frameforge::encodeIntegerPaethResidual(
            current, packed, parameters, previous);
        require(frameforge::decodeIntegerPaethResidual(
                    residual, packed, parameters, previous) == current,
                "randomized packed layout failed exact temporal inversion");

        const std::uint32_t firstPixelBytes = static_cast<std::uint32_t>(componentBytes) *
                                              (1U + bounded(3U));
        const std::uint32_t secondPixelBytes = static_cast<std::uint32_t>(componentBytes) *
                                               (1U + bounded(2U));
        const std::uint32_t firstRowBytes = firstPixelBytes * (1U + bounded(13U));
        const std::uint32_t secondRowBytes = secondPixelBytes * (1U + bounded(9U));
        const std::uint32_t firstRows = 1U + bounded(11U);
        const std::uint32_t secondRows = 1U + bounded(7U);
        const frameforge::FrameLayout planar{
            width, height, 1U,
            {{firstRowBytes, firstRows, firstPixelBytes},
             {secondRowBytes, secondRows, secondPixelBytes}}};
        const std::size_t planarSize = static_cast<std::size_t>(firstRowBytes) * firstRows +
                                       static_cast<std::size_t>(secondRowBytes) * secondRows;
        current.resize(planarSize);
        previous.resize(planarSize);
        for (auto& value : current) value = static_cast<std::uint8_t>(generator());
        for (auto& value : previous) value = static_cast<std::uint8_t>(generator());
        const auto planarResidual = frameforge::encodeIntegerPaethResidual(
            current, planar, parameters, previous);
        require(frameforge::decodeIntegerPaethResidual(
                    planarResidual, planar, parameters, previous) == current,
                "randomized multiplane layout failed exact temporal inversion");
    }
}

void nativeMultiPlaneRoundTrip() {
    const frameforge::FrameLayout layout{
        64U, 32U, 1U,
        {{128U, 32U, 2U}, {64U, 16U, 4U}, {64U, 16U, 4U}}};
    const std::size_t size = 128U * 32U + 64U * 16U * 2U;
    std::mt19937 generator(77123U);
    std::vector<std::uint8_t> current(size);
    std::vector<std::uint8_t> previous(size);
    for (auto& value : current) value = static_cast<std::uint8_t>(generator());
    for (auto& value : previous) value = static_cast<std::uint8_t>(generator());
    const frameforge::IntegerPaethParameters parameters{
        2U, frameforge::SampleByteOrder::LittleEndian};
    const auto encoded = frameforge::encodeIntegerPaethResidual(
        current, layout, parameters, previous);
    require(frameforge::decodeIntegerPaethResidual(
                encoded, layout, parameters, previous) == current,
            "multiplane subsampled layout failed temporal exact round-trip");
}

void rejectInvalidInputs() {
    const frameforge::FrameLayout layout{8U, 4U, 2U, {}};
    const std::vector<std::uint8_t> frame(64U, 0U);
    requireThrows([&] {
        (void)frameforge::encodeIntegerPaethResidual(frame, {0U, 4U, 2U, {}});
    }, "zero-width frame was accepted");
    requireThrows([&] {
        (void)frameforge::encodeIntegerPaethResidual(frame, layout, {0U, {}});
    }, "zero-byte component width was accepted");
    requireThrows([&] {
        (void)frameforge::encodeIntegerPaethResidual(frame, layout, {5U, {}});
    }, "component width above 32 bits was accepted");
    requireThrows([&] {
        (void)frameforge::encodeIntegerPaethResidual(frame, layout, {3U, {}});
    }, "component width not dividing interleaved pixel bytes was accepted");
    requireThrows([&] {
        (void)frameforge::encodeIntegerPaethResidual(
            std::span<const std::uint8_t>(frame).first(10U), layout);
    }, "mismatched frame byte count was accepted");
    requireThrows([&] {
        (void)frameforge::encodeIntegerPaethResidual(
            frame, layout, {}, std::span<const std::uint8_t>(frame).first(12U));
    }, "mismatched temporal seed was accepted");
    requireThrows([&] {
        (void)frameforge::encodeIntegerPaethResidual(frame, layout, {1U, static_cast<
            frameforge::SampleByteOrder>(7U)});
    }, "unknown byte order was accepted");
    requireThrows([&] {
        (void)frameforge::encodeIntegerTemporalDeltaResidual(frame, layout, {}, {});
    }, "temporal-difference transform accepted a missing shared seed");
    const frameforge::FrameLayout invalidPlane{
        2U, 2U, 1U, {{7U, 2U, 2U}}};
    requireThrows([&] {
        (void)frameforge::encodeIntegerPaethResidual(
            std::vector<std::uint8_t>(14U), invalidPlane, {2U, {}});
    }, "plane row not aligned to pixel stride was accepted");
}

}  // namespace

int main() {
    try {
        oneByteEquivalence();
        precisionAndByteOrderProperties();
        bitDepthPatterns();
        randomizedGeometryProperties();
        nativeMultiPlaneRoundTrip();
        rejectInvalidInputs();
    } catch (const std::exception& error) {
        std::cerr << "language transform test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "language transform tests passed\n";
    return 0;
}
