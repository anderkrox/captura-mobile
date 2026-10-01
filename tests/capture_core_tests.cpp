#include "capture_core.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Contract violated");
    }
}

template <typename Action>
void Reject(Action action) {
    bool rejected{};
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected);
}

struct Test {
    std::string name;
    std::function<void()> action;
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace yourots;
    std::vector<Test> tests;
    tests.push_back({"mobile_coordinates", [] {
        const auto crop = ParseCrop(L"410,176,486,864");
        Require(crop.x == 410 && crop.y == 176 && crop.width == 486 && crop.height == 864);
    }});
    const std::vector<std::wstring> invalid_crops{
        L"", L"0,0,486", L"0,0,486,864,1", L"0,0,486,864garbage",
        L"0,0,0,864", L"0,0,486,0", L"-1,0,486,864", L"0,-1,486,864",
        L"0,0,-486,864", L"+1,0,486,864", L"4294967296,0,486,864",
        L"1.5,0,486,864", L"0,,486,864", L"0;0;486;864",
    };
    for (std::size_t index = 0; index < invalid_crops.size(); ++index) {
        tests.push_back({"reject_crop_" + std::to_string(index), [text = invalid_crops[index]] {
            Reject([&] { ParseCrop(text); });
        }});
    }
    tests.push_back({"full_frame", [] {
        const auto crop = ResolveCrop({}, 1920, 1040);
        Require(crop.x == 0 && crop.y == 0 && crop.width == 1920 && crop.height == 1040);
    }});
    tests.push_back({"crop_touching_right_bottom", [] {
        const auto crop = ResolveCrop({1434, 176, 486, 864}, 1920, 1040);
        Require(crop.width == 486 && crop.height == 864);
    }});
    tests.push_back({"single_pixel_at_edge", [] {
        Require(ResolveCrop({1919, 1039, 1, 1}, 1920, 1040).width == 1);
    }});
    const std::vector<CropRect> invalid_bounds{
        {1435, 176, 486, 864}, {410, 177, 486, 864}, {1920, 0, 1, 1},
        {0, 1040, 1, 1}, {410, 0, 0, 864}, {0, 176, 486, 0},
        {0, 0, 4294967295U, 1}, {4294967295U, 0, 2, 1},
    };
    for (std::size_t index = 0; index < invalid_bounds.size(); ++index) {
        tests.push_back({"reject_bounds_" + std::to_string(index), [crop = invalid_bounds[index]] {
            Reject([&] { ResolveCrop(crop, 1920, 1040); });
        }});
    }
    tests.push_back({"reject_empty_frame", [] { Reject([] { ResolveCrop({}, 0, 864); }); }});
    tests.push_back({"padded_rows_keep_bgra_and_remove_padding", [] {
        const std::vector<std::uint8_t> source{
            1,2,3,255, 4,5,6,255, 99,99,99,99,
            7,8,9,255, 10,11,12,255,
        };
        const std::vector<std::uint8_t> expected{1,2,3,255,4,5,6,255,7,8,9,255,10,11,12,255};
        Require(PackBgraRows(source, 2, 2, 12) == expected);
    }});
    tests.push_back({"tight_rows", [] {
        const std::vector<std::uint8_t> source{1,2,3,255,4,5,6,255};
        Require(PackBgraRows(source, 1, 2, 4) == source);
    }});
    tests.push_back({"reject_short_row_pitch", [] {
        Reject([] { PackBgraRows(std::vector<std::uint8_t>(16), 2, 2, 4); });
    }});
    tests.push_back({"reject_truncated_buffer", [] {
        Reject([] { PackBgraRows(std::vector<std::uint8_t>(19), 2, 2, 12); });
    }});
    tests.push_back({"reject_pitch_overflow", [] {
        Reject([] { PackBgraRows({}, 1, 2, std::numeric_limits<std::size_t>::max()); });
    }});
    tests.push_back({"reject_zero_dimensions", [] { Reject([] { PackBgraRows({}, 0, 2, 4); }); }});

    // Use the Windows parser as an independent oracle for path/argument quoting.
    const std::vector<std::wstring> arguments{
        L"", L"plain", L"C:\\recordings\\video.mp4", L"C:\\meus videos\\video.mp4",
        L"C:\\meus videos\\", L"embedded\"quote", L"slash\\\"quote",
        L"two\twords", L"a\\\\\"b\\\\", L"gravação número 1.mp4",
    };
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        tests.push_back({"windows_argument_roundtrip_" + std::to_string(index), [argument = arguments[index]] {
            const auto command = L"program.exe " + QuoteCommandArgument(argument);
            int count{};
            wchar_t** parsed = CommandLineToArgvW(command.c_str(), &count);
            Require(parsed != nullptr);
            const bool matches = count == 2 && argument == parsed[1];
            LocalFree(parsed);
            Require(matches);
        }});
    }
    tests.push_back({"duration_limits", [] {
        Require(ParseDuration(L"1") == 1 && ParseDuration(L"30") == 30 && ParseDuration(L"3600") == 3600);
    }});
    const std::vector<std::wstring> invalid_durations{L"0", L"-1", L"3601", L"30junk", L"2147483647", L""};
    for (std::size_t index = 0; index < invalid_durations.size(); ++index) {
        tests.push_back({"reject_duration_" + std::to_string(index), [text = invalid_durations[index]] {
            Reject([&] { ParseDuration(text); });
        }});
    }

    int failures{};
    std::string cases;
    for (const auto& test : tests) {
        cases += "  <testcase name=\"" + test.name + "\">";
        try {
            test.action();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            cases += "<failure message=\"Contract violated\"/>";
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
        cases += "</testcase>\n";
    }
    if (argc > 1) {
        const std::filesystem::path path = argv[1];
        std::filesystem::create_directories(path.parent_path());
        std::ofstream report(path);
        report << "<testsuite name=\"capture_core\" tests=\"" << tests.size()
               << "\" failures=\"" << failures << "\">\n" << cases << "</testsuite>\n";
        if (!report) { return 2; }
    }
    std::cout << "tests=" << tests.size() << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
