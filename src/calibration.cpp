#include "calibration.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace fs = std::filesystem;

namespace yourots {
namespace {
std::wstring ReadIniString(const fs::path& path, const wchar_t* key) {
    for (DWORD size = 256; size <= 32768; size *= 2) {
        std::wstring buffer(size, L'\0');
        const DWORD length = GetPrivateProfileStringW(
            L"calibration", key, L"", buffer.data(), size, path.c_str());
        if (length < size - 1) {
            buffer.resize(length);
            return buffer;
        }
    }
    return {};
}

bool ValidIdentity(std::wstring_view value) {
    return !value.empty() && value.size() < 16000 &&
        value.find_first_of(std::wstring_view(L"\r\n\0", 3)) == std::wstring_view::npos;
}

void WriteIniString(const fs::path& path, const wchar_t* key, std::wstring_view value) {
    // The profile API removes outer quotes on read. Quote once to preserve
    // literal quotes and spaces in browser titles.
    const std::wstring quoted = L"\"" + std::wstring(value) + L"\"";
    if (!WritePrivateProfileStringW(L"calibration", key, quoted.c_str(), path.c_str())) {
        throw std::runtime_error("Nao foi possivel salvar as configuracoes.");
    }
}

bool ValidFrame(std::uint32_t width, std::uint32_t height) {
    return width > 0 && height > 0 && width <= LONG_MAX && height <= LONG_MAX;
}
} // namespace

std::optional<std::uint32_t> ParseCalibrationUInt(std::wstring_view text) {
    if (text.empty()) { return std::nullopt; }
    std::uint32_t result{};
    for (const wchar_t digit : text) {
        if (digit < L'0' || digit > L'9') { return std::nullopt; }
        const auto value = static_cast<std::uint32_t>(digit - L'0');
        if (result > (std::numeric_limits<std::uint32_t>::max() - value) / 10) {
            return std::nullopt;
        }
        result = result * 10 + value;
    }
    return result;
}

CalibrationCheck CheckCalibration(const Calibration& basis, std::uint32_t width,
                                  std::uint32_t height, UINT dpi) {
    if (!ValidFrame(width, height)) { return CalibrationCheck::EmptyFrame; }
    if (basis.frame_width != width || basis.frame_height != height) {
        return CalibrationCheck::FrameChanged;
    }
    if (dpi == 0 || basis.dpi == 0 || basis.dpi != dpi) { return CalibrationCheck::DpiChanged; }
    if (basis.crop.width == 0 || basis.crop.height == 0) { return CalibrationCheck::OutsideFrame; }
    try { ResolveCrop(basis.crop, width, height); }
    catch (const std::exception&) { return CalibrationCheck::OutsideFrame; }
    if (basis.crop.width * 16ULL != basis.crop.height * 9ULL) {
        return CalibrationCheck::InvalidRatio;
    }
    return CalibrationCheck::Valid;
}

bool SameCalibrationSource(const Calibration& saved, std::wstring_view process,
                           std::wstring_view title, std::wstring_view class_name) {
    return saved.exists && saved.process == process && saved.title == title && saved.class_name == class_name;
}

Calibration LoadCalibration(const fs::path& path) {
    const auto absolute = fs::absolute(path);
    Calibration result;
    result.process = ReadIniString(absolute, L"process");
    result.title = ReadIniString(absolute, L"title");
    result.class_name = ReadIniString(absolute, L"class");
    const wchar_t* keys[]{L"frameWidth", L"frameHeight", L"dpi", L"cropX", L"cropY", L"cropWidth", L"cropHeight"};
    std::uint32_t values[7]{};
    for (std::size_t i = 0; i < std::size(keys); ++i) {
        const auto value = ParseCalibrationUInt(ReadIniString(absolute, keys[i]));
        if (!value) { return result; }
        values[i] = *value;
    }
    result.frame_width = values[0];
    result.frame_height = values[1];
    result.dpi = values[2];
    result.crop = {values[3], values[4], values[5], values[6]};
    result.exists = ValidIdentity(result.process) && ValidIdentity(result.title) && ValidIdentity(result.class_name) &&
        CheckCalibration(result, result.frame_width, result.frame_height, result.dpi) == CalibrationCheck::Valid;
    return result;
}

void SaveCalibration(const fs::path& path, const Calibration& calibration) {
    if (!ValidIdentity(calibration.process) || !ValidIdentity(calibration.title) ||
        !ValidIdentity(calibration.class_name) ||
        CheckCalibration(calibration, calibration.frame_width, calibration.frame_height, calibration.dpi)
            != CalibrationCheck::Valid) {
        throw std::runtime_error("A calibracao atual nao esta valida para ser salva.");
    }
    const auto absolute = fs::absolute(path);
    fs::create_directories(absolute.parent_path());
    static std::atomic_uint counter{};
    const fs::path temporary = absolute.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(counter.fetch_add(1)) + L".tmp";
    try {
        // A UTF-16 BOM makes the Windows profile API preserve Unicode.
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write("\xff\xfe", 2);
        file.close();
        if (!file) { throw std::runtime_error("Nao foi possivel criar as configuracoes."); }
        WriteIniString(temporary, L"process", calibration.process);
        WriteIniString(temporary, L"title", calibration.title);
        WriteIniString(temporary, L"class", calibration.class_name);
        const wchar_t* keys[]{L"frameWidth", L"frameHeight", L"dpi", L"cropX", L"cropY", L"cropWidth", L"cropHeight"};
        const std::uint32_t values[]{calibration.frame_width, calibration.frame_height, calibration.dpi,
            calibration.crop.x, calibration.crop.y, calibration.crop.width, calibration.crop.height};
        for (std::size_t i = 0; i < std::size(keys); ++i) {
            WriteIniString(temporary, keys[i], std::to_wstring(values[i]));
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
        if (!MoveFileExW(temporary.c_str(), absolute.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Nao foi possivel substituir as configuracoes.");
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

RECT FitImageRect(const RECT& bounds, std::uint32_t width, std::uint32_t height) {
    if (!ValidFrame(width, height) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) { return {}; }
    const auto available_width = static_cast<std::int64_t>(bounds.right) - bounds.left;
    const auto available_height = static_cast<std::int64_t>(bounds.bottom) - bounds.top;
    const double scale = std::min(static_cast<double>(available_width) / width,
                                  static_cast<double>(available_height) / height);
    const auto draw_width = std::max<std::int64_t>(1, static_cast<std::int64_t>(width * scale));
    const auto draw_height = std::max<std::int64_t>(1, static_cast<std::int64_t>(height * scale));
    const auto left = bounds.left + (available_width - draw_width) / 2;
    const auto top = bounds.top + (available_height - draw_height) / 2;
    return {static_cast<LONG>(left), static_cast<LONG>(top),
            static_cast<LONG>(left + draw_width), static_cast<LONG>(top + draw_height)};
}

std::optional<POINT> PreviewToFrame(const RECT& image, std::uint32_t width,
                                  std::uint32_t height, POINT point) {
    if (!ValidFrame(width, height) || image.right <= image.left || image.bottom <= image.top ||
        point.x < image.left || point.x >= image.right || point.y < image.top || point.y >= image.bottom) {
        return std::nullopt;
    }
    const auto draw_width = static_cast<std::int64_t>(image.right) - image.left;
    const auto draw_height = static_cast<std::int64_t>(image.bottom) - image.top;
    return POINT{
        static_cast<LONG>((static_cast<std::int64_t>(point.x) - image.left) * width / draw_width),
        static_cast<LONG>((static_cast<std::int64_t>(point.y) - image.top) * height / draw_height)};
}

CropRect RatioRectFromDrag(POINT anchor, POINT current, std::uint32_t width, std::uint32_t height) {
    if (!ValidFrame(width, height) || anchor.x < 0 || anchor.y < 0 ||
        static_cast<std::uint32_t>(anchor.x) >= width || static_cast<std::uint32_t>(anchor.y) >= height) { return {}; }
    const auto dx = static_cast<std::int64_t>(current.x) - anchor.x;
    const auto dy = static_cast<std::int64_t>(current.y) - anchor.y;
    const auto desired = std::max((std::abs(dx) + 8) / 9, (std::abs(dy) + 15) / 16);
    const auto horizontal = dx >= 0 ? width - static_cast<std::uint32_t>(anchor.x) : static_cast<std::uint32_t>(anchor.x);
    const auto vertical = dy >= 0 ? height - static_cast<std::uint32_t>(anchor.y) : static_cast<std::uint32_t>(anchor.y);
    const auto units = static_cast<std::uint32_t>(std::min<std::int64_t>({desired, horizontal / 9U, vertical / 16U}));
    const auto crop_width = units * 9U, crop_height = units * 16U;
    return {static_cast<std::uint32_t>(anchor.x) - (dx < 0 ? crop_width : 0U),
            static_cast<std::uint32_t>(anchor.y) - (dy < 0 ? crop_height : 0U), crop_width, crop_height};
}

RECT CropToPreviewRect(const RECT& image, std::uint32_t width, std::uint32_t height, CropRect crop) {
    if (!ValidFrame(width, height) || image.right <= image.left || image.bottom <= image.top) { return {}; }
    try { crop = ResolveCrop(crop, width, height); }
    catch (const std::exception&) { return {}; }
    const auto draw_width = static_cast<std::int64_t>(image.right) - image.left;
    const auto draw_height = static_cast<std::int64_t>(image.bottom) - image.top;
    return {
        static_cast<LONG>(image.left + crop.x * draw_width / width),
        static_cast<LONG>(image.top + crop.y * draw_height / height),
        static_cast<LONG>(image.left + (static_cast<std::int64_t>(crop.x) + crop.width) * draw_width / width),
        static_cast<LONG>(image.top + (static_cast<std::int64_t>(crop.y) + crop.height) * draw_height / height)};
}
} // namespace yourots
