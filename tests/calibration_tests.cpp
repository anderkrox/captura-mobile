#include "calibration.h"

#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
namespace {
void Require(bool condition) {
    if (!condition) { throw std::runtime_error("Contract violated"); }
}
template <typename Action> void Reject(Action action) {
    bool rejected{};
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected);
}
bool Equal(RECT a, RECT b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
bool Equal(yourots::CropRect a, yourots::CropRect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
yourots::Calibration Basis() {
    return {true, L"msedge.exe", L"Yourots — ação 🎮", L"Chrome_WidgetWin_1", 1920, 1040, 96,
            {410, 176, 486, 864}};
}
struct Test { std::string name; std::function<void()> action; };
struct Scratch {
    fs::path directory;
    unsigned sequence{};
    fs::path Next() { return directory / (std::to_wstring(sequence++) + L".ini"); }
    ~Scratch() { std::error_code ignored; fs::remove_all(directory, ignored); }
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace yourots;
    const fs::path report = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / L"calibration.xml";
    Scratch scratch{report.parent_path() / (L"calibration-fixtures-" + std::to_wstring(GetCurrentProcessId()))};
    fs::create_directories(scratch.directory);
    std::vector<Test> tests;
    tests.push_back({"parse_uint_zero_and_max", [] {
        Require(ParseCalibrationUInt(L"0") == 0U && ParseCalibrationUInt(L"4294967295") == UINT32_MAX);
    }});
    tests.push_back({"parse_uint_leading_zeroes", [] { Require(ParseCalibrationUInt(L"000486") == 486U); }});
    const std::vector<std::wstring> invalid_numbers{
        L"", L"-1", L"+1", L" 1", L"1 ", L"1\t", L"\n1", L"486px", L"1.5", L"0x10",
        L"4294967296", L"999999999999999999999999999999", L"１", std::wstring(L"1\0x", 3)};
    for (std::size_t i = 0; i < invalid_numbers.size(); ++i) {
        tests.push_back({"reject_uint_" + std::to_string(i), [value = invalid_numbers[i]] {
            Require(!ParseCalibrationUInt(value));
        }});
    }
    tests.push_back({"valid_mobile_basis", [] {
        Require(CheckCalibration(Basis(), 1920, 1040, 96) == CalibrationCheck::Valid);
    }});
    tests.push_back({"frame_changes_even_if_crop_still_fits", [] {
        Require(CheckCalibration(Basis(), 2000, 1100, 96) == CalibrationCheck::FrameChanged);
    }});
    tests.push_back({"height_change", [] {
        Require(CheckCalibration(Basis(), 1920, 1100, 96) == CalibrationCheck::FrameChanged);
    }});
    tests.push_back({"dpi_change", [] {
        Require(CheckCalibration(Basis(), 1920, 1040, 144) == CalibrationCheck::DpiChanged);
    }});
    tests.push_back({"unavailable_dpi", [] {
        Require(CheckCalibration(Basis(), 1920, 1040, 0) == CalibrationCheck::DpiChanged);
    }});
    tests.push_back({"empty_frame", [] {
        Require(CheckCalibration(Basis(), 0, 1040, 96) == CalibrationCheck::EmptyFrame);
    }});
    tests.push_back({"frame_larger_than_native_limit", [] {
        Require(CheckCalibration(Basis(), UINT32_MAX, 1040, 96) == CalibrationCheck::EmptyFrame);
    }});
    const std::vector<CropRect> invalid_crops{
        {}, {0, 0, 0, 16}, {0, 0, 9, 0}, {1435, 176, 486, 864}, {410, 177, 486, 864},
        {UINT32_MAX, 0, 9, 16}, {0, 0, UINT32_MAX, UINT32_MAX}};
    for (std::size_t i = 0; i < invalid_crops.size(); ++i) {
        tests.push_back({"invalid_crop_" + std::to_string(i), [crop = invalid_crops[i]] {
            auto basis = Basis(); basis.crop = crop;
            Require(CheckCalibration(basis, 1920, 1040, 96) == CalibrationCheck::OutsideFrame);
        }});
    }
    tests.push_back({"reject_non_mobile_ratio", [] {
        auto basis = Basis(); basis.crop = {0, 0, 486, 863};
        Require(CheckCalibration(basis, 1920, 1040, 96) == CalibrationCheck::InvalidRatio);
    }});
    tests.push_back({"minimum_ratio_and_frame_edges", [] {
        auto basis = Basis(); basis.crop = {1911, 1024, 9, 16};
        Require(CheckCalibration(basis, 1920, 1040, 96) == CalibrationCheck::Valid);
    }});
    tests.push_back({"identity_matches_without_hwnd", [] {
        const auto b = Basis(); Require(SameCalibrationSource(b, b.process, b.title, b.class_name));
    }});
    for (int field = 0; field < 4; ++field) {
        tests.push_back({"reject_identity_mismatch_" + std::to_string(field), [field] {
            auto b = Basis();
            if (field == 0) { b.process = L"other.exe"; }
            if (field == 1) { b.title = L"Other tab"; }
            if (field == 2) { b.class_name = L"OtherClass"; }
            if (field == 3) { b.exists = false; }
            const auto original = Basis();
            Require(!SameCalibrationSource(b, original.process, original.title, original.class_name));
        }});
    }
    tests.push_back({"fit_portrait_with_horizontal_letterbox", [] {
        Require(Equal(FitImageRect({16, 72, 616, 672}, 486, 864), {147, 72, 484, 672}));
    }});
    tests.push_back({"fit_landscape_with_vertical_letterbox", [] {
        Require(Equal(FitImageRect({10, 20, 610, 620}, 1200, 600), {10, 170, 610, 470}));
    }});
    tests.push_back({"fit_native_size", [] {
        Require(Equal(FitImageRect({20, 30, 506, 894}, 486, 864), {20, 30, 506, 894}));
    }});
    tests.push_back({"fit_negative_monitor_coordinates", [] {
        Require(Equal(FitImageRect({-600, -600, 0, 0}, 1200, 600), {-600, -450, 0, -150}));
    }});
    tests.push_back({"fit_empty_or_inverted_bounds", [] {
        Require(Equal(FitImageRect({10, 10, 10, 100}, 600, 964), {}));
        Require(Equal(FitImageRect({10, 100, 100, 10}, 600, 964), {}));
        Require(Equal(FitImageRect({0, 0, 100, 100}, 0, 964), {}));
    }});
    tests.push_back({"fit_one_pixel", [] {
        Require(Equal(FitImageRect({0, 0, 1, 1}, 600, 964), {0, 0, 1, 1}));
    }});
    tests.push_back({"map_scaled_center", [] {
        const auto p = PreviewToFrame({100, 50, 400, 532}, 600, 964, {250, 291});
        Require(p && p->x == 300 && p->y == 482);
    }});
    tests.push_back({"map_top_left", [] {
        const auto p = PreviewToFrame({100, 50, 400, 532}, 600, 964, {100, 50});
        Require(p && p->x == 0 && p->y == 0);
    }});
    tests.push_back({"map_last_visible_pixel", [] {
        const auto p = PreviewToFrame({100, 50, 400, 532}, 600, 964, {399, 531});
        Require(p && p->x == 598 && p->y == 962);
    }});
    const POINT outside[]{{99, 50}, {100, 49}, {400, 50}, {100, 532}};
    for (std::size_t i = 0; i < std::size(outside); ++i) {
        tests.push_back({"reject_letterbox_or_exclusive_edge_" + std::to_string(i), [p = outside[i]] {
            Require(!PreviewToFrame({100, 50, 400, 532}, 600, 964, p));
        }});
    }
    tests.push_back({"map_invalid_geometry", [] {
        Require(!PreviewToFrame({}, 600, 964, {}));
        Require(!PreviewToFrame({0, 0, 10, 10}, 0, 964, {}));
    }});
    tests.push_back({"map_negative_coordinates", [] {
        const auto p = PreviewToFrame({-400, -532, -100, -50}, 600, 964, {-250, -291});
        Require(p && p->x == 300 && p->y == 482);
    }});
    tests.push_back({"crop_projected_to_preview", [] {
        Require(Equal(CropToPreviewRect({100, 50, 400, 532}, 600, 964, {60, 50, 486, 864}),
                      {130, 75, 373, 507}));
    }});
    tests.push_back({"crop_projection_rejects_overflow", [] {
        Require(Equal(CropToPreviewRect({0, 0, 100, 100}, 600, 964, {UINT32_MAX, 0, 9, 16}), {}));
    }});
    const POINT directions[]{{509, 516}, {491, 516}, {509, 484}, {491, 484}};
    const CropRect expected[]{{500, 500, 9, 16}, {491, 500, 9, 16}, {500, 484, 9, 16}, {491, 484, 9, 16}};
    for (std::size_t i = 0; i < std::size(directions); ++i) {
        tests.push_back({"drag_quadrant_" + std::to_string(i), [p = directions[i], crop = expected[i]] {
            Require(Equal(RatioRectFromDrag({500, 500}, p, 1000, 1000), crop));
        }});
    }
    tests.push_back({"drag_zero_area", [] { Require(RatioRectFromDrag({50, 50}, {50, 50}, 600, 964).width == 0); }});
    tests.push_back({"drag_clipped_by_frame", [] {
        Require(Equal(RatioRectFromDrag({57, 50}, {5000, 5000}, 600, 964), {57, 50, 513, 912}));
    }});
    tests.push_back({"drag_invalid_anchor", [] {
        Require(RatioRectFromDrag({-1, 0}, {500, 500}, 600, 964).width == 0);
        Require(RatioRectFromDrag({600, 0}, {500, 500}, 600, 964).width == 0);
    }});
    tests.push_back({"drag_extreme_pointer_preserves_bounds_and_ratio", [] {
        const auto c = RatioRectFromDrag({500, 500}, {LONG_MIN, LONG_MAX}, 1000, 1000);
        Require(c.width > 0 && c.width * 16ULL == c.height * 9ULL);
        Require(c.x + c.width <= 1000 && c.y + c.height <= 1000);
    }});
    tests.push_back({"drag_grid_preserves_ratio_and_anchor", [] {
        for (LONG x = 0; x < 600; x += 19) {
            for (LONG y = 0; y < 964; y += 23) {
                const auto c = RatioRectFromDrag({300, 482}, {x, y}, 600, 964);
                Require(c.width * 16ULL == c.height * 9ULL);
                Require(c.x + c.width <= 600 && c.y + c.height <= 964);
                Require(c.x <= 300 && c.x + c.width >= 300 && c.y <= 482 && c.y + c.height >= 482);
            }
        }
    }});
    tests.push_back({"unicode_persistence_roundtrip", [&] {
        const auto path = scratch.Next(); const auto b = Basis(); SaveCalibration(path, b);
        const auto loaded = LoadCalibration(path);
        Require(loaded.exists && SameCalibrationSource(loaded, b.process, b.title, b.class_name));
        Require(Equal(loaded.crop, b.crop) && loaded.dpi == 96 && loaded.frame_width == 1920 && loaded.frame_height == 1040);
        std::ifstream file(path, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(file)), {});
        Require(bytes.size() > 2 && bytes.substr(0, 2) == "\xff\xfe");
        Require(bytes.find(std::string("h\0w\0n\0d", 7)) == std::string::npos);
    }});
    tests.push_back({"quoted_and_spaced_title_roundtrip", [&] {
        const auto path = scratch.Next(); auto b = Basis(); b.title = L"  \"Jogo = Yourots\"  ";
        SaveCalibration(path, b); Require(LoadCalibration(path).title == b.title);
    }});
    tests.push_back({"long_title_roundtrip_without_truncation", [&] {
        const auto path = scratch.Next(); auto b = Basis(); b.title = std::wstring(5000, L'á');
        SaveCalibration(path, b); Require(LoadCalibration(path).title == b.title);
    }});
    tests.push_back({"missing_file_is_not_calibration", [&] { Require(!LoadCalibration(scratch.Next()).exists); }});
    const wchar_t* fields[]{L"process", L"title", L"class", L"frameWidth", L"frameHeight", L"dpi",
                            L"cropX", L"cropY", L"cropWidth", L"cropHeight"};
    for (std::size_t i = 0; i < std::size(fields); ++i) {
        tests.push_back({"reject_missing_ini_field_" + std::to_string(i), [&, key = fields[i]] {
            const auto path = scratch.Next(); SaveCalibration(path, Basis());
            Require(WritePrivateProfileStringW(L"calibration", key, nullptr, path.c_str()));
            Require(!LoadCalibration(path).exists);
        }});
    }
    const wchar_t* corrupt_values[]{L"-1", L"4294967296", L"12junk", L"0"};
    for (std::size_t i = 0; i < std::size(corrupt_values); ++i) {
        tests.push_back({"reject_corrupt_saved_dimension_" + std::to_string(i), [&, value = corrupt_values[i]] {
            const auto path = scratch.Next(); SaveCalibration(path, Basis());
            Require(WritePrivateProfileStringW(L"calibration", L"frameWidth", value, path.c_str()));
            Require(!LoadCalibration(path).exists);
        }});
    }
    tests.push_back({"reject_corrupt_saved_ratio", [&] {
        const auto path = scratch.Next(); SaveCalibration(path, Basis());
        Require(WritePrivateProfileStringW(L"calibration", L"cropHeight", L"863", path.c_str()));
        Require(!LoadCalibration(path).exists);
    }});
    tests.push_back({"invalid_save_preserves_previous_file", [&] {
        const auto path = scratch.Next(); SaveCalibration(path, Basis()); auto b = Basis(); b.crop.x = 1900;
        Reject([&] { SaveCalibration(path, b); }); Require(Equal(LoadCalibration(path).crop, Basis().crop));
    }});
    tests.push_back({"reject_identity_ini_injection", [&] {
        auto b = Basis(); b.title = L"game\n[calibration]\ncropX=1";
        Reject([&] { SaveCalibration(scratch.Next(), b); });
    }});
    tests.push_back({"reject_empty_identity", [&] {
        auto b = Basis(); b.process.clear(); Reject([&] { SaveCalibration(scratch.Next(), b); });
    }});
    tests.push_back({"failed_replace_removes_temporary_file", [&] {
        const auto path = scratch.Next(); fs::create_directory(path);
        Reject([&] { SaveCalibration(path, Basis()); });
        for (const auto& entry : fs::directory_iterator(scratch.directory)) {
            Require(entry.path().extension() != L".tmp");
        }
    }});
    tests.push_back({"save_replaces_and_reload_reads_current_values", [&] {
        const auto path = scratch.Next(); SaveCalibration(path, Basis()); auto b = Basis(); b.crop.x = 411;
        SaveCalibration(path, b); Require(LoadCalibration(path).crop.x == 411);
    }});
    int failures{};
    std::string cases;
    for (const auto& test : tests) {
        cases += "  <testcase name=\"" + test.name + "\">";
        try { test.action(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) {
            ++failures; cases += "<failure message=\"Contract violated\"/>";
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
        cases += "</testcase>\n";
    }
    fs::create_directories(report.parent_path());
    std::ofstream output(report);
    output << "<testsuite name=\"calibration\" tests=\"" << tests.size() << "\" failures=\"" << failures
           << "\">\n" << cases << "</testsuite>\n";
    if (!output) { return 2; }
    std::cout << "tests=" << tests.size() << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
