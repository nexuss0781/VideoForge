#pragma once

#include <iosfwd>

#include "frameforge/video_reader.hpp"

namespace frameforge {

struct AnalysisOptions {
    int sampleWidth = 96;
    int sampleHeight = 54;
    double sceneThreshold = 0.30;
};

// Writes a streaming JSON document containing metadata, per-frame diagnostics,
// and scene-like temporal segments; memory use is bounded by the small sample
// image plus the number of detected segments.
void analyzeVideo(VideoReader& reader, std::ostream& output,
                  const AnalysisOptions& options = {});

[[nodiscard]] std::string jsonEscape(const std::string& value);

}  // namespace frameforge
