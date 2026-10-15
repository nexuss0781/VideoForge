#include "frameforge/video_archive.hpp"

#include "frameforge/frame_stream.hpp"
#include "frameforge/audio_remux.hpp"
#include "frameforge/video_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <zlib.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace frameforge {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<std::uint8_t, 8> kSourceFileMagic{
    'V', 'F', 'S', 'R', 'A', 'W', '\r', '\n'};
constexpr std::uint16_t kSourceFileVersion = 1U;
constexpr std::uint64_t kSourceFileOverhead = 28U;
constexpr std::size_t kSourceFilePrefixBytes = 24U;
constexpr long double kMinimumSourceToPlaneRatio = 0.006L;

template <typename Unsigned>
void appendLittleEndian(std::vector<std::uint8_t>& output, Unsigned value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        output.push_back(static_cast<std::uint8_t>(value & static_cast<Unsigned>(0xffU)));
        value >>= 8U;
    }
}

template <typename Unsigned, std::size_t N>
Unsigned loadLittleEndian(const std::array<std::uint8_t, N>& input,
                          std::size_t offset) {
    static_assert(std::is_unsigned_v<Unsigned>);
    std::uint64_t value = 0U;
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        value |= static_cast<std::uint64_t>(input[offset + index]) << (index * 8U);
    }
    return static_cast<Unsigned>(value);
}

std::uint32_t crc32Bytes(const std::uint8_t* bytes, std::size_t size) {
    const uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(bytes),
                            static_cast<uInt>(size));
    return static_cast<std::uint32_t>(crc);
}

void appendCrc(std::vector<std::uint8_t>& bytes, std::uint32_t crc) {
    appendLittleEndian(bytes, crc);
}

std::uint64_t requiredFileSize(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error("cannot measure file '" + path.string() + "': " +
                                 error.message());
    }
    if (size > std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("file size exceeds archive limits: " + path.string());
    }
    return static_cast<std::uint64_t>(size);
}

void writeExact(std::ostream& output, const std::uint8_t* bytes,
                std::size_t size, const char* description) {
    output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    if (!output) {
        throw std::runtime_error(std::string("failed writing ") + description);
    }
}

void readExact(std::istream& input, std::uint8_t* bytes,
               std::size_t size, const char* description) {
    input.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error(std::string("truncated source-file archive while reading ") +
                                 description);
    }
}

std::uint64_t writeSourceFileArchive(const std::filesystem::path& sourcePath,
                                     const std::filesystem::path& archivePath,
                                     std::uint64_t expectedSourceBytes) {
    if (expectedSourceBytes >
        std::numeric_limits<std::uint64_t>::max() - kSourceFileOverhead) {
        throw std::length_error("source video is too large for the source-file archive format");
    }
    std::vector<std::uint8_t> headerBody;
    headerBody.reserve(12U);
    appendLittleEndian(headerBody, kSourceFileVersion);
    appendLittleEndian(headerBody, static_cast<std::uint16_t>(0U));
    appendLittleEndian(headerBody, expectedSourceBytes);
    const std::uint32_t headerChecksum = crc32Bytes(headerBody.data(), headerBody.size());

    std::ofstream archive(archivePath, std::ios::binary | std::ios::trunc);
    if (!archive) {
        throw std::runtime_error("cannot create temporary source-file archive: " +
                                 archivePath.string());
    }
    writeExact(archive, kSourceFileMagic.data(), kSourceFileMagic.size(),
               "source-file archive signature");
    writeExact(archive, headerBody.data(), headerBody.size(),
               "source-file archive header");
    std::vector<std::uint8_t> headerChecksumBytes;
    appendCrc(headerChecksumBytes, headerChecksum);
    writeExact(archive, headerChecksumBytes.data(), headerChecksumBytes.size(),
               "source-file archive header checksum");

    std::ifstream source(sourcePath, std::ios::binary);
    if (!source) {
        throw std::runtime_error("cannot reopen source video: " + sourcePath.string());
    }
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    std::uint64_t copiedBytes = 0U;
    uLong payloadChecksum = crc32(0L, Z_NULL, 0);
    while (copiedBytes < expectedSourceBytes) {
        const std::uint64_t remaining = expectedSourceBytes - copiedBytes;
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        source.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(requested));
        if (source.gcount() != static_cast<std::streamsize>(requested)) {
            throw std::runtime_error("source video changed or ended while creating source-file archive");
        }
        writeExact(archive, buffer.data(), requested, "source-file archive payload");
        payloadChecksum = crc32(payloadChecksum,
            reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(requested));
        copiedBytes += static_cast<std::uint64_t>(requested);
    }
    char extraByte = 0;
    source.read(&extraByte, 1);
    if (source.gcount() != 0 || source.bad()) {
        throw std::runtime_error("source video changed while creating source-file archive");
    }
    const std::uint32_t payloadChecksumValue = static_cast<std::uint32_t>(payloadChecksum);
    std::vector<std::uint8_t> payloadChecksumBytes;
    appendCrc(payloadChecksumBytes, payloadChecksumValue);
    writeExact(archive, payloadChecksumBytes.data(), payloadChecksumBytes.size(),
               "source-file archive payload checksum");
    archive.close();
    if (!archive) {
        throw std::runtime_error("failed closing source-file archive");
    }
    const std::uint64_t expectedArchiveBytes = expectedSourceBytes + kSourceFileOverhead;
    if (requiredFileSize(archivePath) != expectedArchiveBytes) {
        throw std::runtime_error("source-file archive size differs from its declared length");
    }
    return expectedArchiveBytes;
}

bool hasSourceFileSignature(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open VFS archive: " + path.string());
    }
    std::array<std::uint8_t, kSourceFileMagic.size()> signature{};
    input.read(reinterpret_cast<char*>(signature.data()),
               static_cast<std::streamsize>(signature.size()));
    return input.gcount() == static_cast<std::streamsize>(signature.size()) &&
           signature == kSourceFileMagic;
}

