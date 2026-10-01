#pragma once

#include "capture_core.h"
#include "capture_preview.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace yourots {

struct RecorderOptions {
    std::filesystem::path ffmpeg_path;
    std::filesystem::path ffprobe_path;
    std::filesystem::path output_path;
    CropRect crop{};
    int fps{30};
    std::size_t queue_capacity{4};
};

struct RecorderResult {
    std::filesystem::path output_path;
    std::filesystem::path temporary_mkv_path;
    std::wstring encoder;
    std::uint64_t frames_written{};
    std::uint64_t source_frames_skipped{};
    std::uint64_t queue_overflows{};
    double duration_seconds{};
};

class Recorder {
public:
    Recorder(std::shared_ptr<WindowCapture> capture, RecorderOptions options);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void Start();
    RecorderResult Stop();
    bool IsRecording() const noexcept;
    std::string LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace yourots
