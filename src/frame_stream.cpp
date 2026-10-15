#include "frameforge/frame_stream.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <zlib.h>

namespace frameforge {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{
    'V', 'F', 'S', 'T', 'R', 'M', '\r', '\n'};
constexpr std::uint16_t kPackedVersion = 1;
constexpr std::uint16_t kNativeVersion = 2;
constexpr std::uint16_t kCodecVersion = 3;
constexpr std::uint16_t kDisplayMetadataVersion = 4;
constexpr std::uint16_t kAudioExtensionVersion = 5;
constexpr std::uint16_t kNativeMetadataFlag = 1;
constexpr std::uint8_t kFrameRecord = 1;
constexpr std::uint8_t kFooterRecord = 0xff;
constexpr std::uint8_t kCompressedFlag = 1;
constexpr std::size_t kHeaderBodyBytes = 24;
constexpr std::size_t kMaxPixelFormatNameBytes = 255;
constexpr std::size_t kMaxMetadataNameBytes = 63;
constexpr std::size_t kMaxPlaneCount = 4;

bool validPixelFormatName(std::string_view name) {
    if (name.empty() || name.size() > kMaxPixelFormatNameBytes) {
        return false;
    }
    for (const unsigned char value : name) {
        const bool valid = (value >= 'a' && value <= 'z') ||
                           (value >= 'A' && value <= 'Z') ||
                           (value >= '0' && value <= '9') ||
                           value == '_' || value == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

bool validMetadataName(std::string_view name) {
    if (name.size() > kMaxMetadataNameBytes) {
        return false;
    }
    for (const unsigned char value : name) {
        const bool valid = (value >= 'a' && value <= 'z') ||
                           (value >= 'A' && value <= 'Z') ||
                           (value >= '0' && value <= '9') ||
                           value == '_' || value == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

void readExact(std::istream& input, std::span<std::uint8_t> bytes,
               const char* description);

void appendMetadataName(std::vector<std::uint8_t>& output,
                        const std::string& name) {
    if (!validMetadataName(name)) {
        throw std::invalid_argument("VFS4 display metadata contains an invalid name");
    }
    output.push_back(static_cast<std::uint8_t>(name.size()));
    output.insert(output.end(), name.begin(), name.end());
}

void readMetadataName(std::istream& input, std::vector<std::uint8_t>& body,
                      std::string& name, const char* description) {
    std::uint8_t length = 0;
    readExact(input, std::span<std::uint8_t>(&length, 1U), description);
    body.push_back(length);
    if (length > kMaxMetadataNameBytes) {
        throw std::runtime_error("VFS4 display metadata name exceeds its size limit");
    }
    std::vector<std::uint8_t> bytes(length);
    readExact(input, bytes, description);
    body.insert(body.end(), bytes.begin(), bytes.end());
    name.assign(bytes.begin(), bytes.end());
    if (!validMetadataName(name)) {
        throw std::runtime_error("VFS4 display metadata contains an invalid name");
    }
}

constexpr std::size_t kFrameRecordHeaderBytes = 28;
constexpr std::size_t kFrameDisplayStateBytes = 5;
constexpr std::size_t kFooterHeaderBytes = 12;

std::size_t checkedFrameBytes(FrameLayout layout) {
    constexpr std::size_t maxSize = std::numeric_limits<std::size_t>::max();
    if (layout.width == 0U || layout.height == 0U) {
        throw std::invalid_argument("frame stream dimensions must be positive");
    }
    if (!layout.planes.empty()) {
        std::size_t totalBytes = 0U;
        for (const FramePlaneLayout& plane : layout.planes) {
            if (plane.rowBytes == 0U || plane.rows == 0U ||
                plane.pixelBytes == 0U || plane.pixelBytes > plane.rowBytes) {
                throw std::invalid_argument("frame stream plane geometry must be positive and valid");
            }
            const std::size_t rowBytes = plane.rowBytes;
            const std::size_t rows = plane.rows;
            if (rowBytes > maxSize / rows) {
                throw std::length_error("frame stream plane size overflows addressable memory");
            }
            const std::size_t planeBytes = rowBytes * rows;
            if (planeBytes > maxSize - totalBytes) {
                throw std::length_error("frame stream total plane size overflows addressable memory");
            }
            totalBytes += planeBytes;
        }
        return totalBytes;
    }
    if (layout.channels == 0U) {
        throw std::invalid_argument("frame stream interleaved channel count must be positive");
    }
    const std::size_t width = static_cast<std::size_t>(layout.width);
    const std::size_t height = static_cast<std::size_t>(layout.height);
    const std::size_t channels = static_cast<std::size_t>(layout.channels);
    if (width > maxSize / channels) {
        throw std::length_error("frame stream row byte count overflows addressable memory");
    }
    const std::size_t rowBytes = width * channels;
    if (rowBytes > maxSize / height) {
        throw std::length_error("frame stream frame size overflows addressable memory");
    }
    return rowBytes * height;
}

template <typename Unsigned>
void appendLittleEndian(std::vector<std::uint8_t>& output, Unsigned value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
        output.push_back(static_cast<std::uint8_t>(value & static_cast<Unsigned>(0xffU)));
        value >>= 8U;
    }
}

void appendSigned64(std::vector<std::uint8_t>& output, std::int64_t value) {
    appendLittleEndian(output, std::bit_cast<std::uint64_t>(value));
}

template <typename Unsigned>
Unsigned loadLittleEndian(std::span<const std::uint8_t> input, std::size_t offset) {
    static_assert(std::is_unsigned_v<Unsigned>);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
        value |= static_cast<std::uint64_t>(input[offset + i]) << (i * 8U);
    }
    return static_cast<Unsigned>(value);
}

std::uint32_t loadCrc(std::span<const std::uint8_t> input) {
    return loadLittleEndian<std::uint32_t>(input, 0);
}

uLong updateCrc(uLong crc, std::span<const std::uint8_t> bytes) {
    constexpr std::size_t chunkLimit = 1U << 20U;
    std::size_t offset = 0;
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
        throw std::runtime_error(std::string("truncated frame stream while reading ") + description);
    }
}

std::array<std::uint8_t, 4> crcBytes(std::uint32_t crc) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(4);
    appendLittleEndian(bytes, crc);
    return {bytes[0], bytes[1], bytes[2], bytes[3]};
}

void requireCrc(std::uint32_t expected, std::span<const std::uint8_t> first,
                std::span<const std::uint8_t> second, const char* description) {
    if (crc32Parts(first, second) != expected) {
        throw std::runtime_error(std::string("frame stream ") + description + " checksum mismatch");
    }
}

}  // namespace