std::uint64_t restoreSourceFileArchive(const std::filesystem::path& archivePath,
                                      const std::filesystem::path& outputPath) {
    std::ifstream archive(archivePath, std::ios::binary);
    if (!archive) {
        throw std::runtime_error("cannot open VFS archive: " + archivePath.string());
    }
    std::array<std::uint8_t, kSourceFilePrefixBytes> prefix{};
    readExact(archive, prefix.data(), prefix.size(), "source-file archive header");
    if (!std::equal(kSourceFileMagic.begin(), kSourceFileMagic.end(), prefix.begin())) {
        throw std::runtime_error("invalid source-file archive signature");
    }
    const std::uint16_t version = loadLittleEndian<std::uint16_t>(prefix, 8U);
    const std::uint16_t flags = loadLittleEndian<std::uint16_t>(prefix, 10U);
    const std::uint64_t sourceBytes = loadLittleEndian<std::uint64_t>(prefix, 12U);
    const std::uint32_t expectedHeaderChecksum =
        loadLittleEndian<std::uint32_t>(prefix, 20U);
    if (version != kSourceFileVersion || flags != 0U) {
        throw std::runtime_error("unsupported source-file archive version or flags");
    }
    if (crc32Bytes(prefix.data() + kSourceFileMagic.size(), 12U) !=
        expectedHeaderChecksum) {
        throw std::runtime_error("source-file archive header checksum mismatch");
    }
    if (sourceBytes > std::numeric_limits<std::uint64_t>::max() - kSourceFileOverhead ||
        requiredFileSize(archivePath) != sourceBytes + kSourceFileOverhead) {
        throw std::runtime_error("source-file archive length does not match its header");
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create temporary restored source file: " +
                                 outputPath.string());
    }
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    std::uint64_t restoredBytes = 0U;
    uLong payloadChecksum = crc32(0L, Z_NULL, 0);
    while (restoredBytes < sourceBytes) {
        const std::uint64_t remaining = sourceBytes - restoredBytes;
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        readExact(archive, buffer.data(), requested, "source-file payload");
        output.write(reinterpret_cast<const char*>(buffer.data()),
                     static_cast<std::streamsize>(requested));
        if (!output) {
            throw std::runtime_error("failed writing restored source file");
        }
        payloadChecksum = crc32(payloadChecksum,
            reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(requested));
        restoredBytes += static_cast<std::uint64_t>(requested);
    }
    std::array<std::uint8_t, 4U> checksumBytes{};
    readExact(archive, checksumBytes.data(), checksumBytes.size(),
              "source-file payload checksum");
    if (loadLittleEndian<std::uint32_t>(checksumBytes, 0U) !=
        static_cast<std::uint32_t>(payloadChecksum)) {
        throw std::runtime_error("source-file archive payload checksum mismatch");
    }
    char trailingByte = 0;
    archive.read(&trailingByte, 1);
    if (archive.gcount() != 0 || archive.bad()) {
        throw std::runtime_error("source-file archive contains trailing or unreadable data");
    }
    output.close();
    if (!output) {
        throw std::runtime_error("failed closing restored source file");
    }
    return sourceBytes;
}

[[noreturn]] void throwFfmpeg(const std::string& operation, int errorCode) {
    throw std::runtime_error(operation + ": " + ffmpegErrorString(errorCode));
}

std::uint64_t fileSizeOrZero(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > std::numeric_limits<std::uint64_t>::max()) {
        return 0U;
    }
    return static_cast<std::uint64_t>(size);
}

std::filesystem::path absoluteNormalized(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        throw std::runtime_error("cannot resolve path '" + path.string() + "': " +
                                 error.message());
    }
    return absolute.lexically_normal();
}

void requireNewOutput(const std::filesystem::path& input,
                      const std::filesystem::path& output) {
    if (output.empty()) {
        throw std::invalid_argument("output path must not be empty");
    }
    const std::filesystem::path normalizedOutput = absoluteNormalized(output);
    const std::filesystem::path normalizedInput = absoluteNormalized(input);
    if (normalizedOutput == normalizedInput) {
        throw std::invalid_argument("input and output paths must be different");
    }
    std::error_code error;
    if (std::filesystem::exists(output, error)) {
        throw std::runtime_error("output already exists; refusing to overwrite: " +
                                 output.string());
    }
    if (error) {
        throw std::runtime_error("cannot inspect output path '" + output.string() +
                                 "': " + error.message());
    }
}

std::filesystem::path makeTemporaryPath(const std::filesystem::path& output,
                                        bool preserveExtension) {
    if (!preserveExtension) {
        return output.string() + ".tmp";
    }
    const std::string extension = output.extension().string();
    const std::string stem = output.stem().string();
    return output.parent_path() / (stem + ".tmp" + extension);
}

void requireTemporaryPathAvailable(const std::filesystem::path& temporary) {
    std::error_code error;
    if (std::filesystem::exists(temporary, error)) {
        throw std::runtime_error("temporary output already exists; refusing to overwrite: " +
                                 temporary.string());
    }
    if (error) {
        throw std::runtime_error("cannot inspect temporary output path '" +
                                 temporary.string() + "': " + error.message());
    }
}

void commitOutput(const std::filesystem::path& temporary,
                  const std::filesystem::path& output) {
    std::error_code error;
    std::filesystem::create_hard_link(temporary, output, error);
    if (error) {
        throw std::runtime_error("cannot finalize output '" + output.string() +
                                 "': " + error.message());
    }
    std::filesystem::remove(temporary, error);
}

FrameLayout nativeFrameLayout(int width, int height, AVPixelFormat pixelFormat) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("input video has invalid frame dimensions");
    }
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixelFormat);
    if (descriptor == nullptr ||
        (descriptor->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM |
                              AV_PIX_FMT_FLAG_PAL)) != 0) {
        throw std::runtime_error("decoded pixel format cannot be represented as ordinary software planes");
    }

    std::array<int, 4> packedLineSizes{};
    const int linesizeResult = av_image_fill_linesizes(packedLineSizes.data(),
                                                       pixelFormat, width);
    if (linesizeResult < 0) {
        throwFfmpeg("cannot calculate decoded pixel plane row sizes", linesizeResult);
    }
    std::array<std::ptrdiff_t, 4> lineSizes{};
    std::transform(packedLineSizes.begin(), packedLineSizes.end(), lineSizes.begin(),
                   [](int value) { return static_cast<std::ptrdiff_t>(value); });
    std::array<std::size_t, 4> planeSizes{};
    const int sizesResult = av_image_fill_plane_sizes(planeSizes.data(), pixelFormat,
                                                       height, lineSizes.data());
    if (sizesResult < 0) {
        throwFfmpeg("cannot calculate decoded pixel plane sizes", sizesResult);
    }

    int planeCount = 0;
    for (int component = 0; component < descriptor->nb_components; ++component) {
        planeCount = std::max(planeCount, descriptor->comp[component].plane + 1);
    }
    if (planeCount <= 0 || planeCount > 4) {
        throw std::runtime_error("decoded pixel format has an unsupported plane count");
    }

    FrameLayout layout{static_cast<std::uint32_t>(width),
                       static_cast<std::uint32_t>(height), 1U, {}};
    layout.planes.reserve(static_cast<std::size_t>(planeCount));
    for (int plane = 0; plane < planeCount; ++plane) {
        const int rowBytes = packedLineSizes[static_cast<std::size_t>(plane)];
        const std::size_t planeBytes = planeSizes[static_cast<std::size_t>(plane)];
        if (rowBytes <= 0 || planeBytes == 0U ||
            planeBytes % static_cast<std::size_t>(rowBytes) != 0U) {
            throw std::runtime_error("decoded pixel format has invalid plane geometry");
        }
        const std::size_t rows = planeBytes / static_cast<std::size_t>(rowBytes);
        if (static_cast<std::uint64_t>(rowBytes) >
                std::numeric_limits<std::uint32_t>::max() ||
            rows == 0U || rows > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("decoded pixel plane dimensions exceed VFS limits");
        }
        layout.planes.push_back(FramePlaneLayout{
            static_cast<std::uint32_t>(rowBytes), static_cast<std::uint32_t>(rows), 1U});
    }
    return layout;
}

