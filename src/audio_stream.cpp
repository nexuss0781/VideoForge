#include "frameforge/audio_stream.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <istream>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <zlib.h>

namespace frameforge {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'V', 'F', 'S', 'A', 'U', 'D', '\r', '\n'};
constexpr std::uint16_t kVersion = 1U;
constexpr std::uint8_t kStreamRecord = 0x10U;
constexpr std::uint8_t kPacketRecord = 1U;
constexpr std::uint8_t kFooterRecord = 0xffU;
constexpr std::size_t kMaxStreams = 256U;
constexpr std::size_t kMaxSideDataEntries = 4096U;
constexpr std::size_t kMaxMetadataEntries = 4096U;
constexpr std::size_t kMaxMetadataBody = 64U * 1024U * 1024U;
constexpr std::size_t kMaxStringBytes = 1024U * 1024U;
constexpr std::size_t kPacketHeaderBytes = 48U;
constexpr std::size_t kMaxAudioPacketBytes = 512U * 1024U * 1024U;

void writeBytes(std::ostream& output, std::span<const std::uint8_t> bytes,
                const char* description) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error(std::string(description) + " exceeds stream write limits");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error(std::string("failed writing ") + description);
    }
}

void readExact(std::istream& input, std::span<std::uint8_t> bytes,
               const char* description) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error(std::string(description) + " exceeds stream read limits");
    }
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error(std::string("truncated VFS audio extension while reading ") +
                                 description);
    }
}

template <typename Unsigned>
void appendLE(std::vector<std::uint8_t>& out, Unsigned value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
        out.push_back(static_cast<std::uint8_t>(value & static_cast<Unsigned>(0xffU)));
        value >>= 8U;
    }
}

void appendI32(std::vector<std::uint8_t>& out, std::int32_t value) {
    appendLE(out, std::bit_cast<std::uint32_t>(value));
}

void appendI64(std::vector<std::uint8_t>& out, std::int64_t value) {
    appendLE(out, std::bit_cast<std::uint64_t>(value));
}

template <typename Unsigned>
Unsigned readLE(std::span<const std::uint8_t> bytes, std::size_t& offset) {
    static_assert(std::is_unsigned_v<Unsigned>);
    if (offset > bytes.size() || sizeof(Unsigned) > bytes.size() - offset) {
        throw std::runtime_error("truncated VFS audio metadata record");
    }
    std::uint64_t value = 0U;
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
    }
    offset += sizeof(Unsigned);
    return static_cast<Unsigned>(value);
}

std::int32_t readI32(std::span<const std::uint8_t> bytes, std::size_t& offset) {
    return std::bit_cast<std::int32_t>(readLE<std::uint32_t>(bytes, offset));
}

std::int64_t readI64(std::span<const std::uint8_t> bytes, std::size_t& offset) {
    return std::bit_cast<std::int64_t>(readLE<std::uint64_t>(bytes, offset));
}

uLong updateCrc(uLong crc, std::span<const std::uint8_t> bytes) {
    constexpr std::size_t chunkLimit = 1U << 20U;
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const std::size_t count = std::min(chunkLimit, bytes.size() - offset);
        crc = crc32(crc, reinterpret_cast<const Bytef*>(bytes.data() + offset),
                    static_cast<uInt>(count));
        offset += count;
    }
    return crc;
}

std::uint32_t crc32Bytes(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint32_t>(updateCrc(crc32(0L, Z_NULL, 0), bytes));
}

std::uint32_t crc32Parts(std::span<const std::uint8_t> first,
                         std::span<const std::uint8_t> second = {}) {
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = updateCrc(crc, first);
    crc = updateCrc(crc, second);
    return static_cast<std::uint32_t>(crc);
}

std::array<std::uint8_t, 4> crcBytes(std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
    return bytes;
}

void appendText(std::vector<std::uint8_t>& body, const std::string& text,
                std::size_t maxBytes, const char* description) {
    if (text.size() > maxBytes || text.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string("VFS audio ") + description + " is too large");
    }
    appendLE(body, static_cast<std::uint32_t>(text.size()));
    body.insert(body.end(), text.begin(), text.end());
}

