#pragma once

#include <windows.h>

#include "capture_core.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace yourots {

struct Calibration {
    bool exists{};
    std::wstring process;
    std::wstring title;
    std::wstring class_name;
    std::uint32_t frame_width{};
    std::uint32_t frame_height{};
    UINT dpi{};
    CropRect crop{};
};

enum class CalibrationCheck { Valid, EmptyFrame, FrameChanged, DpiChanged, OutsideFrame, InvalidRatio };

std::optional<std::uint32_t> ParseCalibrationUInt(std::wstring_view text);
CalibrationCheck CheckCalibration(const Calibration& basis, std::uint32_t width,
                                  std::uint32_t height, UINT dpi);
bool SameCalibrationSource(const Calibration& saved, std::wstring_view process,
                           std::wstring_view title, std::wstring_view class_name);
Calibration LoadCalibration(const std::filesystem::path& path);
void SaveCalibration(const std::filesystem::path& path, const Calibration& calibration);

RECT FitImageRect(const RECT& bounds, std::uint32_t width, std::uint32_t height);
std::optional<POINT> PreviewToFrame(const RECT& image, std::uint32_t width,
                                  std::uint32_t height, POINT point);
CropRect RatioRectFromDrag(POINT anchor, POINT current, std::uint32_t width, std::uint32_t height);
RECT CropToPreviewRect(const RECT& image, std::uint32_t width, std::uint32_t height, CropRect crop);

} // namespace yourots