std::size_t packedFrameBytes(const FrameLayout& layout) {
    std::size_t bytes = 0U;
    for (const FramePlaneLayout& plane : layout.planes) {
        const std::size_t rowBytes = plane.rowBytes;
        const std::size_t rows = plane.rows;
        if (rowBytes == 0U || rows == 0U ||
            rowBytes > std::numeric_limits<std::size_t>::max() / rows ||
            rowBytes * rows > std::numeric_limits<std::size_t>::max() - bytes) {
            throw std::length_error("native decoded frame size overflows addressable memory");
        }
        bytes += rowBytes * rows;
    }
    return bytes;
}

bool hasConsistentFrameCountHint(const VideoMetadata& metadata) noexcept {
    if (metadata.containerFrameHint == 0U ||
        !std::isfinite(metadata.durationSeconds) || metadata.durationSeconds <= 0.0 ||
        !std::isfinite(metadata.fps) || metadata.fps <= 0.0) {
        return false;
    }
    const long double durationFrames =
        static_cast<long double>(metadata.durationSeconds) *
        static_cast<long double>(metadata.fps);
    if (!std::isfinite(durationFrames) || durationFrames <= 0.0L) {
        return false;
    }
    const long double difference = std::fabs(
        durationFrames - static_cast<long double>(metadata.containerFrameHint));
    const long double tolerance = std::max(
        1.0L, static_cast<long double>(metadata.containerFrameHint) * 0.01L);
    return difference <= tolerance;
}

bool isCommonLossyInterFrameCodec(std::string_view codec) noexcept {
    constexpr std::array<std::string_view, 8> supported{
        "h264", "hevc", "av1", "vp8", "vp9", "mpeg1video", "mpeg2video", "mpeg4"};
    return std::find(supported.begin(), supported.end(), codec) != supported.end();
}

