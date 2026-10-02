#include "recorder_core.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace yourots;
using namespace yourots::detail;
namespace {
void Require(bool condition) { if (!condition) { throw std::runtime_error("Contract violated"); } }
template <typename Action> void Reject(Action action) {
    bool rejected{};
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected);
}
struct Test { std::string name; std::function<void()> action; };
FramePacket Packet(std::uint64_t index, std::uint8_t value) {
    return {index, std::make_shared<const std::vector<std::uint8_t>>(4, value)};
}
std::map<std::string, std::string> ValidProbe() {
    return {{"codec_name", "h264"}, {"width", "1080"}, {"height", "1920"},
            {"pix_fmt", "yuv420p"}, {"r_frame_rate", "30/1"}, {"avg_frame_rate", "30/1"},
            {"sample_aspect_ratio", "1:1"}, {"color_range", "tv"}, {"color_space", "bt709"},
            {"color_transfer", "bt709"}, {"color_primaries", "bt709"},
            {"nb_read_frames", "60"}, {"duration", "2.000000"}};
}
std::string ProbeText(const std::map<std::string, std::string>& values, std::string_view newline = "\n") {
    std::string result;
    for (const auto& [key, value] : values) { result += key + "=" + value + std::string(newline); }
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<Test> tests;
    tests.push_back({"short_video_probe_keeps_a_bounded_budget", [] {
        Require(MediaVerificationTimeoutMilliseconds(60, 30) == 32000);
    }});
    tests.push_back({"fractional_video_probe_rounds_up", [] {
        Require(MediaVerificationTimeoutMilliseconds(1, 30) == 31000);
    }});
    tests.push_back({"thirty_minute_probe_allows_full_decode", [] {
        Require(MediaVerificationTimeoutMilliseconds(54000, 30) > 30000);
        Require(MediaVerificationTimeoutMilliseconds(54000, 30) == 600000);
    }});
    tests.push_back({"unknown_recovery_duration_has_a_finite_budget", [] {
        Require(MediaVerificationTimeoutMilliseconds(0, 30) == 600000);
    }});
    tests.push_back({"probe_budget_caps_before_overflow", [] {
        Require(MediaVerificationTimeoutMilliseconds(std::numeric_limits<std::uint64_t>::max(), 30) == 600000);
    }});
    tests.push_back({"probe_budget_cap_boundary", [] {
        Require(MediaVerificationTimeoutMilliseconds(569 * 30, 30) == 599000);
        Require(MediaVerificationTimeoutMilliseconds(570 * 30, 30) == 600000);
    }});
    tests.push_back({"probe_budget_rejects_invalid_frame_rates", [] {
        for (int fps : {0, -1, 29, 60, INT_MAX}) {
            Reject([&] { MediaVerificationTimeoutMilliseconds(60, fps); });
        }
    }});
    tests.push_back({"valid_settings_and_unicode_mp4", [] {
        for (auto cap : {1U, 4U, 32U}) { ValidateRecorderSettings(30, cap, L"pasta com ação 🎮/vídeo.MP4"); }
    }});
    for (int fps : {-1, 0, 1, 29, 31, 60, INT_MAX}) {
        tests.push_back({"reject_fps_" + std::to_string(fps), [fps] {
            Reject([&] { ValidateRecorderSettings(fps, 4, L"video.mp4"); });
        }});
    }
    for (std::size_t cap : {0ULL, 33ULL, std::numeric_limits<std::size_t>::max()}) {
        tests.push_back({"reject_queue_capacity_" + std::to_string(cap), [cap] {
            Reject([&] { ValidateRecorderSettings(30, cap, L"video.mp4"); });
            Reject([&] { BoundedFrameQueue queue(cap); });
        }});
    }
    const std::vector<fs::path> bad_paths{L"", L"folder/", L"video", L"video.mkv", L"video.mp4.tmp"};
    for (std::size_t i = 0; i < bad_paths.size(); ++i) {
        tests.push_back({"reject_output_path_" + std::to_string(i), [path = bad_paths[i]] {
            Reject([&] { ValidateRecorderSettings(30, 4, path); });
        }});
    }
    tests.push_back({"crop_preserves_bgra_channels_rows_and_edges", [] {
        PreviewFrame frame{3, 3, {1,2,3,4, 5,6,7,8, 9,10,11,12,
                                  13,14,15,16, 17,18,19,20, 21,22,23,24,
                                  25,26,27,28, 29,30,31,32, 33,34,35,36}, 1};
        Require(CropRecordingFrame(frame, {1,1,2,2}) ==
                std::vector<std::uint8_t>{17,18,19,20,21,22,23,24,29,30,31,32,33,34,35,36});
        Require(CropRecordingFrame(frame, {2,2,1,1}) == std::vector<std::uint8_t>{33,34,35,36});
        Require(CropRecordingFrame(frame, {}) == frame.bgra);
    }});
    tests.push_back({"crop_rejects_truncated_frame_even_outside_crop", [] {
        PreviewFrame frame{3,3,std::vector<std::uint8_t>(35),1};
        Reject([&] { CropRecordingFrame(frame, {0,0,1,1}); });
    }});
    const std::vector<CropRect> bad_crops{{3,0,1,1}, {0,3,1,1}, {0,0,4,1}, {0,0,1,4},
        {0,0,1,0}, {UINT32_MAX,0,1,1}};
    for (std::size_t i = 0; i < bad_crops.size(); ++i) {
        tests.push_back({"reject_crop_" + std::to_string(i), [crop = bad_crops[i]] {
            Reject([&] { CropRecordingFrame({3,3,std::vector<std::uint8_t>(36),1}, crop); });
        }});
    }
    tests.push_back({"crop_rejects_empty_and_overflow_dimensions", [] {
        Reject([] { CropRecordingFrame({}, {}); });
        Reject([] { CropRecordingFrame({UINT32_MAX,UINT32_MAX,{},1}, {}); });
    }});
    for (const std::wstring encoder : {L"h264_nvenc", L"libx264"}) {
        tests.push_back({encoder == L"libx264" ? "cpu_encoding_contract" : "nvenc_encoding_contract", [encoder] {
            const fs::path output = L"C:\\vídeos com espaços 🎮\\recording.mkv";
            const auto command = L"ffmpeg.exe " + EncodingArguments(output, 486, 864, 30, encoder);
            int count{};
            auto raw = CommandLineToArgvW(command.c_str(), &count);
            Require(raw != nullptr);
            std::vector<std::wstring> args(raw, raw + count);
            LocalFree(raw);
            auto value = [&](const wchar_t* key) -> std::wstring {
                auto it = std::find(args.begin(), args.end(), key);
                Require(it != args.end() && it + 1 != args.end()); return *(it + 1);
            };
            Require(value(L"-pixel_format") == L"bgra" && value(L"-video_size") == L"486x864");
            Require(value(L"-framerate") == L"30" && value(L"-i") == L"pipe:0");
            Require(value(L"-c:v") == encoder && args.back() == output.wstring());
            Require(std::find(args.begin(), args.end(), L"-an") != args.end());
            Require(std::find(args.begin(), args.end(), L"-n") != args.end());
            Require(std::find(args.begin(), args.end(), L"-y") == args.end());
            Require(args[args.size() - 2] == L"matroska");
            Require(value(L"-vf") == L"scale=1080:1920:flags=lanczos:out_color_matrix=bt709:out_range=tv,"
                L"setsar=1,format=yuv420p,setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709");
            if (encoder == L"libx264") { Require(value(L"-preset") == L"veryfast" && value(L"-crf") == L"18"); }
            else { Require(value(L"-preset") == L"p5" && value(L"-cq") == L"18"); }
        }});
    }
    tests.push_back({"remux_copies_only_video_with_exact_timestamps_and_faststart", [] {
        const fs::path mkv = L"C:\\ação 🎮\\temporary recording.mkv";
        const fs::path mp4 = L"C:\\ação 🎮\\final video.mp4";
        const auto command = L"ffmpeg.exe " + RemuxArguments(mkv,mp4);
        int count{}; auto raw = CommandLineToArgvW(command.c_str(),&count); Require(raw != nullptr);
        std::vector<std::wstring> args(raw,raw+count); LocalFree(raw);
        auto value = [&](const wchar_t* key) {
            auto it = std::find(args.begin(),args.end(),key); Require(it != args.end() && it+1 != args.end()); return *(it+1);
        };
        Require(value(L"-i") == mkv.wstring() && args.back() == mp4.wstring());
        Require(value(L"-c:v") == L"copy" && value(L"-map") == L"0:v:0");
        Require(value(L"-movflags") == L"+faststart" && value(L"-video_track_timescale") == L"15360");
        Require(value(L"-bsf:v") == L"setts=prescale=1:pts=round(PTS/512)*512:dts=round(DTS/512)*512:duration=512:time_base=1/15360");
        Require(std::find(args.begin(),args.end(),L"-an") != args.end());
        Require(std::find(args.begin(),args.end(),L"-n") != args.end());
        Require(std::find(args.begin(),args.end(),L"-y") == args.end());
    }});
    tests.push_back({"reject_invalid_encoding_preset", [] {
        Reject([] { EncodingArguments(L"v.mkv",0,864,30,L"libx264"); });
        Reject([] { EncodingArguments(L"v.mkv",486,0,30,L"libx264"); });
        Reject([] { EncodingArguments(L"v.mkv",486,864,60,L"libx264"); });
        Reject([] { EncodingArguments(L"v.mkv",486,864,30,L"injected -an"); });
    }});
    tests.push_back({"valid_ffprobe_lf_crlf_and_duration_rounding", [] {
        Require(ValidateVideoProbe(ProbeText(ValidProbe()),60,30) == 2.0);
        auto values = ValidProbe(); values["duration"] = "2.033";
        Require(ValidateVideoProbe(ProbeText(values,"\r\n"),60,30) == 2.033);
    }});
    for (const auto& [key, value] : ValidProbe()) {
        tests.push_back({"reject_missing_probe_" + key, [key] {
            auto values = ValidProbe(); values.erase(key);
            Reject([&] { ValidateVideoProbe(ProbeText(values),60,30); });
        }});
        tests.push_back({"reject_wrong_probe_" + key, [key] {
            auto values = ValidProbe(); values[key] = "wrong";
            Reject([&] { ValidateVideoProbe(ProbeText(values),60,30); });
        }});
    }
    const std::vector<std::string> invalid_durations{"nan", "NaN", "inf", "-inf", "0", "-2", "",
        "2seconds", " 2", "2 ", "N/A", "2,0", "1.87", "2.13", "1e999"};
    for (std::size_t i = 0; i < invalid_durations.size(); ++i) {
        tests.push_back({"reject_probe_duration_" + std::to_string(i), [value = invalid_durations[i]] {
            auto values = ValidProbe(); values["duration"] = value;
            Reject([&] { ValidateVideoProbe(ProbeText(values),60,30); });
        }});
    }
    tests.push_back({"reject_ambiguous_probe_duplicate_or_malformed", [] {
        Reject([] { ValidateVideoProbe(ProbeText(ValidProbe()) + "duration=2\n",60,30); });
        Reject([] { ValidateVideoProbe(ProbeText(ValidProbe()) + "unexpected\n",60,30); });
    }});
    tests.push_back({"reject_probe_frame_count_mismatch_or_invalid_timeline", [] {
        Reject([] { ValidateVideoProbe(ProbeText(ValidProbe()),61,30); });
        Reject([] { ValidateVideoProbe(ProbeText(ValidProbe()),0,30); });
        Reject([] { ValidateVideoProbe(ProbeText(ValidProbe()),60,60); });
    }});
    tests.push_back({"silent_audio_probe_accepts_only_whitespace", [] {
        ValidateAudioProbe(""); ValidateAudioProbe("\r\n\t ");
        Reject([] { ValidateAudioProbe("0\n"); }); Reject([] { ValidateAudioProbe("1,2\n"); });
    }});
    for (std::size_t cap : {1U,4U,32U}) {
        tests.push_back({"bounded_queue_drops_oldest_" + std::to_string(cap), [cap] {
            BoundedFrameQueue queue(cap);
            for (unsigned i = 0; i < 1000; ++i) {
                queue.Push(Packet(i,static_cast<std::uint8_t>(i)));
                Require(queue.Size() <= cap);
            }
            Require(queue.Size() == cap && queue.Overflows() == 1000 - cap);
            for (auto i = 1000 - cap; i < 1000; ++i) { Require(queue.Pop().timeline_index == i); }
            Require(queue.Empty()); Reject([&] { queue.Pop(); });
            queue.Push(Packet(0,1)); queue.Clear();
            Require(queue.Empty() && queue.Overflows() == 0);
        }});
    }
    tests.push_back({"queue_releases_dropped_frame_memory", [] {
        BoundedFrameQueue queue(1); auto first = Packet(0,1);
        std::weak_ptr<const std::vector<std::uint8_t>> weak = first.pixels;
        queue.Push(std::move(first)); queue.Push(Packet(1,2)); Require(weak.expired());
        Reject([&] { queue.Push({2,{}}); });
        Reject([&] { queue.Push({2,std::make_shared<const std::vector<std::uint8_t>>()}); });
    }});
    tests.push_back({"timeline_repeats_previous_across_queue_gaps", [] {
        FrameTimeline timeline; std::vector<std::uint8_t> actual;
        auto sink = [&](auto bytes) { actual.push_back(bytes[0]); };
        timeline.Write(Packet(0,1),sink); timeline.Write(Packet(3,2),sink); timeline.Write(Packet(5,3),sink);
        Require(actual == std::vector<std::uint8_t>{1,1,1,2,2,3} && timeline.FramesWritten() == 6);
    }});
    tests.push_back({"timeline_backfills_dropped_initial_frame", [] {
        FrameTimeline timeline; std::vector<std::uint8_t> actual;
        timeline.Write(Packet(3,7),[&](auto bytes) { actual.push_back(bytes[0]); });
        Require(actual == std::vector<std::uint8_t>{7,7,7,7} && timeline.FramesWritten() == 4);
    }});
    tests.push_back({"static_shared_frame_keeps_duration", [] {
        FrameTimeline timeline; auto pixels = Packet(0,9).pixels; unsigned writes{};
        for (unsigned i = 0; i < 90; ++i) { timeline.Write({i,pixels},[&](auto bytes) { Require(bytes[0] == 9); ++writes; }); }
        Require(writes == 90 && timeline.FramesWritten() == 90);
    }});
    tests.push_back({"timeline_rejects_stale_duplicate_empty_and_overflow", [] {
        FrameTimeline timeline; auto sink = [](auto) {};
        timeline.Write(Packet(1,1),sink);
        Reject([&] { timeline.Write(Packet(1,2),sink); });
        Reject([&] { timeline.Write(Packet(0,2),sink); });
        Reject([&] { timeline.Write({2,{}},sink); });
        Reject([&] { timeline.Write(Packet(UINT64_MAX,2),sink); });
        Require(timeline.FramesWritten() == 2);
    }});
    tests.push_back({"timeline_failed_write_is_not_counted", [] {
        FrameTimeline timeline; unsigned writes{};
        Reject([&] { timeline.Write(Packet(3,1),[&](auto) {
            if (writes == 2) { throw std::runtime_error("broken pipe"); } ++writes;
        }); });
        Require(writes == 2 && timeline.FramesWritten() == 2);
    }});
    tests.push_back({"queue_and_timeline_preserve_all_slots_under_pressure", [] {
        BoundedFrameQueue queue(4); FrameTimeline timeline;
        for (unsigned i = 0; i < 30; ++i) { queue.Push(Packet(i,static_cast<std::uint8_t>(i))); }
        std::vector<std::uint8_t> actual;
        while (!queue.Empty()) { timeline.Write(queue.Pop(),[&](auto bytes) { actual.push_back(bytes[0]); }); }
        Require(actual.size() == 30 && queue.Overflows() == 26);
        Require(std::all_of(actual.begin(),actual.begin()+27,[](auto v) { return v == 26; }));
        Require(actual.back() == 29 && timeline.FramesWritten() == 30);
    }});
    int failures{}; std::string cases;
    for (const auto& test : tests) {
        cases += "  <testcase name=\"" + test.name + "\">";
        try { test.action(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) {
            ++failures; cases += "<failure message=\"Contract violated\"/>";
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
        cases += "</testcase>\n";
    }
    if (argc > 1) {
        const fs::path path = argv[1]; fs::create_directories(path.parent_path());
        std::ofstream report(path);
        report << "<testsuite name=\"recorder\" tests=\"" << tests.size() << "\" failures=\"" << failures
               << "\">\n" << cases << "</testsuite>\n";
        if (!report) { return 2; }
    }
    std::cout << "tests=" << tests.size() << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