FrameStreamWriter::FrameStreamWriter(std::ostream& output,
                                     FrameStreamMetadata metadata,
                                     int compressionLevel,
                                     CompressionCodec compressionCodec,
                                     bool includesAudioExtension)
    : output_(output), metadata_(metadata), compressionLevel_(compressionLevel),
      compressionCodec_(compressionCodec) {
    (void)checkedFrameBytes(metadata_.layout);
    if (metadata_.timeBaseNumerator == 0U || metadata_.timeBaseDenominator == 0U) {
        throw std::invalid_argument("frame stream timebase numerator and denominator must be positive");
    }
    if (compressionLevel_ < 0 || compressionLevel_ > 9) {
        throw std::invalid_argument("compression level must be between 0 and 9");
    }
    if (compressionCodec_ != CompressionCodec::Deflate &&
        compressionCodec_ != CompressionCodec::Zstandard) {
        throw std::invalid_argument("frame stream writer requires DEFLATE or Zstandard");
    }

    const bool nativeVideo = !metadata_.pixelFormat.empty();
    if (includesAudioExtension && !nativeVideo) {
        throw std::invalid_argument("VFS5 audio extensions require native video metadata");
    }
    if (nativeVideo) {
        if (!validPixelFormatName(metadata_.pixelFormat) ||
            metadata_.layout.planes.empty() ||
            metadata_.layout.planes.size() > kMaxPlaneCount) {
            throw std::invalid_argument("native video metadata has an invalid pixel format or plane count");
        }
        if (metadata_.layout.channels == 0U) {
            throw std::invalid_argument("native video layout channel marker must be positive");
        }
    } else if (!metadata_.layout.planes.empty()) {
        throw std::invalid_argument("plane layouts require a native pixel-format name");
    }

    std::vector<std::uint8_t> body;
    body.reserve(kHeaderBodyBytes + 4U + metadata_.pixelFormat.size() +
                 metadata_.layout.planes.size() * 12U + 128U);
    formatVersion_ = includesAudioExtension
        ? kAudioExtensionVersion
        : (nativeVideo ? kDisplayMetadataVersion : kCodecVersion);
    appendLittleEndian(body, formatVersion_);
    appendLittleEndian(body, nativeVideo ? kNativeMetadataFlag : static_cast<std::uint16_t>(0));
    appendLittleEndian(body, metadata_.layout.width);
    appendLittleEndian(body, metadata_.layout.height);
    appendLittleEndian(body, metadata_.layout.channels);
    appendLittleEndian(body, metadata_.timeBaseNumerator);
    appendLittleEndian(body, metadata_.timeBaseDenominator);
    if (nativeVideo) {
        appendLittleEndian(body, static_cast<std::uint16_t>(metadata_.pixelFormat.size()));
        appendLittleEndian(body, static_cast<std::uint16_t>(metadata_.layout.planes.size()));
        body.insert(body.end(), metadata_.pixelFormat.begin(), metadata_.pixelFormat.end());
        for (const FramePlaneLayout& plane : metadata_.layout.planes) {
            appendLittleEndian(body, plane.rowBytes);
            appendLittleEndian(body, plane.rows);
            appendLittleEndian(body, plane.pixelBytes);
        }
        if (metadata_.sampleAspectRatioNumerator == 0U ||
            metadata_.sampleAspectRatioDenominator == 0U) {
            throw std::invalid_argument("VFS4 sample aspect ratio must be positive");
        }
        appendMetadataName(body, metadata_.colorRange);
        appendMetadataName(body, metadata_.colorPrimaries);
        appendMetadataName(body, metadata_.colorTransfer);
        appendMetadataName(body, metadata_.colorSpace);
        appendMetadataName(body, metadata_.chromaLocation);
        appendLittleEndian(body, metadata_.sampleAspectRatioNumerator);
        appendLittleEndian(body, metadata_.sampleAspectRatioDenominator);
        appendMetadataName(body, metadata_.fieldOrder);
    }
    std::vector<std::uint8_t> checksum;
    appendLittleEndian(checksum, crc32Bytes(body));

    writeBytes(output_, kMagic, "frame stream magic");
    writeBytes(output_, body, "frame stream header");
    writeBytes(output_, checksum, "frame stream header checksum");
}