std::string sourcePreservationFastPathReason(const VideoMetadata& metadata,
                                             std::uint64_t inputBytes) {
    if (metadata.audioStreamCount != 0U || inputBytes == 0U ||
        inputBytes > std::numeric_limits<std::uint64_t>::max() - kSourceFileOverhead ||
        metadata.containerFrameHint == 0U) {
        return {};
    }
    if (!isCommonLossyInterFrameCodec(metadata.codec) ||
        !hasConsistentFrameCountHint(metadata) || metadata.width <= 0 ||
        metadata.height <= 0 || metadata.pixelFormat == "unknown") {
        return {};
    }

    try {
        const AVPixelFormat pixelFormat = av_get_pix_fmt(metadata.pixelFormat.c_str());
        if (pixelFormat == AV_PIX_FMT_NONE) {
            return {};
        }
        const std::uint64_t bytesPerFrame = static_cast<std::uint64_t>(
            packedFrameBytes(nativeFrameLayout(metadata.width, metadata.height,
                                               pixelFormat)));
        if (bytesPerFrame == 0U ||
            bytesPerFrame > std::numeric_limits<std::uint64_t>::max() /
                                metadata.containerFrameHint) {
            return {};
        }
        const std::uint64_t decodedPlaneBytes =
            bytesPerFrame * metadata.containerFrameHint;
        if (static_cast<long double>(inputBytes) <
            static_cast<long double>(decodedPlaneBytes) *
                kMinimumSourceToPlaneRatio) {
            return {};
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        // An uncertain estimate must fall back to the existing measured path.
        return {};
    }
    return "source-density-fast-path";
}

void appendFileBytes(const std::filesystem::path& sourcePath,
                     const std::filesystem::path& destinationPath) {
    std::ifstream source(sourcePath, std::ios::binary);
    if (!source) {
        throw std::runtime_error("cannot open temporary VFS audio extension: " +
                                 sourcePath.string());
    }
    std::ofstream destination(destinationPath, std::ios::binary | std::ios::app);
    if (!destination) {
        throw std::runtime_error("cannot append VFS audio extension to archive: " +
                                 destinationPath.string());
    }
    std::array<char, 64U * 1024U> buffer{};
    while (source) {
        source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = source.gcount();
        if (count > 0) {
            destination.write(buffer.data(), count);
            if (!destination) {
                throw std::runtime_error("failed appending VFS audio extension");
            }
        }
    }
    if (source.bad()) {
        throw std::runtime_error("failed reading temporary VFS audio extension");
    }
    destination.close();
    if (!destination) {
        throw std::runtime_error("failed closing VFS archive after appending audio");
    }
}

bool videoMetadataSupportsNutAudio(const VideoMetadata& metadata) noexcept {
    return metadata.colorRange.empty() && metadata.colorPrimaries.empty() &&
           metadata.colorTransfer.empty() && metadata.colorSpace.empty() &&
           metadata.chromaLocation.empty() &&
           (metadata.fieldOrder.empty() || metadata.fieldOrder == "progressive");
}

std::vector<std::uint8_t> packNativeFrame(const AVFrame* frame,
                                          const FrameLayout& layout) {
    if (frame == nullptr) {
        throw std::invalid_argument("cannot pack a null decoded frame");
    }
    std::vector<std::uint8_t> packed(packedFrameBytes(layout));
    std::size_t offset = 0U;
    for (std::size_t plane = 0; plane < layout.planes.size(); ++plane) {
        const FramePlaneLayout& planeLayout = layout.planes[plane];
        const int sourceStride = frame->linesize[plane];
        const std::int64_t strideMagnitude = sourceStride < 0
            ? -static_cast<std::int64_t>(sourceStride)
            : static_cast<std::int64_t>(sourceStride);
        if (frame->data[plane] == nullptr ||
            strideMagnitude < static_cast<std::int64_t>(planeLayout.rowBytes)) {
            throw std::runtime_error("decoded frame plane has a null pointer or undersized stride");
        }
        for (std::size_t row = 0; row < planeLayout.rows; ++row) {
            const auto rowOffset = static_cast<std::ptrdiff_t>(row) * sourceStride;
            const std::uint8_t* source = frame->data[plane] + rowOffset;
            std::copy_n(source, planeLayout.rowBytes,
                        packed.data() + offset + row * planeLayout.rowBytes);
        }
        offset += static_cast<std::size_t>(planeLayout.rowBytes) * planeLayout.rows;
    }
    return packed;
}

FrameStreamMetadata streamMetadataFor(const VideoMetadata& source,
                                     AVPixelFormat pixelFormat) {
    const FrameLayout layout = nativeFrameLayout(source.width, source.height, pixelFormat);
    const char* pixelName = av_get_pix_fmt_name(pixelFormat);
    if (pixelName == nullptr) {
        throw std::runtime_error("decoded pixel format has no stable FFmpeg name");
    }
    FrameStreamMetadata metadata{
        layout,
        source.timeBase.num > 0 && source.timeBase.den > 0
            ? static_cast<std::uint32_t>(source.timeBase.num) : 1U,
        source.timeBase.num > 0 && source.timeBase.den > 0
            ? static_cast<std::uint32_t>(source.timeBase.den) : 1000000U,
        pixelName};
    metadata.colorRange = source.colorRange;
    metadata.colorPrimaries = source.colorPrimaries;
    metadata.colorTransfer = source.colorTransfer;
    metadata.colorSpace = source.colorSpace;
    metadata.chromaLocation = source.chromaLocation;
    metadata.sampleAspectRatioNumerator = source.sampleAspectRatioNumerator;
    metadata.sampleAspectRatioDenominator = source.sampleAspectRatioDenominator;
    metadata.fieldOrder = source.fieldOrder;
    return metadata;
}

std::int64_t frameTimestamp(const AVFrame* frame, const VideoMetadata& metadata,
                            std::uint64_t fallbackIndex,
                            const FrameStreamMetadata& archiveMetadata) {
    if (frame != nullptr && frame->best_effort_timestamp != AV_NOPTS_VALUE &&
        metadata.timeBase.num > 0 && metadata.timeBase.den > 0 &&
        static_cast<std::uint32_t>(metadata.timeBase.num) ==
            archiveMetadata.timeBaseNumerator &&
        static_cast<std::uint32_t>(metadata.timeBase.den) ==
            archiveMetadata.timeBaseDenominator) {
        return frame->best_effort_timestamp;
    }
    long double timestamp = static_cast<long double>(fallbackIndex);
    if (metadata.fps > 0.0 && std::isfinite(metadata.fps)) {
        timestamp = timestamp * static_cast<long double>(archiveMetadata.timeBaseDenominator) /
                    (static_cast<long double>(archiveMetadata.timeBaseNumerator) *
                     static_cast<long double>(metadata.fps));
    }
    if (!std::isfinite(timestamp) ||
        timestamp > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        timestamp < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        throw std::overflow_error("computed video frame timestamp exceeds VFS limits");
    }
    return static_cast<std::int64_t>(std::llround(timestamp));
}

bool colorValueMatches(int value, const std::string& expected, int unspecified,
                       int (*parseName)(const char*)) {
    if (value == unspecified) {
        return true;
    }
    return !expected.empty() && parseName(expected.c_str()) == value;
}

bool frameHasUnpreservedDisplayMetadata(const AVFrame* frame,
                                        const FrameStreamMetadata& metadata) {
    if (frame == nullptr) {
        return true;
    }
    const AVRational expectedAspectRatio{
        static_cast<int>(metadata.sampleAspectRatioNumerator),
        static_cast<int>(metadata.sampleAspectRatioDenominator)};
    const bool aspectRatioMismatch =
        frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0 &&
        static_cast<std::int64_t>(frame->sample_aspect_ratio.num) *
                expectedAspectRatio.den !=
            static_cast<std::int64_t>(expectedAspectRatio.num) *
                frame->sample_aspect_ratio.den;
    const bool metadataMismatch =
        !colorValueMatches(frame->color_range, metadata.colorRange,
                           AVCOL_RANGE_UNSPECIFIED, av_color_range_from_name) ||
        !colorValueMatches(frame->color_primaries, metadata.colorPrimaries,
                           AVCOL_PRI_UNSPECIFIED, av_color_primaries_from_name) ||
        !colorValueMatches(frame->color_trc, metadata.colorTransfer,
                           AVCOL_TRC_UNSPECIFIED, av_color_transfer_from_name) ||
        !colorValueMatches(frame->colorspace, metadata.colorSpace,
                           AVCOL_SPC_UNSPECIFIED, av_color_space_from_name) ||
        !colorValueMatches(frame->chroma_location, metadata.chromaLocation,
                           AVCHROMA_LOC_UNSPECIFIED,
                           av_chroma_location_from_name);
    const bool unsupportedRepeatPicture = frame->repeat_pict < -1 ||
                                          frame->repeat_pict > 4;
    return frame->nb_side_data != 0 || av_dict_count(frame->metadata) != 0 ||
           metadataMismatch || aspectRatioMismatch || unsupportedRepeatPicture ||
           frame->crop_top != 0U || frame->crop_bottom != 0U ||
           frame->crop_left != 0U || frame->crop_right != 0U;
}

VideoArchiveStats finishStats(std::uint64_t frameCount, std::uint32_t width,
                              std::uint32_t height, std::string pixelFormat,
                              std::uint64_t inputBytes, std::uint64_t outputBytes,
                              Clock::time_point start) {
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const double safeElapsed = std::max(elapsed, 1.0e-9);
    const double fps = static_cast<double>(frameCount) / safeElapsed;
    const double ratio = inputBytes == 0U
        ? 0.0
        : static_cast<double>(outputBytes) / static_cast<double>(inputBytes);
    return VideoArchiveStats{frameCount, width, height, std::move(pixelFormat),
                             inputBytes, outputBytes, elapsed, fps, ratio};
}

template <typename Enum>
Enum parseColorName(const std::string& name, int (*parse)(const char*),
                    Enum unspecified, const char* description) {
    if (name.empty()) {
        return unspecified;
    }
    const int value = parse(name.c_str());
    if (value < 0) {
        throw std::runtime_error(std::string("VFS4 archive has an unsupported ") +
                                 description + " name");
    }
    return static_cast<Enum>(value);
}

AVColorRange colorRangeFromName(const std::string& name) {
    return parseColorName(name, av_color_range_from_name,
                          AVCOL_RANGE_UNSPECIFIED, "color range");
}

AVColorPrimaries colorPrimariesFromName(const std::string& name) {
    return parseColorName(name, av_color_primaries_from_name,
                          AVCOL_PRI_UNSPECIFIED, "color primaries");
}

AVColorTransferCharacteristic colorTransferFromName(const std::string& name) {
    return parseColorName(name, av_color_transfer_from_name,
                          AVCOL_TRC_UNSPECIFIED, "transfer characteristic");
}

AVColorSpace colorSpaceFromName(const std::string& name) {
    return parseColorName(name, av_color_space_from_name,
                          AVCOL_SPC_UNSPECIFIED, "matrix coefficients");
}

AVChromaLocation chromaLocationFromName(const std::string& name) {
    return parseColorName(name, av_chroma_location_from_name,
                          AVCHROMA_LOC_UNSPECIFIED, "chroma location");
}

AVFieldOrder fieldOrderFromName(const std::string& name) {
    if (name.empty()) return AV_FIELD_UNKNOWN;
    if (name == "progressive") return AV_FIELD_PROGRESSIVE;
    if (name == "tt") return AV_FIELD_TT;
    if (name == "bb") return AV_FIELD_BB;
    if (name == "tb") return AV_FIELD_TB;
    if (name == "bt") return AV_FIELD_BT;
    throw std::runtime_error("VFS4 archive has an unsupported field-order name");
}

class LosslessNutWriter final {
public:
    LosslessNutWriter(const std::filesystem::path& path,
                      FrameStreamMetadata metadata,
                      bool legacyRgb24Conversion = false)
        : metadata_(std::move(metadata)),
          legacyRgb24Conversion_(legacyRgb24Conversion) {
        try {
            initialize(path);
        } catch (...) {
            release();
            throw;
        }
    }

    ~LosslessNutWriter() { release(); }

    LosslessNutWriter(const LosslessNutWriter&) = delete;
    LosslessNutWriter& operator=(const LosslessNutWriter&) = delete;

    void writeFrame(const DecodedStreamFrame& source) {
        if (finished_) {
            throw std::logic_error("cannot write a frame after finishing the video output");
        }
        const std::size_t expected = packedFrameBytes(metadata_.layout);
        if (source.pixels.size() != expected) {
            throw std::runtime_error("decoded VFS frame has an invalid native plane byte count");
        }

        int result = av_frame_make_writable(frame_);
        if (result < 0) {
            throwFfmpeg("cannot prepare lossless output frame", result);
        }
        if (legacyRgb24Conversion_ && metadata_.pixelFormat == "rgb24") {
            const std::size_t width = metadata_.layout.width;
            const std::size_t sourceRowBytes = width * 3U;
            const std::size_t destinationRowBytes = width * 4U;
            const int destinationStride = frame_->linesize[0];
            const std::int64_t strideMagnitude = destinationStride < 0
                ? -static_cast<std::int64_t>(destinationStride)
                : static_cast<std::int64_t>(destinationStride);
            if (frame_->data[0] == nullptr ||
                strideMagnitude < static_cast<std::int64_t>(destinationRowBytes)) {
                throw std::runtime_error("FFV1 BGR0 output plane has an undersized stride");
            }
            for (std::size_t row = 0; row < metadata_.layout.height; ++row) {
                const std::uint8_t* sourceRow = source.pixels.data() + row * sourceRowBytes;
                std::uint8_t* destinationRow = frame_->data[0] +
                    static_cast<std::ptrdiff_t>(row) * destinationStride;
                for (std::size_t column = 0; column < width; ++column) {
                    destinationRow[column * 4U] = sourceRow[column * 3U + 2U];
                    destinationRow[column * 4U + 1U] = sourceRow[column * 3U + 1U];
                    destinationRow[column * 4U + 2U] = sourceRow[column * 3U];
                    destinationRow[column * 4U + 3U] = 0U;
                }
            }
        } else {
            std::size_t offset = 0U;
            for (std::size_t plane = 0; plane < metadata_.layout.planes.size(); ++plane) {
                const FramePlaneLayout& planeLayout = metadata_.layout.planes[plane];
                const int destinationStride = frame_->linesize[plane];
                const std::int64_t strideMagnitude = destinationStride < 0
                    ? -static_cast<std::int64_t>(destinationStride)
                    : static_cast<std::int64_t>(destinationStride);
                if (frame_->data[plane] == nullptr ||
                    strideMagnitude < static_cast<std::int64_t>(planeLayout.rowBytes)) {
                    throw std::runtime_error("lossless output plane has a null pointer or undersized stride");
                }
                for (std::size_t row = 0; row < planeLayout.rows; ++row) {
                    const auto rowOffset = static_cast<std::ptrdiff_t>(row) * destinationStride;
                    std::uint8_t* destination = frame_->data[plane] + rowOffset;
                    std::copy_n(source.pixels.data() + offset + row * planeLayout.rowBytes,
                                planeLayout.rowBytes, destination);
                }
                offset += static_cast<std::size_t>(planeLayout.rowBytes) * planeLayout.rows;
            }
        }
        frame_->color_range = codec_->color_range;
        frame_->color_primaries = codec_->color_primaries;
        frame_->color_trc = codec_->color_trc;
        frame_->colorspace = codec_->colorspace;
        frame_->chroma_location = codec_->chroma_sample_location;
        frame_->sample_aspect_ratio = codec_->sample_aspect_ratio;
        frame_->flags &= ~static_cast<unsigned int>(
            AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
        if (source.displayState.interlaced) {
            frame_->flags |= static_cast<unsigned int>(AV_FRAME_FLAG_INTERLACED);
        }
        if (source.displayState.topFieldFirst) {
            frame_->flags |= static_cast<unsigned int>(AV_FRAME_FLAG_TOP_FIELD_FIRST);
        }
        frame_->repeat_pict = source.displayState.repeatPicture;
        frame_->pts = source.timestamp;
        if (matroskaOutput_) {
            const std::int64_t millisecondTimestamp =
                av_rescale_q(source.timestamp, sourceTimeBase_, codec_->time_base);
            if (av_compare_ts(source.timestamp, sourceTimeBase_,
                              millisecondTimestamp, codec_->time_base) != 0) {
                throw std::runtime_error(
                    "Matroska output cannot represent a VFS timestamp exactly at millisecond precision");
            }
            frame_->pts = millisecondTimestamp;
        }

        result = avcodec_send_frame(codec_, frame_);
        if (result == AVERROR(EAGAIN)) {
            receivePackets(false);
            result = avcodec_send_frame(codec_, frame_);
        }
        if (result < 0) {
            throwFfmpeg("lossless video encoder rejected a frame", result);
        }
        receivePackets(false);
    }

    void finish() {
        if (finished_) {
            throw std::logic_error("video output has already been finalized");
        }
        int result = avcodec_send_frame(codec_, nullptr);
        if (result == AVERROR(EAGAIN)) {
            receivePackets(false);
            result = avcodec_send_frame(codec_, nullptr);
        }
        if (result < 0 && result != AVERROR_EOF) {
            throwFfmpeg("cannot flush lossless video encoder", result);
        }
        if (result != AVERROR_EOF) {
            receivePackets(true);
        }
        result = av_write_trailer(format_);
        if (result < 0) {
            throwFfmpeg("cannot write video container trailer", result);
        }
        if (format_->pb != nullptr) {
            result = avio_closep(&format_->pb);
            if (result < 0) {
                throwFfmpeg("cannot close reconstructed video file", result);
            }
        }
        finished_ = true;
    }

private:
    void initialize(const std::filesystem::path& path) {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
        });
        matroskaOutput_ = extension == ".mkv";
        if (!matroskaOutput_ &&
            (!metadata_.colorRange.empty() || !metadata_.colorPrimaries.empty() ||
             !metadata_.colorTransfer.empty() || !metadata_.colorSpace.empty() ||
             !metadata_.chromaLocation.empty() ||
             (!metadata_.fieldOrder.empty() && metadata_.fieldOrder != "progressive"))) {
            throw std::invalid_argument(
                "NUT output cannot preserve the VFS4 colorimetry or interlaced field-order metadata; use .mkv with exactly millisecond-representable timestamps");
        }
        sourceTimeBase_ = AVRational{
            static_cast<int>(metadata_.timeBaseNumerator),
            static_cast<int>(metadata_.timeBaseDenominator)};
        const FrameLayout& layout = metadata_.layout;
        const std::uint32_t width = layout.width;
        const std::uint32_t height = layout.height;
        if (width == 0U || height == 0U ||
            width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("VFS dimensions are too large for FFmpeg video output");
        }
        if (metadata_.timeBaseNumerator == 0U || metadata_.timeBaseDenominator == 0U ||
            metadata_.timeBaseNumerator > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            metadata_.timeBaseDenominator > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("VFS timebase exceeds FFmpeg rational limits");
        }
        const AVPixelFormat pixelFormat = av_get_pix_fmt(metadata_.pixelFormat.c_str());
        if (pixelFormat == AV_PIX_FMT_NONE) {
            throw std::runtime_error("VFS archive names an unknown FFmpeg pixel format");
        }
        if (nativeFrameLayout(static_cast<int>(width), static_cast<int>(height),
                              pixelFormat).planes != layout.planes) {
            throw std::runtime_error("VFS plane geometry does not match its pixel format");
        }
        const AVPixelFormat encoderPixelFormat =
            legacyRgb24Conversion_ && pixelFormat == AV_PIX_FMT_RGB24
            ? AV_PIX_FMT_BGR0
            : pixelFormat;

        const char* muxerName = matroskaOutput_ ? "matroska" : "nut";
        int result = avformat_alloc_output_context2(&format_, nullptr, muxerName,
                                                     path.string().c_str());
        if (result < 0 || format_ == nullptr) {
            throwFfmpeg("cannot create lossless video container",
                        result < 0 ? result : AVERROR_UNKNOWN);
        }
        const auto supportsPixelFormat = [encoderPixelFormat](const AVCodec* candidate) {
            if (candidate == nullptr || candidate->pix_fmts == nullptr) {
                return candidate != nullptr;
            }
            for (const AVPixelFormat* supported = candidate->pix_fmts;
                 *supported != AV_PIX_FMT_NONE; ++supported) {
                if (*supported == encoderPixelFormat) {
                    return true;
                }
            }
            return false;
        };
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_FFV1);
        if (!supportsPixelFormat(encoder)) {
            encoder = avcodec_find_encoder(AV_CODEC_ID_RAWVIDEO);
            if (!supportsPixelFormat(encoder)) {
                throw std::runtime_error("no installed lossless encoder supports native pixel format '" +
                                         metadata_.pixelFormat + "'");
            }
        }
        codec_ = avcodec_alloc_context3(encoder);
        if (codec_ == nullptr) {
            throw std::bad_alloc();
        }
        codec_->width = static_cast<int>(width);
        codec_->height = static_cast<int>(height);
        codec_->pix_fmt = encoderPixelFormat;
        codec_->time_base = matroskaOutput_ ? AVRational{1, 1000} : sourceTimeBase_;
        if (metadata_.sampleAspectRatioNumerator >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            metadata_.sampleAspectRatioDenominator >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("VFS4 sample aspect ratio exceeds FFmpeg limits");
        }
        codec_->color_range = colorRangeFromName(metadata_.colorRange);
        codec_->color_primaries = colorPrimariesFromName(metadata_.colorPrimaries);
        codec_->color_trc = colorTransferFromName(metadata_.colorTransfer);
        codec_->colorspace = colorSpaceFromName(metadata_.colorSpace);
        codec_->chroma_sample_location = chromaLocationFromName(metadata_.chromaLocation);
        codec_->sample_aspect_ratio = AVRational{
            static_cast<int>(metadata_.sampleAspectRatioNumerator),
            static_cast<int>(metadata_.sampleAspectRatioDenominator)};
        codec_->field_order = fieldOrderFromName(metadata_.fieldOrder);
        codec_->gop_size = 1;
        codec_->thread_count = 0;
        if ((format_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        result = avcodec_open2(codec_, encoder, nullptr);
        if (result < 0) {
            throwFfmpeg("cannot initialize lossless video encoder", result);
        }

        stream_ = avformat_new_stream(format_, nullptr);
        if (stream_ == nullptr) {
            throw std::bad_alloc();
        }
        stream_->time_base = codec_->time_base;
        result = avcodec_parameters_from_context(stream_->codecpar, codec_);
        if (result < 0) {
            throwFfmpeg("cannot configure lossless video stream", result);
        }
        stream_->codecpar->color_range = codec_->color_range;
        stream_->codecpar->color_primaries = codec_->color_primaries;
        stream_->codecpar->color_trc = codec_->color_trc;
        stream_->codecpar->color_space = codec_->colorspace;
        stream_->codecpar->chroma_location = codec_->chroma_sample_location;
        stream_->codecpar->sample_aspect_ratio = codec_->sample_aspect_ratio;
        stream_->codecpar->field_order = codec_->field_order;
        stream_->sample_aspect_ratio = codec_->sample_aspect_ratio;
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (frame_ == nullptr || packet_ == nullptr) {
            throw std::bad_alloc();
        }
        frame_->format = codec_->pix_fmt;
        frame_->width = codec_->width;
        frame_->height = codec_->height;
        result = av_frame_get_buffer(frame_, 32);
        if (result < 0) {
            throwFfmpeg("cannot allocate lossless output frame", result);
        }

        if ((format_->oformat->flags & AVFMT_NOFILE) == 0) {
            result = avio_open(&format_->pb, path.string().c_str(), AVIO_FLAG_WRITE);
            if (result < 0) {
                throwFfmpeg("cannot open reconstructed video output", result);
            }
        }
        result = avformat_write_header(format_, nullptr);
        if (result < 0) {
            throwFfmpeg("cannot write reconstructed video header", result);
        }
        if (matroskaOutput_ &&
            (stream_->time_base.num <= 0 || stream_->time_base.den <= 0)) {
            throw std::runtime_error("Matroska muxer did not provide a valid output time base");
        }
    }

    void receivePackets(bool flushing) {
        while (true) {
            const int result = avcodec_receive_packet(codec_, packet_);
            if (result == AVERROR(EAGAIN)) {
                if (flushing) {
                    throw std::runtime_error("lossless video encoder requested input after flush");
                }
                return;
            }
            if (result == AVERROR_EOF) {
                return;
            }
            if (result < 0) {
                throwFfmpeg("lossless video packet generation failed", result);
            }
            if (matroskaOutput_) {
                const auto exactlyRepresentable = [this](std::int64_t timestamp) {
                    if (timestamp == AV_NOPTS_VALUE) {
                        return true;
                    }
                    const std::int64_t converted = av_rescale_q(
                        timestamp, codec_->time_base, stream_->time_base);
                    return av_compare_ts(timestamp, codec_->time_base,
                                         converted, stream_->time_base) == 0;
                };
                if (!exactlyRepresentable(packet_->pts) ||
                    !exactlyRepresentable(packet_->dts)) {
                    throw std::runtime_error(
                        "Matroska stream time base would round a VFS packet timestamp");
                }
            }
            packet_->stream_index = stream_->index;
            av_packet_rescale_ts(packet_, codec_->time_base, stream_->time_base);
            const int writeResult = av_interleaved_write_frame(format_, packet_);
            av_packet_unref(packet_);
            if (writeResult < 0) {
                throwFfmpeg("cannot write lossless video packet", writeResult);
            }
        }
    }

    void release() noexcept {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_);
        if (format_ != nullptr) {
            if (format_->pb != nullptr && (format_->oformat->flags & AVFMT_NOFILE) == 0) {
                avio_closep(&format_->pb);
            }
            avformat_free_context(format_);
            format_ = nullptr;
        }
    }

    FrameStreamMetadata metadata_;
    bool legacyRgb24Conversion_ = false;
    bool matroskaOutput_ = false;
    AVRational sourceTimeBase_{1, 1000};
    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    bool finished_ = false;
};

}  // namespace