std::string readText(std::span<const std::uint8_t> body, std::size_t& offset,
                     std::size_t maxBytes, const char* description) {
    const std::uint32_t count = readLE<std::uint32_t>(body, offset);
    if (count > maxBytes || count > body.size() - offset) {
        throw std::runtime_error(std::string("invalid VFS audio ") + description + " length");
    }
    const auto* begin = reinterpret_cast<const char*>(body.data() + offset);
    std::string result(begin, begin + count);
    offset += count;
    return result;
}

void appendSideData(std::vector<std::uint8_t>& out,
                    const std::vector<AudioSideData>& sideData) {
    if (sideData.size() > kMaxSideDataEntries ||
        sideData.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("VFS audio side-data entry count exceeds its limit");
    }
    appendLE(out, static_cast<std::uint16_t>(sideData.size()));
    for (const AudioSideData& item : sideData) {
        if (item.bytes.size() > kMaxMetadataBody ||
            item.bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("VFS audio side-data item exceeds its size limit");
        }
        appendLE(out, item.type);
        appendLE(out, static_cast<std::uint32_t>(item.bytes.size()));
        out.insert(out.end(), item.bytes.begin(), item.bytes.end());
    }
}

std::vector<AudioSideData> readSideData(std::span<const std::uint8_t> body,
                                        std::size_t& offset,
                                        std::size_t count) {
    if (count > kMaxSideDataEntries) {
        throw std::runtime_error("VFS audio side-data count exceeds its limit");
    }
    std::vector<AudioSideData> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        AudioSideData item;
        item.type = readLE<std::uint32_t>(body, offset);
        const std::uint32_t length = readLE<std::uint32_t>(body, offset);
        if (length > kMaxMetadataBody || length > body.size() - offset) {
            throw std::runtime_error("VFS audio side-data length is invalid");
        }
        item.bytes.assign(body.begin() + static_cast<std::ptrdiff_t>(offset),
                          body.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<std::uint8_t> serializeStream(const AudioStreamInfo& stream) {
    if (stream.codecName.empty() || stream.codecName.size() > 255U ||
        stream.channelLayout.size() > 4096U || stream.archiveStreamIndex >= kMaxStreams ||
        stream.timeBaseNumerator <= 0 || stream.timeBaseDenominator <= 0) {
        throw std::invalid_argument("VFS audio stream metadata is incomplete or out of range");
    }
    if (stream.extradata.size() > kMaxMetadataBody ||
        stream.codedSideData.size() > kMaxSideDataEntries ||
        stream.metadata.size() > kMaxMetadataEntries) {
        throw std::length_error("VFS audio stream metadata exceeds its size limits");
    }
    std::vector<std::uint8_t> body;
    body.reserve(112U + stream.codecName.size() + stream.channelLayout.size() +
                 stream.extradata.size());
    appendLE(body, stream.archiveStreamIndex);
    appendI32(body, stream.codecId);
    appendLE(body, stream.codecTag);
    appendI32(body, stream.format);
    appendI32(body, stream.profile);
    appendI32(body, stream.level);
    appendI64(body, stream.bitRate);
    appendI32(body, stream.bitsPerCodedSample);
    appendI32(body, stream.bitsPerRawSample);
    appendI32(body, stream.sampleRate);
    appendI32(body, stream.blockAlign);
    appendI32(body, stream.frameSize);
    appendI32(body, stream.initialPadding);
    appendI32(body, stream.trailingPadding);
    appendI32(body, stream.seekPreroll);
    appendI32(body, stream.channelCount);
    appendI32(body, stream.timeBaseNumerator);
    appendI32(body, stream.timeBaseDenominator);
    appendLE(body, stream.disposition);
    appendI32(body, stream.containerStreamId);
    appendText(body, stream.codecName, 255U, "codec name");
    appendText(body, stream.channelLayout, 4096U, "channel layout");
    appendLE(body, static_cast<std::uint32_t>(stream.extradata.size()));
    body.insert(body.end(), stream.extradata.begin(), stream.extradata.end());
    appendSideData(body, stream.codedSideData);
    appendLE(body, static_cast<std::uint32_t>(stream.metadata.size()));
    for (const auto& [key, value] : stream.metadata) {
        if (key.empty() || key.size() > 4096U) {
            throw std::length_error("VFS audio metadata key is empty or too large");
        }
        appendText(body, key, 4096U, "metadata key");
        appendText(body, value, kMaxStringBytes, "metadata value");
    }
    if (body.size() > kMaxMetadataBody) {
        throw std::length_error("VFS audio stream metadata record exceeds its size limit");
    }
    return body;
}

AudioStreamInfo parseStream(std::span<const std::uint8_t> body) {
    std::size_t offset = 0U;
    AudioStreamInfo stream;
    stream.archiveStreamIndex = readLE<std::uint32_t>(body, offset);
    stream.codecId = readI32(body, offset);
    stream.codecTag = readLE<std::uint32_t>(body, offset);
    stream.format = readI32(body, offset);
    stream.profile = readI32(body, offset);
    stream.level = readI32(body, offset);
    stream.bitRate = readI64(body, offset);
    stream.bitsPerCodedSample = readI32(body, offset);
    stream.bitsPerRawSample = readI32(body, offset);
    stream.sampleRate = readI32(body, offset);
    stream.blockAlign = readI32(body, offset);
    stream.frameSize = readI32(body, offset);
    stream.initialPadding = readI32(body, offset);
    stream.trailingPadding = readI32(body, offset);
    stream.seekPreroll = readI32(body, offset);
    stream.channelCount = readI32(body, offset);
    stream.timeBaseNumerator = readI32(body, offset);
    stream.timeBaseDenominator = readI32(body, offset);
    stream.disposition = readLE<std::uint32_t>(body, offset);
    stream.containerStreamId = readI32(body, offset);
    stream.codecName = readText(body, offset, 255U, "codec name");
    stream.channelLayout = readText(body, offset, 4096U, "channel layout");
    const std::uint32_t extradataLength = readLE<std::uint32_t>(body, offset);
    if (extradataLength > kMaxMetadataBody || extradataLength > body.size() - offset) {
        throw std::runtime_error("VFS audio codec extradata length is invalid");
    }
    stream.extradata.assign(body.begin() + static_cast<std::ptrdiff_t>(offset),
                            body.begin() + static_cast<std::ptrdiff_t>(offset + extradataLength));
    offset += extradataLength;
    const std::uint16_t codedSideCount = readLE<std::uint16_t>(body, offset);
    stream.codedSideData = readSideData(body, offset, codedSideCount);
    const std::uint32_t metadataCount = readLE<std::uint32_t>(body, offset);
    if (metadataCount > kMaxMetadataEntries) {
        throw std::runtime_error("VFS audio stream metadata entry count exceeds its limit");
    }
    for (std::uint32_t i = 0; i < metadataCount; ++i) {
        std::string key = readText(body, offset, 4096U, "metadata key");
        std::string value = readText(body, offset, kMaxStringBytes, "metadata value");
        if (key.empty() || !stream.metadata.emplace(std::move(key), std::move(value)).second) {
            throw std::runtime_error("VFS audio stream metadata contains an invalid or duplicate key");
        }
    }
    if (offset != body.size() || stream.codecName.empty() ||
        stream.archiveStreamIndex >= kMaxStreams || stream.timeBaseNumerator <= 0 ||
        stream.timeBaseDenominator <= 0) {
        throw std::runtime_error("VFS audio stream metadata is inconsistent");
    }
    return stream;
}

}  // namespace

AudioStreamWriter::AudioStreamWriter(std::ostream& output,
                                     std::vector<AudioStreamInfo> streams)
    : output_(output), streams_(std::move(streams)) {
    if (streams_.empty() || streams_.size() > kMaxStreams) {
        throw std::invalid_argument("VFS audio extension must describe 1 to 256 audio streams");
    }
    std::sort(streams_.begin(), streams_.end(),
              [](const AudioStreamInfo& left, const AudioStreamInfo& right) {
                  return left.archiveStreamIndex < right.archiveStreamIndex;
              });
    for (std::size_t i = 0; i < streams_.size(); ++i) {
        if (streams_[i].archiveStreamIndex != i) {
            throw std::invalid_argument("VFS audio stream indices must be contiguous from zero");
        }
    }

    std::vector<std::uint8_t> header;
    appendLE(header, kVersion);
    appendLE(header, static_cast<std::uint16_t>(0U));
    appendLE(header, static_cast<std::uint32_t>(streams_.size()));
    writeBytes(output_, kMagic, "audio extension signature");
    writeBytes(output_, header, "audio extension header");
    const auto headerCrc = crcBytes(crc32Bytes(header));
    writeBytes(output_, headerCrc, "audio extension header checksum");

    for (const AudioStreamInfo& stream : streams_) {
        const std::vector<std::uint8_t> body = serializeStream(stream);
        std::vector<std::uint8_t> recordHeader;
        recordHeader.reserve(8U);
        recordHeader.push_back(kStreamRecord);
        recordHeader.insert(recordHeader.end(), 3U, 0U);
        appendLE(recordHeader, static_cast<std::uint32_t>(body.size()));
        writeBytes(output_, recordHeader, "audio stream record header");
        writeBytes(output_, body, "audio stream metadata");
        const auto checksum = crcBytes(crc32Parts(recordHeader, body));
        writeBytes(output_, checksum, "audio stream metadata checksum");
    }
}

void AudioStreamWriter::writePacket(std::uint32_t archiveStreamIndex,
                                    const AVPacket& packet) {
    if (finished_) {
        throw std::logic_error("cannot write an audio packet after finishing the extension");
    }
    if (archiveStreamIndex >= streams_.size() || packet.size < 0 ||
        (packet.size > 0 && packet.data == nullptr) || packet.side_data_elems < 0 ||
        (packet.side_data_elems > 0 && packet.side_data == nullptr) ||
        static_cast<std::size_t>(packet.size) > kMaxAudioPacketBytes ||
        static_cast<std::size_t>(packet.side_data_elems) > kMaxSideDataEntries) {
        throw std::invalid_argument("audio packet is invalid or exceeds VFS limits");
    }
    for (int i = 0; i < packet.side_data_elems; ++i) {
        const AVPacketSideData& side = packet.side_data[i];
        if (static_cast<std::uint32_t>(side.type) >=
                static_cast<std::uint32_t>(AV_PKT_DATA_NB) ||
            side.size > kMaxMetadataBody || (side.size > 0U && side.data == nullptr)) {
            throw std::runtime_error("audio packet side data cannot be restored safely");
        }
    }
    if (packetCount_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("VFS audio packet count overflow");
    }

    std::vector<std::uint8_t> header;
    header.reserve(kPacketHeaderBytes);
    header.push_back(kPacketRecord);
    header.insert(header.end(), 3U, 0U);
    appendLE(header, archiveStreamIndex);
    appendI64(header, packet.pts);
    appendI64(header, packet.dts);
    appendI64(header, packet.duration);
    appendLE(header, std::bit_cast<std::uint32_t>(packet.flags));
    appendLE(header, static_cast<std::uint64_t>(packet.size));
    appendLE(header, static_cast<std::uint32_t>(packet.side_data_elems));

    uLong crc = updateCrc(crc32(0L, Z_NULL, 0), header);
    writeBytes(output_, header, "audio packet header");
    if (packet.size > 0) {
        const auto payload = std::span<const std::uint8_t>(
            packet.data, static_cast<std::size_t>(packet.size));
        writeBytes(output_, payload, "audio packet payload");
        crc = updateCrc(crc, payload);
    }
    for (int i = 0; i < packet.side_data_elems; ++i) {
        const AVPacketSideData& side = packet.side_data[i];
        std::vector<std::uint8_t> sideHeader;
        sideHeader.reserve(8U);
        appendLE(sideHeader, static_cast<std::uint32_t>(side.type));
        appendLE(sideHeader, static_cast<std::uint32_t>(side.size));
        writeBytes(output_, sideHeader, "audio packet side-data header");
        crc = updateCrc(crc, sideHeader);
        if (side.size > 0U) {
            const auto data = std::span<const std::uint8_t>(side.data, side.size);
            writeBytes(output_, data, "audio packet side data");
            crc = updateCrc(crc, data);
        }
    }
    const auto checksum = crcBytes(static_cast<std::uint32_t>(crc));
    writeBytes(output_, checksum, "audio packet checksum");
    ++packetCount_;
}

void AudioStreamWriter::finish() {
    if (finished_) {
        throw std::logic_error("audio stream extension has already been finished");
    }
    std::vector<std::uint8_t> footer;
    footer.reserve(12U);
    footer.push_back(kFooterRecord);
    footer.insert(footer.end(), 3U, 0U);
    appendLE(footer, packetCount_);
    const auto checksum = crcBytes(crc32Bytes(footer));
    writeBytes(output_, footer, "audio extension footer");
    writeBytes(output_, checksum, "audio extension footer checksum");
    finished_ = true;
}

AudioStreamReader::AudioStreamReader(std::istream& input,
                                     std::uint64_t maxPacketBytes)
    : input_(input), maxPacketBytes_(maxPacketBytes) {
    if (maxPacketBytes_ == 0U || maxPacketBytes_ > kMaxAudioPacketBytes) {
        throw std::invalid_argument("VFS audio packet size limit is outside supported bounds");
    }
    std::array<std::uint8_t, 8> magic{};
    readExact(input_, magic, "extension signature");
    if (magic != kMagic) {
        throw std::runtime_error("invalid VFS audio extension signature");
    }
    std::array<std::uint8_t, 8> header{};
    readExact(input_, header, "extension version and stream count");
    if (crc32Bytes(header) != [&]() {
            std::array<std::uint8_t, 4> checksum{};
            readExact(input_, checksum, "extension header checksum");
            std::size_t offset = 0U;
            return readLE<std::uint32_t>(checksum, offset);
        }()) {
        throw std::runtime_error("VFS audio extension header checksum mismatch");
    }
    std::size_t headerOffset = 0U;
    const std::uint16_t version = readLE<std::uint16_t>(header, headerOffset);
    const std::uint16_t flags = readLE<std::uint16_t>(header, headerOffset);
    const std::uint32_t streamCount = readLE<std::uint32_t>(header, headerOffset);
    if (version != kVersion || flags != 0U || streamCount == 0U ||
        streamCount > kMaxStreams) {
        throw std::runtime_error("unsupported or invalid VFS audio extension header");
    }

    streams_.reserve(streamCount);
    for (std::uint32_t i = 0; i < streamCount; ++i) {
        std::array<std::uint8_t, 8> recordHeader{};
        readExact(input_, recordHeader, "audio stream record header");
        if (recordHeader[0] != kStreamRecord || recordHeader[1] != 0U ||
            recordHeader[2] != 0U || recordHeader[3] != 0U) {
            throw std::runtime_error("VFS audio extension contains an invalid stream record");
        }
        std::size_t lengthOffset = 4U;
        const std::uint32_t bodyLength = readLE<std::uint32_t>(recordHeader, lengthOffset);
        if (bodyLength == 0U || bodyLength > kMaxMetadataBody) {
            throw std::length_error("VFS audio stream metadata length is invalid");
        }
        std::vector<std::uint8_t> body(bodyLength);
        readExact(input_, body, "audio stream metadata");
        std::array<std::uint8_t, 4> checksumBytes{};
        readExact(input_, checksumBytes, "audio stream metadata checksum");
        std::size_t checksumOffset = 0U;
        const std::uint32_t expected = readLE<std::uint32_t>(checksumBytes, checksumOffset);
        if (crc32Parts(recordHeader, body) != expected) {
            throw std::runtime_error("VFS audio stream metadata checksum mismatch");
        }
        AudioStreamInfo stream = parseStream(body);
        if (stream.archiveStreamIndex != i) {
            throw std::runtime_error("VFS audio stream indices are not contiguous");
        }
        streams_.push_back(std::move(stream));
    }
}

bool AudioStreamReader::readNext(AudioPacketRecord& packet) {
    if (finished_) {
        return false;
    }
    const int first = input_.get();
    if (first == std::char_traits<char>::eof()) {
        throw std::runtime_error("VFS audio extension ended before its footer");
    }
    const std::uint8_t recordType = static_cast<std::uint8_t>(first);
    std::array<std::uint8_t, 3> control{};
    readExact(input_, control, "record control fields");
    if (recordType == kFooterRecord) {
        if (control != std::array<std::uint8_t, 3>{0U, 0U, 0U}) {
            throw std::runtime_error("VFS audio footer has nonzero reserved fields");
        }
        std::array<std::uint8_t, 8> countBytes{};
        std::array<std::uint8_t, 4> checksumBytes{};
        readExact(input_, countBytes, "footer packet count");
        readExact(input_, checksumBytes, "footer checksum");
        std::vector<std::uint8_t> footer{recordType, control[0], control[1], control[2]};
        footer.insert(footer.end(), countBytes.begin(), countBytes.end());
        std::size_t checksumOffset = 0U;
        const std::uint32_t expected = readLE<std::uint32_t>(checksumBytes, checksumOffset);
        if (crc32Bytes(footer) != expected) {
            throw std::runtime_error("VFS audio extension footer checksum mismatch");
        }
        std::size_t countOffset = 0U;
        if (readLE<std::uint64_t>(countBytes, countOffset) != packetCount_) {
            throw std::runtime_error("VFS audio footer count does not match packet records");
        }
        if (input_.peek() != std::char_traits<char>::eof()) {
            throw std::runtime_error("VFS audio extension contains trailing bytes after its footer");
        }
        finished_ = true;
        return false;
    }
    if (recordType != kPacketRecord || control != std::array<std::uint8_t, 3>{0U, 0U, 0U}) {
        throw std::runtime_error("VFS audio extension contains an unknown or malformed record");
    }

    std::array<std::uint8_t, kPacketHeaderBytes - 4U> remainder{};
    readExact(input_, remainder, "audio packet header");
    std::vector<std::uint8_t> header{recordType, control[0], control[1], control[2]};
    header.insert(header.end(), remainder.begin(), remainder.end());
    std::size_t offset = 0U;
    AudioPacketRecord parsed;
    parsed.archiveStreamIndex = readLE<std::uint32_t>(remainder, offset);
    parsed.pts = readI64(remainder, offset);
    parsed.dts = readI64(remainder, offset);
    parsed.duration = readI64(remainder, offset);
    parsed.flags = readLE<std::uint32_t>(remainder, offset);
    const std::uint64_t payloadLength = readLE<std::uint64_t>(remainder, offset);
    const std::uint32_t sideCount = readLE<std::uint32_t>(remainder, offset);
    if (parsed.archiveStreamIndex >= streams_.size() || payloadLength > maxPacketBytes_ ||
        payloadLength > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        payloadLength > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        sideCount > kMaxSideDataEntries) {
        throw std::length_error("VFS audio packet metadata exceeds supported limits");
    }
    parsed.payload.resize(static_cast<std::size_t>(payloadLength));
    readExact(input_, parsed.payload, "audio packet payload");
    uLong crc = updateCrc(crc32(0L, Z_NULL, 0), header);
    crc = updateCrc(crc, parsed.payload);
    parsed.sideData.reserve(sideCount);
    for (std::uint32_t i = 0; i < sideCount; ++i) {
        std::array<std::uint8_t, 8> sideHeader{};
        readExact(input_, sideHeader, "audio packet side-data header");
        std::size_t sideOffset = 0U;
        AudioSideData side;
        side.type = readLE<std::uint32_t>(sideHeader, sideOffset);
        const std::uint32_t sideLength = readLE<std::uint32_t>(sideHeader, sideOffset);
        if (sideLength > kMaxMetadataBody) {
            throw std::length_error("VFS audio packet side data exceeds its size limit");
        }
        side.bytes.resize(sideLength);
        readExact(input_, side.bytes, "audio packet side data");
        crc = updateCrc(crc, sideHeader);
        crc = updateCrc(crc, side.bytes);
        parsed.sideData.push_back(std::move(side));
    }
    std::array<std::uint8_t, 4> checksumBytes{};
    readExact(input_, checksumBytes, "audio packet checksum");
    std::size_t checksumOffset = 0U;
    if (static_cast<std::uint32_t>(crc) != readLE<std::uint32_t>(checksumBytes, checksumOffset)) {
        throw std::runtime_error("VFS audio packet checksum mismatch");
    }
    packet = std::move(parsed);
    ++packetCount_;
    return true;
}

}  // namespace frameforge