void FrameStreamWriter::writeFrame(std::span<const std::uint8_t> pixels,
                                   std::int64_t timestamp,
                                   FrameDisplayState displayState) {
    if (finished_) {
        throw std::logic_error("cannot write a frame after finishing the frame stream");
    }
    if (frameCount_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("frame stream frame count overflow");
    }
    const bool nativeVideo = !metadata_.pixelFormat.empty();
    if (!nativeVideo &&
        (displayState.interlaced || displayState.topFieldFirst ||
         displayState.repeatPicture != 0)) {
        throw std::invalid_argument("VFS3 packed-byte streams cannot store display state");
    }
    if (displayState.repeatPicture < -1 || displayState.repeatPicture > 4) {
        throw std::invalid_argument("VFS4 repeat-picture value is outside the supported range");
    }

    EncodedFrame packet = encodeLosslessFrame(pixels, metadata_.layout,
                                              previousFrame_, compressionLevel_,
                                              compressionCodec_);
    if (packet.payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw std::length_error("encoded frame payload is too large for the stream format");
    }

    std::vector<std::uint8_t> header;
    header.reserve(kFrameRecordHeaderBytes +
                   (nativeVideo ? kFrameDisplayStateBytes : 0U));
    header.push_back(kFrameRecord);
    header.push_back(packet.codec == CompressionCodec::Raw ? 0U : kCompressedFlag);
    header.push_back(static_cast<std::uint8_t>(packet.prediction));
    header.push_back(static_cast<std::uint8_t>(packet.codec));
    appendLittleEndian(header, frameCount_);
    appendSigned64(header, timestamp);
    appendLittleEndian(header, static_cast<std::uint64_t>(packet.payload.size()));
    if (nativeVideo) {
        const std::uint8_t flags =
            static_cast<std::uint8_t>((displayState.interlaced ? 1U : 0U) |
                                      (displayState.topFieldFirst ? 2U : 0U));
        header.push_back(flags);
        appendLittleEndian(header, std::bit_cast<std::uint32_t>(
                                      displayState.repeatPicture));
    }
    const std::uint32_t checksum = crc32Parts(header, packet.payload);
    const auto checksumBytes = crcBytes(checksum);

    writeBytes(output_, header, "frame record header");
    writeBytes(output_, packet.payload, "frame record payload");
    writeBytes(output_, checksumBytes, "frame record checksum");
    previousFrame_.assign(pixels.begin(), pixels.end());
    ++frameCount_;
}

