#include "frameforge/analyzer.hpp"
#include "frameforge/png_writer.hpp"
#include "frameforge/video_archive.hpp"
#include "frameforge/video_reader.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct FrameRange {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

struct Selection {
    std::vector<std::uint64_t> indices;
    std::vector<FrameRange> ranges;

    [[nodiscard]] bool empty() const noexcept {
        return indices.empty() && ranges.empty();
    }

    void normalize() {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        std::sort(ranges.begin(), ranges.end(), [](const FrameRange& left,
                                                   const FrameRange& right) {
            return left.first < right.first ||
                   (left.first == right.first && left.last < right.last);
        });
        std::vector<FrameRange> merged;
        for (const FrameRange& range : ranges) {
            const bool adjacent = !merged.empty() &&
                merged.back().last != std::numeric_limits<std::uint64_t>::max() &&
                range.first == merged.back().last + 1U;
            if (!merged.empty() && (range.first <= merged.back().last || adjacent)) {
                merged.back().last = std::max(merged.back().last, range.last);
            } else {
                merged.push_back(range);
            }
        }
        ranges = std::move(merged);
    }

    [[nodiscard]] bool contains(std::uint64_t frame) const {
        if (std::binary_search(indices.begin(), indices.end(), frame)) {
            return true;
        }
        const auto after = std::upper_bound(ranges.begin(), ranges.end(), frame,
            [](std::uint64_t value, const FrameRange& range) {
                return value < range.first;
            });
        return after != ranges.begin() && frame <= std::prev(after)->last;
    }

    [[nodiscard]] std::uint64_t maximum() const {
        std::uint64_t result = 0;
        for (const std::uint64_t index : indices) {
            result = std::max(result, index);
        }
        for (const FrameRange& range : ranges) {
            result = std::max(result, range.last);
        }
        return result;
    }
};

struct ExportedFrame {
    std::uint64_t index = 0;
    double timestamp = 0.0;
    std::string filename;
};

void printUsage(std::ostream& output) {
    output << R"(FrameForge — high-throughput video inspection and frame segmentation

Usage:
  frameforge analyze INPUT [--output REPORT.json] [--threads N]
                         [--threshold 0.30] [--sample-width 96] [--sample-height 54]
  frameforge extract INPUT --out DIRECTORY
                         [--indices 0,12,99] [--ranges 100:120,500:520]
                         [--threads N]
  frameforge pack INPUT --out ARCHIVE.vfs [--threads N] [--compression 0..9]
  frameforge unpack ARCHIVE.vfs --out OUTPUT

Commands:
  analyze  Decode every video frame, count them exactly, emit per-frame image
           statistics, and infer temporal scene-like segments. REPORT.json is
           streamed to disk; use --output - for stdout (the default).
  extract  Decode sequentially and export the exact requested zero-based frames
           as lossless PNG files, with a manifest.json. Ranges are inclusive.
           Provide --indices and/or --ranges; at least one is required.
  pack     Compare measured sizes for checksummed native planes (VFS3/VFS4/VFS5)
           and a byte-preserving source-file representation; store the smaller
           safe candidate. VFS5 stores compressed audio packets when NUT can
           preserve stream metadata and timestamps exactly.
  unpack   Restore source-file archives byte-for-byte, or validate VFS1–VFS5 and
           create lossless NUT output; VFS5 restores audio packets without
           decoding or re-encoding them.

Options:
  --threads N       FFmpeg decoder threads; 0 lets FFmpeg choose (default 0).
  --threshold X     Scene heuristic threshold in (0,1], default 0.30.
  --sample-width N  Analysis thumbnail width, default 96 pixels.
  --sample-height N Analysis thumbnail height, default 54 pixels.
  --compression N   VFS Zstandard effort level 0 (fastest) through 9, default 1.
  --help            Show this help.

This tool performs deterministic technical and visual-change analysis, not
semantic object recognition. Frame numbering follows decoded presentation order.
Supported input codecs/containers depend on the installed FFmpeg build.
)";
}

