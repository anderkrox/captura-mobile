#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace yourots {

struct CropRect {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    bool IsFullFrame() const noexcept {
        return x == 0 && y == 0 && width == 0 && height == 0;
    }
};

CropRect ParseCrop(std::wstring_view text);
CropRect ResolveCrop(CropRect crop, std::uint32_t frame_width, std::uint32_t frame_height);
std::vector<std::uint8_t> PackBgraRows(std::span<const std::uint8_t> source,
                                    std::uint32_t width, std::uint32_t height,
                                    std::size_t row_pitch);
std::wstring QuoteCommandArgument(std::wstring_view argument);
int ParseDuration(std::wstring_view text);

} // namespace yourots