VideoArchiveStats packVideoFile(const std::string& inputPath,
                                const std::string& outputPath,
                                int decoderThreads,
                                int compressionLevel) {
    if (decoderThreads < 0 || decoderThreads > 256) {
        throw std::invalid_argument("decoder thread count must be between 0 and 256");
    }
    if (compressionLevel < 0 || compressionLevel > 9) {
        throw std::invalid_argument("compression level must be between 0 and 9");
    }
    const std::filesystem::path input(inputPath);
    const std::filesystem::path output(outputPath);
    requireNewOutput(input, output);
    const std::filesystem::path temporary = makeTemporaryPath(output, false);
    const std::filesystem::path sourceTemporary = temporary.string() + ".source";
    const std::filesystem::path audioTemporary = temporary.string() + ".audio";
    requireTemporaryPathAvailable(temporary);

    VideoReader reader(inputPath, decoderThreads);
    const VideoMetadata& sourceMetadata = reader.metadata();
    const bool hasAudio = sourceMetadata.audioStreamCount != 0U;
    AudioInputStreams audioStreams;
    bool audioDescriptionValid = !hasAudio;
    if (hasAudio) {
        try {
            audioStreams = inspectAudioStreams(input);
            audioDescriptionValid =
                audioStreams.streams.size() == sourceMetadata.audioStreamCount &&
                audioStreamsCanUseNut(audioStreams.streams) &&
                videoMetadataSupportsNutAudio(sourceMetadata);
        } catch (const std::exception&) {
            audioDescriptionValid = false;
        }
    }
    bool nativeArchiveEligible = sourceMetadata.nativeFrameArchiveEligible &&
                                 audioDescriptionValid;

    const std::uint64_t inputBytes = fileSizeOrZero(input);
    const std::string fastPathReason = hasAudio &&
            (!audioDescriptionValid || !sourceMetadata.nativeFrameArchiveEligible)
        ? (audioDescriptionValid ? "source-content-safety-fast-path"
                                 : "audio-remux-unsupported-fast-path")
        : sourcePreservationFastPathReason(sourceMetadata, inputBytes);
    if (!fastPathReason.empty()) {
        const Clock::time_point fastPathStart = Clock::now();
        std::uint64_t sourceArchiveBytes = 0U;
        try {
            requireTemporaryPathAvailable(sourceTemporary);
            sourceArchiveBytes = writeSourceFileArchive(input, sourceTemporary, inputBytes);
            std::error_code error;
            std::filesystem::rename(sourceTemporary, temporary, error);
            if (error) {
                throw std::runtime_error("cannot select source-file archive fast path: " +
                                         error.message());
            }
            commitOutput(temporary, output);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            std::filesystem::remove(sourceTemporary, ignored);
            throw;
        }

        const std::uint32_t width = sourceMetadata.width > 0
            ? static_cast<std::uint32_t>(sourceMetadata.width) : 0U;
        const std::uint32_t height = sourceMetadata.height > 0
            ? static_cast<std::uint32_t>(sourceMetadata.height) : 0U;
        VideoArchiveStats stats = finishStats(sourceMetadata.containerFrameHint,
                                              width, height, sourceMetadata.pixelFormat,
                                              inputBytes, fileSizeOrZero(output),
                                              fastPathStart);
        stats.frameCountIsExact = false;
        stats.representationMode = "source-file";
        stats.representationReason = fastPathReason;
        stats.sourceFileArchiveBytes = sourceArchiveBytes;
        return stats;
    }

    const Clock::time_point start = Clock::now();
    std::uint64_t frameCount = 0;
    std::uint64_t nativeArchiveBytes = 0U;
    std::uint64_t sourceFileArchiveBytes = 0U;
    std::string representationMode = "native-planes";
    std::string representationReason = "native-planes-only";
    AVPixelFormat decodedPixelFormat = AV_PIX_FMT_NONE;
    FrameStreamMetadata archiveMetadata;
    std::unique_ptr<FrameStreamWriter> writer;
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("cannot create temporary VFS archive: " +
                                     temporary.string());
        }
        frameCount = reader.decode([&](const AVFrame* frame, std::uint64_t index) {
            if (frame == nullptr || frame->width != sourceMetadata.width ||
                frame->height != sourceMetadata.height || frame->format < 0) {
                throw std::runtime_error("input video frame layout changed during decoding");
            }
            const AVPixelFormat currentFormat = static_cast<AVPixelFormat>(frame->format);
            if (decodedPixelFormat == AV_PIX_FMT_NONE) {
                decodedPixelFormat = currentFormat;
                archiveMetadata = streamMetadataFor(sourceMetadata, decodedPixelFormat);
                const std::size_t frameBytes = packedFrameBytes(archiveMetadata.layout);
                if (static_cast<std::uint64_t>(frameBytes) > 512ULL * 1024ULL * 1024ULL) {
                    throw std::length_error("native decoded frame exceeds the VFS allocation limit");
                }
                writer = std::make_unique<FrameStreamWriter>(stream, archiveMetadata,
                                                              compressionLevel,
                                                              CompressionCodec::Zstandard,
                                                              hasAudio);
            } else if (currentFormat != decodedPixelFormat) {
                throw std::runtime_error("decoded pixel format changed during the video stream");
            }
            if (frameHasUnpreservedDisplayMetadata(frame, archiveMetadata)) {
                nativeArchiveEligible = false;
            }
            const std::vector<std::uint8_t> packed =
                packNativeFrame(frame, archiveMetadata.layout);
            const FrameDisplayState displayState{
                (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0,
                (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0,
                frame->repeat_pict};
            writer->writeFrame(packed, frameTimestamp(frame, sourceMetadata, index,
                                                       archiveMetadata),
                               displayState);
        });
        if (frameCount == 0U) {
            throw std::runtime_error("input contains no decoded video frames");
        }
        writer->finish();
        writer.reset();
        stream.flush();
        if (!stream) {
            throw std::runtime_error("failed flushing VFS archive");
        }
        stream.close();
        if (!stream) {
            throw std::runtime_error("failed closing VFS archive");
        }
        bool audioCaptureFailed = false;
        if (hasAudio) {
            try {
                requireTemporaryPathAvailable(audioTemporary);
                std::ofstream audioOutput(audioTemporary, std::ios::binary | std::ios::trunc);
                if (!audioOutput) {
                    throw std::runtime_error("cannot create temporary VFS audio extension");
                }
                writeAudioExtension(input, audioStreams, audioOutput);
                audioOutput.flush();
                if (!audioOutput) {
                    throw std::runtime_error("failed flushing VFS audio extension");
                }
                audioOutput.close();
                if (!audioOutput) {
                    throw std::runtime_error("failed closing VFS audio extension");
                }
                appendFileBytes(audioTemporary, temporary);
                std::error_code error;
                if (!std::filesystem::remove(audioTemporary, error) || error) {
                    throw std::runtime_error("cannot remove temporary VFS audio extension: " +
                        (error ? error.message() : std::string("temporary file disappeared")));
                }
            } catch (const std::exception&) {
                std::error_code ignored;
                std::filesystem::remove(audioTemporary, ignored);
                std::filesystem::remove(temporary, ignored);
                nativeArchiveEligible = false;
                audioCaptureFailed = true;
            }
        }
        if (!audioCaptureFailed) {
            nativeArchiveBytes = requiredFileSize(temporary);
        }
        if (inputBytes != 0U && inputBytes <=
                std::numeric_limits<std::uint64_t>::max() - kSourceFileOverhead) {
            requireTemporaryPathAvailable(sourceTemporary);
            sourceFileArchiveBytes = writeSourceFileArchive(input, sourceTemporary,
                                                             inputBytes);
            if (audioCaptureFailed || !nativeArchiveEligible ||
                sourceFileArchiveBytes < nativeArchiveBytes) {
                std::error_code error;
                const bool removed = std::filesystem::remove(temporary, error);
                if (error || !removed) {
                    throw std::runtime_error("cannot replace native archive candidate: " +
                        (error ? error.message() : std::string("temporary candidate disappeared")));
                }
                std::filesystem::rename(sourceTemporary, temporary, error);
                if (error) {
                    throw std::runtime_error("cannot select source-file archive candidate: " +
                                             error.message());
                }
                representationMode = "source-file";
                representationReason = audioCaptureFailed
                    ? "audio-remux-source-fallback"
                    : nativeArchiveEligible
                    ? "smaller-source-file"
                    : "source-content-safety";
            } else {
                std::error_code error;
                const bool removed = std::filesystem::remove(sourceTemporary, error);
                if (error || !removed) {
                    throw std::runtime_error("cannot remove unselected source-file candidate: " +
                        (error ? error.message() : std::string("temporary candidate disappeared")));
                }
            }
        }
        if (!nativeArchiveEligible && sourceFileArchiveBytes == 0U) {
            throw std::runtime_error(
                "decoded-plane archive would discard source content, and a byte-preserving archive could not be created");
        }
        commitOutput(temporary, output);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        std::filesystem::remove(sourceTemporary, ignored);
        std::filesystem::remove(audioTemporary, ignored);
        throw;
    }

    VideoArchiveStats stats = finishStats(frameCount, archiveMetadata.layout.width,
                                          archiveMetadata.layout.height,
                                          archiveMetadata.pixelFormat, inputBytes,
                                          fileSizeOrZero(output), start);
    stats.representationMode = std::move(representationMode);
    stats.representationReason = std::move(representationReason);
    stats.nativeArchiveBytes = nativeArchiveBytes;
    stats.sourceFileArchiveBytes = sourceFileArchiveBytes;
    return stats;
}

