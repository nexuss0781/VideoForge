cmake_minimum_required(VERSION 3.20)
project(frameforge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
)
find_package(ZLIB REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_library(frameforge_codec STATIC
    src/frame_codec.cpp
    src/frame_stream.cpp
    src/language_transform.cpp
)
target_include_directories(frameforge_codec PUBLIC include)
target_link_libraries(frameforge_codec PUBLIC ZLIB::ZLIB PkgConfig::ZSTD)
target_compile_options(frameforge_codec PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)

add_library(frameforge_audio_archive STATIC src/audio_stream.cpp)
target_include_directories(frameforge_audio_archive PUBLIC include)
target_link_libraries(frameforge_audio_archive PUBLIC PkgConfig::FFMPEG ZLIB::ZLIB)
target_compile_options(frameforge_audio_archive PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)

add_executable(frameforge
    src/main.cpp
    src/video_reader.cpp
    src/audio_remux.cpp
    src/video_archive.cpp
    src/analyzer.cpp
    src/png_writer.cpp
)
target_include_directories(frameforge PRIVATE include)
target_link_libraries(frameforge PRIVATE PkgConfig::FFMPEG frameforge_codec
    frameforge_audio_archive)
target_compile_options(frameforge PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)

include(GNUInstallDirs)
install(TARGETS frameforge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()
add_executable(frameforge_codec_test tests/frame_codec_test.cpp)
target_link_libraries(frameforge_codec_test PRIVATE frameforge_codec)
target_compile_options(frameforge_codec_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)
add_test(NAME frameforge_codec_unit COMMAND frameforge_codec_test)

add_executable(frameforge_language_transform_test tests/language_transform_test.cpp)
target_link_libraries(frameforge_language_transform_test PRIVATE frameforge_codec)
target_compile_options(frameforge_language_transform_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)
add_test(NAME frameforge_language_transform_unit COMMAND frameforge_language_transform_test)

add_executable(frameforge_language_transform_benchmark
    benchmarks/language_transform_benchmark.cpp
)
target_link_libraries(frameforge_language_transform_benchmark PRIVATE frameforge_codec)
target_compile_options(frameforge_language_transform_benchmark PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)

add_executable(frameforge_stream_test tests/frame_stream_test.cpp)
target_link_libraries(frameforge_stream_test PRIVATE frameforge_codec)
target_compile_options(frameforge_stream_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)
add_test(NAME frameforge_stream_unit COMMAND frameforge_stream_test)

add_executable(frameforge_audio_stream_test tests/audio_stream_test.cpp)
target_link_libraries(frameforge_audio_stream_test PRIVATE frameforge_audio_archive)
target_compile_options(frameforge_audio_stream_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)
add_test(NAME frameforge_audio_stream_unit COMMAND frameforge_audio_stream_test)

add_library(frameforge_motion_experiment STATIC
    experiments/block_motion_codec.cpp
    experiments/global_motion_gate.cpp
)
target_include_directories(frameforge_motion_experiment PUBLIC include experiments)
target_link_libraries(frameforge_motion_experiment PUBLIC frameforge_codec)
target_compile_options(frameforge_motion_experiment PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)

add_executable(frameforge_block_motion_test tests/block_motion_codec_test.cpp)
target_link_libraries(frameforge_block_motion_test PRIVATE frameforge_motion_experiment)
target_compile_options(frameforge_block_motion_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)
add_test(NAME frameforge_block_motion_experiment COMMAND frameforge_block_motion_test)

add_executable(frameforge_global_motion_test tests/global_motion_gate_test.cpp)
target_link_libraries(frameforge_global_motion_test PRIVATE frameforge_motion_experiment)
target_compile_options(frameforge_global_motion_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)
add_test(NAME frameforge_global_motion_gate_experiment COMMAND frameforge_global_motion_test)

add_executable(frameforge_block_motion_benchmark
    benchmarks/block_motion_benchmark.cpp
)
target_include_directories(frameforge_block_motion_benchmark PRIVATE experiments)
target_link_libraries(frameforge_block_motion_benchmark PRIVATE frameforge_motion_experiment)
target_compile_options(frameforge_block_motion_benchmark PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)

add_executable(frameforge_global_motion_benchmark
    benchmarks/global_motion_benchmark.cpp
)
target_include_directories(frameforge_global_motion_benchmark PRIVATE experiments)
target_link_libraries(frameforge_global_motion_benchmark PRIVATE frameforge_motion_experiment)
target_compile_options(frameforge_global_motion_benchmark PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)

find_program(FFMPEG_CLI ffmpeg)
find_program(FFPROBE_CLI ffprobe)
find_package(Python3 COMPONENTS Interpreter)
if(FFMPEG_CLI AND Python3_Interpreter_FOUND)
    add_test(NAME frameforge_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh
                $<TARGET_FILE:frameforge> ${FFMPEG_CLI} ${Python3_EXECUTABLE})

    set(FRAMEFORGE_BENCHMARK_FRAMES 48 CACHE STRING
        "Frames per deterministic synthetic video in the benchmark")
    set(FRAMEFORGE_BENCHMARK_SIZES "320x180,640x360,1280x720" CACHE STRING
        "Comma-separated even video dimensions for the benchmark")
    set(FRAMEFORGE_BENCHMARK_REPEATS 3 CACHE STRING
        "Pack/unpack repetitions per benchmark case; median times are reported")
    set(FRAMEFORGE_BENCHMARK_COMPRESSION 1 CACHE STRING
        "VFS Zstandard compression level for the benchmark (0..9)")
    add_custom_target(frameforge_benchmark
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/video_benchmark.py
                --frameforge $<TARGET_FILE:frameforge>
                --ffmpeg ${FFMPEG_CLI}
                --output-dir ${CMAKE_BINARY_DIR}/benchmark-results
                --frames ${FRAMEFORGE_BENCHMARK_FRAMES}
                --sizes ${FRAMEFORGE_BENCHMARK_SIZES}
                --repeats ${FRAMEFORGE_BENCHMARK_REPEATS}
                --compression ${FRAMEFORGE_BENCHMARK_COMPRESSION}
                --build-type $<CONFIG>
        DEPENDS frameforge
        COMMENT "Running deterministic video codec benchmark ($<CONFIG>)"
        USES_TERMINAL)

    if(FFPROBE_CLI)
        set(FRAMEFORGE_AUDIO_BENCHMARK_FRAMES 48 CACHE STRING
            "Frames per audio-bearing source in the VFS5 benchmark")
        set(FRAMEFORGE_AUDIO_BENCHMARK_SIZES "160x90,320x180,640x360" CACHE STRING
            "Comma-separated even dimensions for the audio-bearing VFS5 benchmark")
        set(FRAMEFORGE_AUDIO_BENCHMARK_REPEATS 3 CACHE STRING
            "Pack/unpack repetitions per audio-bearing benchmark case")
        set(FRAMEFORGE_AUDIO_BENCHMARK_COMPRESSION 1 CACHE STRING
            "VFS Zstandard compression level for the audio-bearing benchmark (0..9)")
        add_custom_target(frameforge_audio_benchmark
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/audio_video_benchmark.py
                    --frameforge $<TARGET_FILE:frameforge>
                    --ffmpeg ${FFMPEG_CLI}
                    --ffprobe ${FFPROBE_CLI}
                    --output-dir ${CMAKE_BINARY_DIR}/audio-video-benchmark-results
                    --frames ${FRAMEFORGE_AUDIO_BENCHMARK_FRAMES}
                    --sizes ${FRAMEFORGE_AUDIO_BENCHMARK_SIZES}
                    --repeats ${FRAMEFORGE_AUDIO_BENCHMARK_REPEATS}
                    --compression ${FRAMEFORGE_AUDIO_BENCHMARK_COMPRESSION}
                    --build-type $<CONFIG>
            DEPENDS frameforge
            COMMENT "Running reproducible audio-bearing VFS5 benchmark ($<CONFIG>)"
            USES_TERMINAL)
    endif()

    add_custom_target(frameforge_block_motion_study
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/block_motion_study.py
                --frameforge $<TARGET_FILE:frameforge>
                --motion-benchmark $<TARGET_FILE:frameforge_block_motion_benchmark>
                --ffmpeg ${FFMPEG_CLI}
                --output-dir ${CMAKE_BINARY_DIR}/block-motion-study
                --frames ${FRAMEFORGE_BENCHMARK_FRAMES}
                --sizes ${FRAMEFORGE_BENCHMARK_SIZES}
                --repeats ${FRAMEFORGE_BENCHMARK_REPEATS}
        DEPENDS frameforge frameforge_block_motion_benchmark
        COMMENT "Running the reversible block-motion codec study ($<CONFIG>)"
        USES_TERMINAL)

    add_custom_target(frameforge_global_motion_study
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/global_motion_study.py
                --frameforge $<TARGET_FILE:frameforge>
                --motion-benchmark $<TARGET_FILE:frameforge_global_motion_benchmark>
                --ffmpeg ${FFMPEG_CLI}
                --output-dir ${CMAKE_BINARY_DIR}/global-motion-study
                --frames ${FRAMEFORGE_BENCHMARK_FRAMES}
                --sizes ${FRAMEFORGE_BENCHMARK_SIZES}
                --repeats ${FRAMEFORGE_BENCHMARK_REPEATS}
        DEPENDS frameforge frameforge_global_motion_benchmark
        COMMENT "Running the gated global-translation experiment ($<CONFIG>)"
        USES_TERMINAL)
endif()
