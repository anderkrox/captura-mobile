#include "capture_core.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace yourots {
namespace {
std::uint32_t ParseUnsigned(std::wstring_view text) {
    if (text.empty()) {
        throw std::invalid_argument("Numero vazio.");
    }
    std::uint32_t value{};
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            throw std::invalid_argument("Use somente digitos decimais sem sinal.");
        }
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
            throw std::out_of_range("Numero excede uint32.");
        }
        value = value * 10U + digit;
    }
    return value;
}
} // namespace

CropRect ParseCrop(std::wstring_view text) {
    std::uint32_t fields[4]{};
    for (int index = 0; index < 4; ++index) {
        const auto comma = text.find(L',');
        if ((index < 3 && comma == std::wstring_view::npos) ||
            (index == 3 && comma != std::wstring_view::npos)) {
            throw std::invalid_argument("Formato invalido para --crop. Use X,Y,LARGURA,ALTURA.");
        }
        fields[index] = ParseUnsigned(text.substr(0, comma));
        if (index < 3) {
            text.remove_prefix(comma + 1);
        }
    }
    if (fields[2] == 0 || fields[3] == 0) {
        throw std::invalid_argument("O recorte precisa de largura e altura positivas.");
    }
    return {fields[0], fields[1], fields[2], fields[3]};
}

CropRect ResolveCrop(CropRect crop, std::uint32_t frame_width, std::uint32_t frame_height) {
    if (frame_width == 0 || frame_height == 0) {
        throw std::invalid_argument("Quadro vazio.");
    }
    if (crop.IsFullFrame()) {
        return {0, 0, frame_width, frame_height};
    }
    if (crop.width == 0 || crop.height == 0 ||
        crop.x >= frame_width || crop.y >= frame_height ||
        crop.width > frame_width - crop.x || crop.height > frame_height - crop.y) {
        throw std::out_of_range("Recorte fora do quadro.");
    }
    return crop;
}

std::vector<std::uint8_t> PackBgraRows(std::span<const std::uint8_t> source,
                                    std::uint32_t width, std::uint32_t height,
                                    std::size_t row_pitch) {
    if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / 4U) {
        throw std::invalid_argument("Dimensoes BGRA invalidas.");
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
    if (row_pitch < row_bytes || row_pitch > std::numeric_limits<std::size_t>::max() / height ||
        source.size() < row_pitch * (height - 1U) + row_bytes) {
        throw std::invalid_argument("Buffer ou RowPitch insuficiente.");
    }
    std::vector<std::uint8_t> result(row_bytes * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::copy_n(source.data() + row * row_pitch, row_bytes, result.data() + row * row_bytes);
    }
    return result;
}

std::wstring QuoteCommandArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring result = L"\"";
    std::size_t backslashes{};
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(character == L'\"' ? backslashes * 2 + 1 : backslashes, L'\\');
        result.push_back(character);
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

int ParseDuration(std::wstring_view text) {
    const auto seconds = ParseUnsigned(text);
    if (seconds == 0 || seconds > 3600) {
        throw std::out_of_range("--seconds deve estar entre 1 e 3600.");
    }
    return static_cast<int>(seconds);
}
} // namespace yourots
