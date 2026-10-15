#include "frameforge/audio_stream.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
}

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

std::string makeAudioExtension() {
    frameforge::AudioStreamInfo stream;
    stream.archiveStreamIndex = 0U;
    stream.codecId = AV_CODEC_ID_PCM_S16LE;
    stream.codecName = "pcm_s16le";
    stream.format = -1;
    stream.profile = -1;
    stream.level = -1;
    stream.sampleRate = 48000;
    stream.channelCount = 1;
    stream.timeBaseNumerator = 1;
    stream.timeBaseDenominator = 48000;
    stream.channelLayout = "mono";
    stream.metadata.emplace("language", "eng");

    std::ostringstream output(std::ios::binary);
    frameforge::AudioStreamWriter writer(output, {stream});
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        throw std::bad_alloc();
    }
    try {
        const int result = av_new_packet(packet, 6);
        if (result < 0) {
            throw std::runtime_error("cannot allocate test packet");
        }
        const std::array<std::uint8_t, 6> payload{0x11U, 0x22U, 0x33U,
                                                  0x44U, 0x55U, 0x66U};
        std::memcpy(packet->data, payload.data(), payload.size());
        packet->pts = 1024;
        packet->dts = 1024;
        packet->duration = 1024;
        packet->flags = AV_PKT_FLAG_KEY;
        std::uint8_t* sideData = av_packet_new_side_data(
            packet, AV_PKT_DATA_SKIP_SAMPLES, 10U);
        if (sideData == nullptr) {
            throw std::bad_alloc();
        }
        const std::array<std::uint8_t, 10> sideBytes{
            1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U};
        std::memcpy(sideData, sideBytes.data(), sideBytes.size());
        writer.writePacket(0U, *packet);
        writer.finish();
        av_packet_free(&packet);
        return output.str();
    } catch (...) {
        av_packet_free(&packet);
        throw;
    }
}

void roundTripDeterministicAudioExtension() {
    const std::string first = makeAudioExtension();
    const std::string second = makeAudioExtension();
    require(first == second, "audio extension serialization is not deterministic");

    std::istringstream input(first, std::ios::binary);
    frameforge::AudioStreamReader reader(input);
    require(reader.streams().size() == 1U, "audio stream metadata count changed");
    const auto& stream = reader.streams().front();
    require(stream.codecId == AV_CODEC_ID_PCM_S16LE &&
                stream.codecName == "pcm_s16le" && stream.sampleRate == 48000 &&
                stream.channelLayout == "mono" && stream.channelCount == 1 &&
                stream.timeBaseNumerator == 1 && stream.timeBaseDenominator == 48000 &&
                stream.metadata.at("language") == "eng",
            "audio codec parameters or metadata changed during round-trip");

    frameforge::AudioPacketRecord packet;
    require(reader.readNext(packet), "audio extension ended before its packet");
    const std::array<std::uint8_t, 6> expectedPayload{
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};
    require(packet.archiveStreamIndex == 0U && packet.pts == 1024 &&
                packet.dts == 1024 && packet.duration == 1024 &&
                packet.flags == AV_PKT_FLAG_KEY &&
                std::equal(packet.payload.begin(), packet.payload.end(),
                           expectedPayload.begin(), expectedPayload.end()),
            "compressed audio packet payload or timing changed during round-trip");
    require(packet.sideData.size() == 1U &&
                packet.sideData[0].type == AV_PKT_DATA_SKIP_SAMPLES &&
                packet.sideData[0].bytes.size() == 10U &&
                packet.sideData[0].bytes[9] == 10U,
            "audio packet side data changed during round-trip");
    require(!reader.readNext(packet) && reader.packetCount() == 1U,
            "audio footer or packet count was not validated");
}

void rejectsCorruption() {
    std::string bytes = makeAudioExtension();
    require(bytes.size() > 64U, "test audio extension unexpectedly small");
    bytes[bytes.size() - 20U] ^= 0x40;
    std::istringstream input(bytes, std::ios::binary);
    frameforge::AudioStreamReader reader(input);
    frameforge::AudioPacketRecord packet;
    requireThrows([&] {
        while (reader.readNext(packet)) {
        }
    }, "corrupt audio packet or footer was accepted");
}

}  // namespace

int main() {
    try {
        roundTripDeterministicAudioExtension();
        rejectsCorruption();
    } catch (const std::exception& error) {
        std::cerr << "audio stream test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "audio stream deterministic round-trip and corruption tests passed\n";
    return 0;
}