std::uint64_t parseUnsigned(std::string_view text, const std::string& label) {
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument(label + " must be a non-negative integer");
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid " + label + ": " + std::string(text));
    }
    return value;
}

int parseInt(const std::string& text, const std::string& label,
             int minimum, int maximum) {
    const std::uint64_t value = parseUnsigned(text, label);
    if (value < static_cast<std::uint64_t>(minimum) ||
        value > static_cast<std::uint64_t>(maximum)) {
        throw std::invalid_argument(label + " must be between " +
                                    std::to_string(minimum) + " and " +
                                    std::to_string(maximum));
    }
    return static_cast<int>(value);
}

double parseDouble(const std::string& text, const std::string& label) {
    std::size_t consumed = 0;
    double value = 0.0;
    try {
        value = std::stod(text, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid " + label + ": " + text);
    }
    if (consumed != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("invalid " + label + ": " + text);
    }
    return value;
}

void parseIndices(const std::string& value, Selection& selection) {
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (item.empty()) {
            throw std::invalid_argument("--indices contains an empty item");
        }
        selection.indices.push_back(parseUnsigned(item, "frame index"));
    }
    if (!value.empty() && value.back() == ',') {
        throw std::invalid_argument("--indices must not end with a comma");
    }
    std::sort(selection.indices.begin(), selection.indices.end());
    selection.indices.erase(std::unique(selection.indices.begin(), selection.indices.end()),
                            selection.indices.end());
}

void parseRanges(const std::string& value, Selection& selection) {
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        const std::size_t colon = item.find(':');
        if (colon == std::string::npos || colon == 0U || colon + 1U >= item.size() ||
            item.find(':', colon + 1U) != std::string::npos) {
            throw std::invalid_argument("invalid frame range '" + item + "' (expected START:END)");
        }
        const std::uint64_t first = parseUnsigned(std::string_view(item).substr(0, colon),
                                                  "range start");
        const std::uint64_t last = parseUnsigned(std::string_view(item).substr(colon + 1U),
                                                 "range end");
        if (first > last) {
            throw std::invalid_argument("frame range start must not exceed its end");
        }
        selection.ranges.push_back(FrameRange{first, last});
    }
    if (!value.empty() && value.back() == ',') {
        throw std::invalid_argument("--ranges must not end with a comma");
    }
}

void writeManifest(const std::filesystem::path& outputDirectory,
                   const frameforge::VideoMetadata& metadata,
                   const std::vector<ExportedFrame>& frames,
                   std::uint64_t decodedCount) {
    const auto finalPath = outputDirectory / "manifest.json";
    const auto temporaryPath = outputDirectory / "manifest.json.tmp";
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create extraction manifest: " + temporaryPath.string());
    }
    output << "{\"schema_version\":\"1.0\",\"input\":\""
           << frameforge::jsonEscape(metadata.path) << "\",\"frame_width\":"
           << metadata.width << ",\"frame_height\":" << metadata.height
           << ",\"decoded_frame_count\":" << decodedCount
           << ",\"container_frame_count_hint\":" << metadata.containerFrameHint
           << ",\"source_duration_seconds\":" << metadata.durationSeconds
           << ",\"exported_count\":" << frames.size() << ",\"frames\":[";
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i != 0U) {
            output << ',';
        }
        output << "{\"index\":" << frames[i].index
               << ",\"timestamp_seconds\":" << frames[i].timestamp
               << ",\"file\":\"" << frameforge::jsonEscape(frames[i].filename) << "\"}";
    }
    output << "]}\n";
    output.close();
    if (!output) {
        throw std::runtime_error("failed writing extraction manifest");
    }
    std::filesystem::rename(temporaryPath, finalPath);
}