VideoArchiveStats unpackVideoFile(const std::string& inputPath,
                                  const std::string& outputPath) {
    const std::filesystem::path input(inputPath);
    const std::filesystem::path output(outputPath);
    requireNewOutput(input, output);
    const bool sourceFileMode = hasSourceFileSignature(input);
    if (!sourceFileMode) {
        std::string extension = output.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension != ".nut" && extension != ".mkv") {
            throw std::invalid_argument("unpack output for VFS1/VFS2/VFS3/VFS4/VFS5 must use .nut for exact VFS timing or .mkv when all timestamps are exactly representable in milliseconds");
        }
    }
    const std::filesystem::path temporary = makeTemporaryPath(output, true);
    requireTemporaryPathAvailable(temporary);

    const std::uint64_t inputBytes = fileSizeOrZero(input);
    const Clock::time_point start = Clock::now();
    if (sourceFileMode) {
        try {
            (void)restoreSourceFileArchive(input, temporary);
            commitOutput(temporary, output);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
        VideoArchiveStats stats = finishStats(0U, 0U, 0U, {}, inputBytes,
                                              fileSizeOrZero(output), start);
        stats.frameCountIsExact = false;
        stats.representationMode = "source-file";
        return stats;
    }

    std::ifstream stream(input, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open VFS archive: " + input.string());
    }
    FrameStreamReader reader(stream);
    const FrameStreamMetadata metadata = reader.metadata();
    const bool hasAudioExtension = reader.formatVersion() == 5U;
    if (hasAudioExtension) {
        std::string extension = output.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        if (extension != ".nut") {
            throw std::invalid_argument(
                "VFS5 compressed audio requires .nut output to preserve packet timestamps exactly");
        }
    }
    FrameStreamMetadata outputMetadata = metadata;
    const bool legacyRgb24Conversion = metadata.pixelFormat.empty();
    if (metadata.pixelFormat.empty()) {
        if (metadata.layout.channels != 3U) {
            throw std::runtime_error("legacy VFS1 archive is not a canonical RGB24 frame stream");
        }
        outputMetadata.pixelFormat = "rgb24";
        outputMetadata.layout.planes = nativeFrameLayout(
            static_cast<int>(metadata.layout.width),
            static_cast<int>(metadata.layout.height), AV_PIX_FMT_RGB24).planes;
    } else {
        const AVPixelFormat pixelFormat = av_get_pix_fmt(metadata.pixelFormat.c_str());
        if (pixelFormat == AV_PIX_FMT_NONE ||
            nativeFrameLayout(static_cast<int>(metadata.layout.width),
                              static_cast<int>(metadata.layout.height), pixelFormat).planes !=
                metadata.layout.planes) {
            throw std::runtime_error("VFS native plane metadata is inconsistent with its pixel format");
        }
    }
    std::uint64_t frameCount = 0;
    const std::filesystem::path videoTemporary = temporary.string() + ".video.nut";
    try {
        const std::filesystem::path& videoOutput =
            hasAudioExtension ? videoTemporary : temporary;
        if (hasAudioExtension) {
            requireTemporaryPathAvailable(videoTemporary);
        }
        LosslessNutWriter writer(videoOutput, outputMetadata, legacyRgb24Conversion);
        DecodedStreamFrame frame;
        while (reader.readNext(frame)) {
            writer.writeFrame(frame);
            ++frameCount;
        }
        if (frameCount == 0U) {
            throw std::runtime_error("VFS archive contains no video frames");
        }
        writer.finish();
        if (hasAudioExtension) {
            muxNutVideoAndAudio(videoTemporary, stream, temporary);
            std::error_code error;
            if (!std::filesystem::remove(videoTemporary, error) || error) {
                throw std::runtime_error("cannot remove temporary lossless video stream: " +
                    (error ? error.message() : std::string("temporary file disappeared")));
            }
        }
        commitOutput(temporary, output);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        std::filesystem::remove(videoTemporary, ignored);
        throw;
    }

    VideoArchiveStats stats = finishStats(frameCount, metadata.layout.width,
                                          metadata.layout.height,
                                          outputMetadata.pixelFormat, inputBytes,
                                          fileSizeOrZero(output), start);
    stats.representationMode = "native-planes";
    return stats;
}

}  // namespace frameforge
