#pragma once

#include "frameforge/audio_stream.hpp"

#include <filesystem>
#include <iosfwd>
#include <vector>

namespace frameforge {

struct AudioInputStreams {
    std::vector<AudioStreamInfo> streams;
    // Original AVFormatContext stream indices, parallel to streams.
    std::vector<int> inputStreamIndices;
};

[[nodiscard]] AudioInputStreams inspectAudioStreams(const std::filesystem::path& source);
[[nodiscard]] bool audioStreamsCanUseNut(const std::vector<AudioStreamInfo>& streams);
void writeAudioExtension(const std::filesystem::path& source,
                         const AudioInputStreams& streams,
                         std::ostream& output);
void muxNutVideoAndAudio(const std::filesystem::path& videoOnlyNut,
                         std::istream& audioExtension,
                         const std::filesystem::path& outputNut);

}  // namespace frameforge