int analyzeCommand(const std::string& inputPath, int argc, char** argv) {
    std::string outputPath = "-";
    int threads = 0;
    frameforge::AnalysisOptions options;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (option == "--threads" && i + 1 < argc) {
            threads = parseInt(argv[++i], "thread count", 0, 256);
        } else if (option == "--threshold" && i + 1 < argc) {
            options.sceneThreshold = parseDouble(argv[++i], "scene threshold");
        } else if (option == "--sample-width" && i + 1 < argc) {
            options.sampleWidth = parseInt(argv[++i], "sample width", 1, 8192);
        } else if (option == "--sample-height" && i + 1 < argc) {
            options.sampleHeight = parseInt(argv[++i], "sample height", 1, 8192);
        } else {
            throw std::invalid_argument("unknown or incomplete analyze option: " + option);
        }
    }

    frameforge::VideoReader reader(inputPath, threads);
    if (outputPath == "-") {
        frameforge::analyzeVideo(reader, std::cout, options);
    } else {
        const std::filesystem::path finalPath(outputPath);
        const std::filesystem::path temporaryPath = outputPath + ".tmp";
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create report: " + temporaryPath.string());
        }
        try {
            frameforge::analyzeVideo(reader, output, options);
            output.close();
            if (!output) {
                throw std::runtime_error("failed while finalizing analysis report");
            }
            std::filesystem::rename(temporaryPath, finalPath);
        } catch (...) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            throw;
        }
    }
    return 0;
}

int extractCommand(const std::string& inputPath, int argc, char** argv) {
    std::filesystem::path outputDirectory;
    Selection selection;
    int threads = 0;
    bool haveOutputDirectory = false;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--out" && i + 1 < argc) {
            outputDirectory = argv[++i];
            haveOutputDirectory = true;
        } else if (option == "--indices" && i + 1 < argc) {
            parseIndices(argv[++i], selection);
        } else if (option == "--ranges" && i + 1 < argc) {
            parseRanges(argv[++i], selection);
        } else if (option == "--threads" && i + 1 < argc) {
            threads = parseInt(argv[++i], "thread count", 0, 256);
        } else {
            throw std::invalid_argument("unknown or incomplete extract option: " + option);
        }
    }
    if (!haveOutputDirectory || outputDirectory.empty()) {
        throw std::invalid_argument("extract requires --out DIRECTORY");
    }
    selection.normalize();
    if (selection.empty()) {
        throw std::invalid_argument("extract requires --indices and/or --ranges");
    }

    frameforge::VideoReader reader(inputPath, threads);
    const frameforge::VideoMetadata& metadata = reader.metadata();
    frameforge::PngWriter writer(outputDirectory, metadata.width, metadata.height);
    std::vector<ExportedFrame> exported;
    const std::uint64_t decodedCount = reader.decode([&](const AVFrame* frame,
                                                         std::uint64_t index) {
        if (!selection.contains(index)) {
            return;
        }
        const std::filesystem::path path = writer.write(frame, index);
        exported.push_back(ExportedFrame{index,
            frameforge::frameTimestampSeconds(frame, metadata, index), path.filename().string()});
    });

    if (decodedCount == 0U) {
        throw std::runtime_error("input contains no decoded video frames");
    }
    if (selection.maximum() >= decodedCount) {
        throw std::out_of_range("requested frame " + std::to_string(selection.maximum()) +
                                " is outside the decoded frame range [0," +
                                std::to_string(decodedCount - 1U) +
                                "]; already exported valid frames, if any, remain in the output directory");
    }
    if (exported.empty()) {
        throw std::runtime_error("no frames matched the requested selection");
    }
    writeManifest(outputDirectory, metadata, exported, decodedCount);

    std::cout << "{\"input\":\"" << frameforge::jsonEscape(inputPath)
              << "\",\"decoded_frame_count\":" << decodedCount
              << ",\"exported_count\":" << exported.size()
              << ",\"output_directory\":\""
              << frameforge::jsonEscape(outputDirectory.string())
              << "\",\"manifest\":\""
              << frameforge::jsonEscape((outputDirectory / "manifest.json").string())
              << "\"}\n";
    return 0;
}

