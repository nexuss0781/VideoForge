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

add_executable(frameforge
    src/main.cpp
    src/video_reader.cpp
    src/analyzer.cpp
    src/png_writer.cpp
)
target_include_directories(frameforge PRIVATE include)
target_link_libraries(frameforge PRIVATE PkgConfig::FFMPEG)
target_compile_options(frameforge PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
)

include(GNUInstallDirs)
install(TARGETS frameforge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()
find_program(FFMPEG_CLI ffmpeg)
find_package(Python3 COMPONENTS Interpreter)
if(FFMPEG_CLI AND Python3_Interpreter_FOUND)
    add_test(NAME frameforge_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh
                $<TARGET_FILE:frameforge> ${FFMPEG_CLI} ${Python3_EXECUTABLE})
endif()
