#include "recorder_core.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace yourots::detail {

void ValidateRecorderSettings(int fps, std::size_t capacity, const std::filesystem::path& output) {
    if (fps != 30) {
        throw std::invalid_argument("A Fase 3 habilita somente o preset validado de 30 FPS.");
    }
    if (capacity == 0 || capacity > 32) {
        throw std::invalid_argument("A fila da gravacao precisa ter entre 1 e 32 quadros.");
    }
    auto extension = output.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    });
    if (output.empty() || extension != L".mp4" || output.stem().empty()) {
        throw std::invalid_argument("Defina um arquivo de saida com extensao .mp4.");
    }
}

std::vector<std::uint8_t> CropRecordingFrame(const PreviewFrame& frame, CropRect crop) {
    crop = ResolveCrop(crop, frame.width, frame.height);
    const std::size_t source_stride = static_cast<std::size_t>(frame.width) * 4U;
    if (source_stride > std::numeric_limits<std::size_t>::max() / frame.height ||
        frame.bgra.size() < source_stride * frame.height) {
        throw std::invalid_argument("Quadro BGRA incompleto recebido da captura.");
    }
    const std::size_t target_stride = static_cast<std::size_t>(crop.width) * 4U;
    std::vector<std::uint8_t> result(target_stride * crop.height);
    for (std::uint32_t row = 0; row < crop.height; ++row) {
        const auto* source = frame.bgra.data() +
            static_cast<std::size_t>(crop.y + row) * source_stride + static_cast<std::size_t>(crop.x) * 4U;
        std::copy_n(source, target_stride, result.data() + static_cast<std::size_t>(row) * target_stride);
    }
    return result;
}

std::wstring EncodingArguments(const std::filesystem::path& mkv, std::uint32_t width,
                               std::uint32_t height, int fps, std::wstring_view encoder) {
    if (width == 0 || height == 0 || fps != 30 ||
        (encoder != L"h264_nvenc" && encoder != L"libx264")) {
        throw std::invalid_argument("Preset de codificacao invalido.");
    }
    std::wstringstream args;
    args << L"-hide_banner -loglevel error -y "
         << L"-f rawvideo -pixel_format bgra -video_size " << width << L'x' << height
         << L" -framerate " << fps << L" -i pipe:0 "
         << L"-vf scale=1080:1920:flags=lanczos:out_color_matrix=bt709:out_range=tv,"
            L"setsar=1,format=yuv420p,"
            L"setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709 "
         << L"-an -c:v " << encoder;
    args << (encoder == L"h264_nvenc" ? L" -preset p5 -cq 18" : L" -preset veryfast -crf 18");
    args << L" -f matroska " << QuoteCommandArgument(mkv.wstring());
    return args.str();
}

std::wstring RemuxArguments(const std::filesystem::path& mkv, const std::filesystem::path& mp4) {
    // MKV rounds timestamps to milliseconds. Snap PTS/DTS back to the 30 FPS grid
    // in a timebase with exactly 512 ticks/frame, preserving B-frame ordering.
    return L"-hide_banner -loglevel error -y -i " + QuoteCommandArgument(mkv.wstring()) +
        L" -map 0:v:0 -c:v copy -an "
        L"-bsf:v setts=prescale=1:pts=round(PTS/512)*512:dts=round(DTS/512)*512:duration=512:time_base=1/15360 "
        L"-video_track_timescale 15360 -movflags +faststart " + QuoteCommandArgument(mp4.wstring());
}

double ValidateVideoProbe(std::string_view output, std::uint64_t frames, int fps) {
    if (frames == 0 || fps != 30) {
        throw std::invalid_argument("Linha do tempo de gravacao invalida.");
    }
    std::map<std::string, std::string> values;
    std::istringstream stream{std::string(output)};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        const auto separator = line.find('=');
        if (separator == std::string::npos ||
            !values.emplace(line.substr(0, separator), line.substr(separator + 1)).second) {
            throw std::invalid_argument("MP4 invalido: resposta ambigua do FFprobe.");
        }
    }
    const std::map<std::string, std::string> expected{
        {"codec_name", "h264"}, {"width", "1080"}, {"height", "1920"},
        {"pix_fmt", "yuv420p"}, {"r_frame_rate", "30/1"}, {"avg_frame_rate", "30/1"},
        {"sample_aspect_ratio", "1:1"}, {"color_range", "tv"},
        {"color_space", "bt709"}, {"color_transfer", "bt709"}, {"color_primaries", "bt709"},
        {"nb_read_frames", std::to_string(frames)},
    };
    for (const auto& [key, value] : expected) {
        if (!values.contains(key) || values.at(key) != value) {
            throw std::invalid_argument("MP4 invalido: " + key + " deveria ser " + value + ".");
        }
    }
    if (!values.contains("duration")) {
        throw std::invalid_argument("MP4 invalido: duracao ausente no FFprobe.");
    }
    const auto& raw = values.at("duration");
    double duration{};
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), duration);
    const double expected_duration = static_cast<double>(frames) / fps;
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() ||
        !std::isfinite(duration) || duration <= 0 || std::abs(duration - expected_duration) > 0.12) {
        throw std::invalid_argument("MP4 invalido: duracao diverge da linha do tempo da gravacao.");
    }
    return duration;
}

void ValidateAudioProbe(std::string_view output) {
    if (output.find_first_not_of("\r\n\t ") != std::string_view::npos) {
        throw std::invalid_argument("MP4 invalido: foi encontrada uma faixa de audio.");
    }
}

BoundedFrameQueue::BoundedFrameQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0 || capacity > 32) { throw std::invalid_argument("Capacidade de fila invalida."); }
}

void BoundedFrameQueue::Push(FramePacket packet) {
    if (!packet.pixels || packet.pixels->empty()) { throw std::invalid_argument("Quadro vazio."); }
    if (packets_.size() == capacity_) {
        packets_.pop_front();
        ++overflows_;
    }
    packets_.push_back(std::move(packet));
}

FramePacket BoundedFrameQueue::Pop() {
    if (packets_.empty()) { throw std::logic_error("Fila vazia."); }
    auto packet = std::move(packets_.front());
    packets_.pop_front();
    return packet;
}

void BoundedFrameQueue::Clear() noexcept {
    packets_.clear();
    overflows_ = 0;
}

void FrameTimeline::Write(const FramePacket& packet,
                          const std::function<void(std::span<const std::uint8_t>)>& sink) {
    if (!packet.pixels || packet.pixels->empty() || packet.timeline_index < next_index_ ||
        packet.timeline_index == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("Pacote de gravacao invalido.");
    }
    const auto filler = previous_ ? previous_ : packet.pixels;
    while (next_index_ < packet.timeline_index) {
        sink(*filler);
        ++next_index_;
    }
    sink(*packet.pixels);
    ++next_index_;
    previous_ = packet.pixels;
}

} // namespace yourots::detail