void printArchiveStats(std::string_view operation, const std::string& inputPath,
                       const std::string& outputPath,
                       const frameforge::VideoArchiveStats& stats) {
    std::cout << std::setprecision(9)
              << "{\"operation\":\"" << operation
              << "\",\"input\":\"" << frameforge::jsonEscape(inputPath)
              << "\",\"output\":\"" << frameforge::jsonEscape(outputPath)
              << "\",\"width\":" << stats.width
              << ",\"height\":" << stats.height
              << ",\"pixel_format\":\""
              << frameforge::jsonEscape(stats.pixelFormat) << "\""
              << ",\"frame_count\":" << stats.frameCount
              << ",\"frame_count_exact\":"
              << (stats.frameCountIsExact ? "true" : "false")
              << ",\"representation_mode\":\""
              << frameforge::jsonEscape(stats.representationMode) << "\""
              << ",\"representation_reason\":\""
              << frameforge::jsonEscape(stats.representationReason) << "\""
              << ",\"input_bytes\":" << stats.inputBytes
              << ",\"output_bytes\":" << stats.outputBytes
              << ",\"output_input_ratio\":" << stats.outputInputRatio
              << ",\"native_archive_bytes\":" << stats.nativeArchiveBytes
              << ",\"source_file_archive_bytes\":" << stats.sourceFileArchiveBytes
              << ",\"elapsed_seconds\":" << stats.elapsedSeconds
              << ",\"frames_per_second\":" << stats.framesPerSecond << "}\n";
}

int packCommand(const std::string& inputPath, int argc, char** argv) {
    std::string outputPath;
    int threads = 0;
    int compression = 1;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--out" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (option == "--threads" && i + 1 < argc) {
            threads = parseInt(argv[++i], "thread count", 0, 256);
        } else if (option == "--compression" && i + 1 < argc) {
            compression = parseInt(argv[++i], "compression level", 0, 9);
        } else {
            throw std::invalid_argument("unknown or incomplete pack option: " + option);
        }
    }
    if (outputPath.empty()) {
        throw std::invalid_argument("pack requires --out ARCHIVE.vfs");
    }
    const frameforge::VideoArchiveStats stats =
        frameforge::packVideoFile(inputPath, outputPath, threads, compression);
    printArchiveStats("pack", inputPath, outputPath, stats);
    return 0;
}

int unpackCommand(const std::string& inputPath, int argc, char** argv) {
    std::string outputPath;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--out" && i + 1 < argc) {
            outputPath = argv[++i];
        } else {
            throw std::invalid_argument("unknown or incomplete unpack option: " + option);
        }
    }
    if (outputPath.empty()) {
        throw std::invalid_argument("unpack requires --out VIDEO.nut");
    }
    const frameforge::VideoArchiveStats stats =
        frameforge::unpackVideoFile(inputPath, outputPath);
    printArchiveStats("unpack", inputPath, outputPath, stats);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help" ||
        std::string_view(argv[1]) == "-h") {
        printUsage(argc < 2 ? std::cerr : std::cout);
        return argc < 2 ? 2 : 0;
    }
    try {
        const std::string command = argv[1];
        if (command == "analyze") {
            if (argc < 3) {
                throw std::invalid_argument("analyze requires an input video path");
            }
            return analyzeCommand(argv[2], argc, argv);
        }
        if (command == "extract") {
            if (argc < 3) {
                throw std::invalid_argument("extract requires an input video path");
            }
            return extractCommand(argv[2], argc, argv);
        }
        if (command == "pack") {
            if (argc < 3) {
                throw std::invalid_argument("pack requires an input video path");
            }
            return packCommand(argv[2], argc, argv);
        }
        if (command == "unpack") {
            if (argc < 3) {
                throw std::invalid_argument("unpack requires a VFS archive path");
            }
            return unpackCommand(argv[2], argc, argv);
        }
        printUsage(std::cerr);
        throw std::invalid_argument("unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "frameforge: " << error.what() << '\n';
        return 1;
    }
}
