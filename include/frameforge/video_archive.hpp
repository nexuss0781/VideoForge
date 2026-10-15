#pragma once

#include <cstdint>
#include <string>

namespace frameforge {

struct VideoArchiveStats {
    std::uint64_t frameCount = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string pixelFormat;
    std::uint64_t inputBytes = 0;
    std::uint64_t outputBytes = 0;
    double elapsedSeconds = 0.0;
    double framesPerSecond = 0.0;
    double outputInputRatio = 0.0;
    std::string representationMode = "native-planes";
    // For pack operations: smaller-candidate choice or a content-safety fallback.
    // Unpack operations leave this as "not-applicable".
    std::string representationReason = "not-applicable";
    std::uint64_t nativeArchiveBytes = 0;
    std::uint64_t sourceFileArchiveBytes = 0;
    // False when frameCount is only a container hint or was not measured by decoding.
    bool frameCountIsExact = true;
};

// Decode the source video and compare a checksummed native-plane archive
// with a checksummed byte-preserving source-file representation. The smaller
// measured representation is selected. VFS5 stores compressed audio packets
// and stream parameters when exact NUT remuxing is supported; otherwise the
// exact-source representation is retained. Outputs are transactional and never
// overwrite a file.
[[nodiscard]] VideoArchiveStats packVideoFile(const std::string& inputPath,
                                              const std::string& outputPath,
                                              int decoderThreads = 0,
                                              int compressionLevel = 1);

// Decode and validate a source-file or VFS1–VFS5 archive. Source-file archives
// restore their original bytes exactly; VFS1–VFS4 archives write lossless video,
// while VFS5 also restores archived compressed audio packets to NUT without
// audio re-encoding. Source-file restore statistics do not include decoded frame
// metadata because that mode restores bytes without decoding frames. Legacy
// RGB24 VFS1 is readable.
[[nodiscard]] VideoArchiveStats unpackVideoFile(const std::string& inputPath,
                                                const std::string& outputPath);

}  // namespace frameforge