void FrameStreamWriter::finish() {
    if (finished_) {
        throw std::logic_error("frame stream has already been finished");
    }
    std::vector<std::uint8_t> footer;
    footer.reserve(kFooterHeaderBytes);
    footer.push_back(kFooterRecord);
    footer.insert(footer.end(), 3U, 0U);
    appendLittleEndian(footer, frameCount_);
    const auto checksum = crcBytes(crc32Bytes(footer));
    writeBytes(output_, footer, "frame stream footer");
    writeBytes(output_, checksum, "frame stream footer checksum");
    finished_ = true;
}

FrameStreamReader::FrameStreamReader(std::istream& input,
                                     std::uint64_t maxDecodedFrameBytes)
    : input_(input) {
    std::array<std::uint8_t, kMagic.size()> magic{};
    readExact(input_, magic, "file signature");
    if (magic != kMagic) {
        throw std::runtime_error("invalid frame stream signature");
    }

    std::array<std::uint8_t, 4> prefix{};
    readExact(input_, prefix, "file header version and flags");
    std::array<std::uint8_t, kHeaderBodyBytes - prefix.size()> remainder{};
    readExact(input_, remainder, "file header geometry");
    std::vector<std::uint8_t> body;
    body.reserve(kHeaderBodyBytes + 4U + kMaxPixelFormatNameBytes +
                 kMaxPlaneCount * 12U + 256U);
    body.insert(body.end(), prefix.begin(), prefix.end());
    body.insert(body.end(), remainder.begin(), remainder.end());

    const std::span<const std::uint8_t> baseView(body);
    const std::uint16_t version = loadLittleEndian<std::uint16_t>(baseView, 0);
    const std::uint16_t flags = loadLittleEndian<std::uint16_t>(baseView, 2);
    bool nativeVideo = false;
    if (version == kPackedVersion) {
        if (flags != 0U) {
            throw std::runtime_error("VFS1 header has unsupported flags");
        }
    } else if (version == kNativeVersion) {
        if (flags != kNativeMetadataFlag) {
            throw std::runtime_error("VFS2 header has unsupported flags");
        }
        nativeVideo = true;
    } else if (version == kCodecVersion) {
        if (flags != 0U && flags != kNativeMetadataFlag) {
            throw std::runtime_error("VFS3 header has unsupported flags");
        }
        nativeVideo = flags == kNativeMetadataFlag;
    } else if (version == kDisplayMetadataVersion ||
               version == kAudioExtensionVersion) {
        if (flags != kNativeMetadataFlag) {
            throw std::runtime_error("VFS4/VFS5 header has unsupported flags");
        }
        nativeVideo = true;
    } else {
        throw std::runtime_error("unsupported frame stream version " + std::to_string(version));
    }
    if (nativeVideo) {
        std::array<std::uint8_t, 4> extension{};
        readExact(input_, extension, "native video metadata lengths");
        body.insert(body.end(), extension.begin(), extension.end());
        const std::span<const std::uint8_t> extensionView(extension);
        const std::uint16_t formatLength =
            loadLittleEndian<std::uint16_t>(extensionView, 0);
        const std::uint16_t planeCount =
            loadLittleEndian<std::uint16_t>(extensionView, 2);
        if (formatLength == 0U || formatLength > kMaxPixelFormatNameBytes ||
            planeCount == 0U || planeCount > kMaxPlaneCount) {
            throw std::runtime_error("native video header has invalid pixel-format or plane metadata lengths");
        }
        std::vector<std::uint8_t> formatBytes(formatLength);
        readExact(input_, formatBytes, "native pixel-format name");
        body.insert(body.end(), formatBytes.begin(), formatBytes.end());
        metadata_.pixelFormat.assign(formatBytes.begin(), formatBytes.end());
        if (!validPixelFormatName(metadata_.pixelFormat)) {
            throw std::runtime_error("native video header has an invalid pixel-format name");
        }
        const std::size_t planeBytes = static_cast<std::size_t>(planeCount) * 12U;
        std::vector<std::uint8_t> descriptors(planeBytes);
        readExact(input_, descriptors, "native plane descriptors");
        body.insert(body.end(), descriptors.begin(), descriptors.end());
        const std::span<const std::uint8_t> descriptorView(descriptors);
        for (std::size_t index = 0; index < planeCount; ++index) {
            const std::size_t offset = index * 12U;
            metadata_.layout.planes.push_back(FramePlaneLayout{
                loadLittleEndian<std::uint32_t>(descriptorView, offset),
                loadLittleEndian<std::uint32_t>(descriptorView, offset + 4U),
                loadLittleEndian<std::uint32_t>(descriptorView, offset + 8U)});
        }
        if (version >= kDisplayMetadataVersion) {
            readMetadataName(input_, body, metadata_.colorRange,
                             "VFS4 color range");
            readMetadataName(input_, body, metadata_.colorPrimaries,
                             "VFS4 color primaries");
            readMetadataName(input_, body, metadata_.colorTransfer,
                             "VFS4 transfer characteristic");
            readMetadataName(input_, body, metadata_.colorSpace,
                             "VFS4 matrix coefficients");
            readMetadataName(input_, body, metadata_.chromaLocation,
                             "VFS4 chroma location");
            std::array<std::uint8_t, 8> aspectRatio{};
            readExact(input_, aspectRatio, "VFS4 sample aspect ratio");
            body.insert(body.end(), aspectRatio.begin(), aspectRatio.end());
            const std::span<const std::uint8_t> ratioView(aspectRatio);
            metadata_.sampleAspectRatioNumerator =
                loadLittleEndian<std::uint32_t>(ratioView, 0U);
            metadata_.sampleAspectRatioDenominator =
                loadLittleEndian<std::uint32_t>(ratioView, 4U);
            if (metadata_.sampleAspectRatioNumerator == 0U ||
                metadata_.sampleAspectRatioDenominator == 0U) {
                throw std::runtime_error("VFS4 header has an invalid sample aspect ratio");
            }
            readMetadataName(input_, body, metadata_.fieldOrder,
                             "VFS4 field order");
        }
    }

    std::array<std::uint8_t, 4> storedChecksum{};
    readExact(input_, storedChecksum, "file header checksum");
    if (crc32Bytes(body) != loadCrc(storedChecksum)) {
        throw std::runtime_error("frame stream header checksum mismatch");
    }
    const std::span<const std::uint8_t> bodyView(body);
    metadata_.layout.width = loadLittleEndian<std::uint32_t>(bodyView, 4);
    metadata_.layout.height = loadLittleEndian<std::uint32_t>(bodyView, 8);
    metadata_.layout.channels = loadLittleEndian<std::uint32_t>(bodyView, 12);
    metadata_.timeBaseNumerator = loadLittleEndian<std::uint32_t>(bodyView, 16);
    metadata_.timeBaseDenominator = loadLittleEndian<std::uint32_t>(bodyView, 20);
    if (nativeVideo != !metadata_.pixelFormat.empty()) {
        throw std::runtime_error("frame stream version and video metadata disagree");
    }
    formatVersion_ = version;
    if (metadata_.timeBaseNumerator == 0U || metadata_.timeBaseDenominator == 0U) {
        throw std::runtime_error("frame stream header has an invalid timebase");
    }
    if (nativeVideo && metadata_.layout.channels == 0U) {
        throw std::runtime_error("native video header has an invalid channel marker");
    }
    const std::size_t decodedSize = checkedFrameBytes(metadata_.layout);
    if (static_cast<std::uint64_t>(decodedSize) > maxDecodedFrameBytes) {
        throw std::length_error("frame stream decoded frame exceeds the configured size limit");
    }
    decodedFrameBytes_ = decodedSize;
}

