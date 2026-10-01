#pragma once

#include "capture_preview.h"
#include "capture_core.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace yourots::detail {

void ValidateRecorderSettings(int fps, std::size_t capacity, const std::filesystem::path& output);
std::vector<std::uint8_t> CropRecordingFrame(const PreviewFrame& frame, CropRect crop);
std::wstring EncodingArguments(const std::filesystem::path& mkv, std::uint32_t width,
                               std::uint32_t height, int fps, std::wstring_view encoder);
std::wstring RemuxArguments(const std::filesystem::path& mkv, const std::filesystem::path& mp4);
double ValidateVideoProbe(std::string_view output, std::uint64_t frames, int fps);
void ValidateAudioProbe(std::string_view output);

struct FramePacket {
    std::uint64_t timeline_index{};
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
};

// Synchronization belongs to Recorder; this queue owns the bounded/drop policy.
class BoundedFrameQueue {
public:
    explicit BoundedFrameQueue(std::size_t capacity);
    void Push(FramePacket packet);
    FramePacket Pop();
    bool Empty() const noexcept { return packets_.empty(); }
    std::size_t Size() const noexcept { return packets_.size(); }
    std::uint64_t Overflows() const noexcept { return overflows_; }
    void Clear() noexcept;
private:
    std::size_t capacity_;
    std::deque<FramePacket> packets_;
    std::uint64_t overflows_{};
};

class FrameTimeline {
public:
    void Write(const FramePacket& packet,
               const std::function<void(std::span<const std::uint8_t>)>& sink);
    std::uint64_t FramesWritten() const noexcept { return next_index_; }
private:
    std::uint64_t next_index_{};
    std::shared_ptr<const std::vector<std::uint8_t>> previous_;
};

} // namespace yourots::detail