bool FrameStreamReader::readNext(DecodedStreamFrame& frame) {
    if (finished_) {
        return false;
    }

    const int first = input_.get();
    if (first == std::char_traits<char>::eof()) {
        throw std::runtime_error("frame stream ended before its footer");
    }
    const std::uint8_t recordType = static_cast<std::uint8_t>(first);
    std::array<std::uint8_t, 3> control{};
    readExact(input_, control, "record control fields");

    if (recordType == kFooterRecord) {
        if (control != std::array<std::uint8_t, 3>{0U, 0U, 0U}) {
            throw std::runtime_error("frame stream footer has nonzero reserved fields");
        }
        std::array<std::uint8_t, 8> countBytes{};
        std::array<std::uint8_t, 4> checksumBytes{};
        readExact(input_, countBytes, "footer frame count");
        readExact(input_, checksumBytes, "footer checksum");
        std::vector<std::uint8_t> footer{recordType, control[0], control[1], control[2]};
        footer.insert(footer.end(), countBytes.begin(), countBytes.end());
        if (crc32Bytes(footer) != loadCrc(checksumBytes)) {
            throw std::runtime_error("frame stream footer checksum mismatch");
        }
        const std::uint64_t declaredCount =
            loadLittleEndian<std::uint64_t>(countBytes, 0);
        if (declaredCount != frameCount_) {
            throw std::runtime_error("frame stream footer count does not match decoded records");
        }
        if (formatVersion_ != kAudioExtensionVersion &&
            input_.peek() != std::char_traits<char>::eof()) {
            throw std::runtime_error("frame stream contains trailing bytes after its footer");
        }
        finished_ = true;
        return false;
    }

    if (recordType != kFrameRecord) {
        throw std::runtime_error("frame stream contains an unknown record type");
    }
    if ((control[0] & static_cast<std::uint8_t>(~kCompressedFlag)) != 0U ||
        (formatVersion_ != kCodecVersion &&
         formatVersion_ != kDisplayMetadataVersion &&
         formatVersion_ != kAudioExtensionVersion && control[2] != 0U)) {
        throw std::runtime_error("frame stream frame record has unsupported flags");
    }
    CompressionCodec codec = CompressionCodec::Raw;
    const bool compressed = (control[0] & kCompressedFlag) != 0U;
    if (formatVersion_ == kCodecVersion ||
        formatVersion_ == kDisplayMetadataVersion ||
        formatVersion_ == kAudioExtensionVersion) {
        if (control[2] > static_cast<std::uint8_t>(CompressionCodec::Zstandard)) {
            throw std::runtime_error("VFS3 frame record uses an unknown compression codec");
        }
        codec = static_cast<CompressionCodec>(control[2]);
        if (compressed != (codec != CompressionCodec::Raw)) {
            throw std::runtime_error("VFS3 frame codec and compressed flag disagree");
        }
    } else {
        codec = compressed ? CompressionCodec::Deflate : CompressionCodec::Raw;
    }

    std::array<std::uint8_t, 24> numericFields{};
    readExact(input_, numericFields, "frame record header");
    std::vector<std::uint8_t> header;
    header.reserve(kFrameRecordHeaderBytes +
                   (formatVersion_ >= kDisplayMetadataVersion
                        ? kFrameDisplayStateBytes : 0U));
    header.push_back(recordType);
    header.insert(header.end(), control.begin(), control.end());
    header.insert(header.end(), numericFields.begin(), numericFields.end());
    const std::span<const std::uint8_t> numericView(numericFields);
    const std::uint64_t index = loadLittleEndian<std::uint64_t>(numericView, 0);
    const std::uint64_t timestampBits = loadLittleEndian<std::uint64_t>(numericView, 8);
    const std::int64_t timestamp = std::bit_cast<std::int64_t>(timestampBits);
    const std::uint64_t payloadLength = loadLittleEndian<std::uint64_t>(numericView, 16);
    FrameDisplayState displayState;
    if (formatVersion_ >= kDisplayMetadataVersion) {
        std::array<std::uint8_t, kFrameDisplayStateBytes> serializedState{};
        readExact(input_, serializedState, "VFS4 frame display state");
        header.insert(header.end(), serializedState.begin(), serializedState.end());
        if ((serializedState[0] & 0xfcU) != 0U) {
            throw std::runtime_error("VFS4 frame has unsupported display-state flags");
        }
        displayState.interlaced = (serializedState[0] & 1U) != 0U;
        displayState.topFieldFirst = (serializedState[0] & 2U) != 0U;
        const std::span<const std::uint8_t> stateView(serializedState);
        displayState.repeatPicture = std::bit_cast<std::int32_t>(
            loadLittleEndian<std::uint32_t>(stateView, 1U));
        if (displayState.repeatPicture < -1 || displayState.repeatPicture > 4) {
            throw std::runtime_error("VFS4 frame has an unsupported repeat-picture value");
        }
    }
    if (index != frameCount_) {
        throw std::runtime_error("frame stream record index is not contiguous");
    }
    if (payloadLength == 0U || payloadLength > static_cast<std::uint64_t>(decodedFrameBytes_) ||
        payloadLength > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::length_error("frame stream payload length is invalid or exceeds the frame size");
    }
    if (payloadLength > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("frame stream payload exceeds stream read limits");
    }

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payloadLength));
    readExact(input_, payload, "frame payload");
    std::array<std::uint8_t, 4> checksumBytes{};
    readExact(input_, checksumBytes, "frame checksum");
    requireCrc(loadCrc(checksumBytes), header, payload, "frame record");

    EncodedFrame packet;
    packet.layout = metadata_.layout;
    packet.prediction = static_cast<PredictionMode>(control[1]);
    packet.codec = codec;
    packet.decodedSize = static_cast<std::uint64_t>(decodedFrameBytes_);
    packet.payload = std::move(payload);
    std::vector<std::uint8_t> pixels = decodeLosslessFrame(packet, previousFrame_);
    previousFrame_ = pixels;
    frame.index = index;
    frame.timestamp = timestamp;
    frame.pixels = std::move(pixels);
    frame.displayState = displayState;
    if (frameCount_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("frame stream frame count overflow");
    }
    ++frameCount_;
    return true;
}

}  // namespace frameforge
