#include "recorder.h"
#include "recorder_core.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace yourots {
namespace {

using Clock = std::chrono::steady_clock;

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return "erro Win32 sem mensagem";
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring BuildCommand(const fs::path& executable, std::wstring_view arguments) {
    std::wstring command = QuoteCommandArgument(executable.wstring());
    if (!arguments.empty()) {
        command.push_back(L' ');
        command.append(arguments);
    }
    return command;
}

struct ProcessOutput {
    DWORD exit_code{};
    std::string output;
};

ProcessOutput RunProcessCapture(const fs::path& executable, std::wstring_view arguments, DWORD timeout_ms) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_handle{};
    HANDLE write_handle{};
    if (!CreatePipe(&read_handle, &write_handle, &security, 0)) {
        throw std::runtime_error("CreatePipe falhou ao iniciar processo externo.");
    }
    struct HandleGuard {
        HANDLE* handle;
        ~HandleGuard() {
            if (*handle != nullptr) {
                CloseHandle(*handle);
                *handle = nullptr;
            }
        }
    } read_guard{&read_handle}, write_guard{&write_handle};

    if (!SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("SetHandleInformation falhou ao iniciar processo externo.");
    }

    HANDLE stdin_handle = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stdin_handle == INVALID_HANDLE_VALUE) {
        stdin_handle = nullptr;
        throw std::runtime_error("Nao foi possivel abrir NUL para a entrada do processo externo.");
    }
    HandleGuard stdin_guard{&stdin_handle};

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_handle;
    startup.hStdOutput = write_handle;
    startup.hStdError = write_handle;

    PROCESS_INFORMATION process{};
    std::wstring command = BuildCommand(executable, arguments);
    if (!CreateProcessW(
            nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("Nao foi possivel iniciar processo externo. Win32=" + std::to_string(GetLastError()));
    }
    CloseHandle(process.hThread);
    CloseHandle(write_handle);
    write_handle = nullptr;

    HandleGuard process_guard{&process.hProcess};
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string output;
    char buffer[4096];
    // Drain while the child runs: waiting first deadlocks once its pipe fills.
    for (;;) {
        const bool process_done = WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0;
        DWORD available{};
        if (PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD bytes_read{};
            if (ReadFile(read_handle, buffer, std::min<DWORD>(available, sizeof(buffer)), &bytes_read, nullptr)) {
                constexpr std::size_t limit = 1U << 20U;
                output.append(buffer, std::min<std::size_t>(bytes_read, limit - output.size()));
            }
        } else if (process_done) {
            break;
        } else {
            WaitForSingleObject(process.hProcess, 10);
        }
        if (Clock::now() >= deadline) {
            TerminateProcess(process.hProcess, 2);
            WaitForSingleObject(process.hProcess, 5000);
            throw std::runtime_error("Processo externo excedeu o tempo limite.");
        }
    }

    DWORD exit_code{};
    GetExitCodeProcess(process.hProcess, &exit_code);
    return {exit_code, std::move(output)};
}

bool ProbeEncoder(const fs::path& ffmpeg, std::wstring_view encoder) {
    std::wstringstream args;
    args << L"-hide_banner -loglevel error -f lavfi -i color=c=black:s=64x64:r=30:d=0.1 "
         << L"-frames:v 1 -an -c:v " << encoder << L" -f null -";
    return RunProcessCapture(ffmpeg, args.str(), 15000).exit_code == 0;
}

std::wstring SelectEncoder(const fs::path& ffmpeg) {
    if (ProbeEncoder(ffmpeg, L"h264_nvenc")) {
        return L"h264_nvenc";
    }
    if (ProbeEncoder(ffmpeg, L"libx264")) {
        return L"libx264";
    }
    throw std::runtime_error("Nenhum encoder H.264 disponivel: NVENC e libx264 falharam na inicializacao.");
}

class FfmpegPipe {
public:
    FfmpegPipe(const fs::path& ffmpeg, const fs::path& output_mkv,
               std::uint32_t width, std::uint32_t height, int fps, std::wstring encoder)
        : encoder_(std::move(encoder)) {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        if (!CreatePipe(&stdin_read_, &stdin_write_, &security, 1U << 20U)) {
            throw std::runtime_error("CreatePipe falhou para o FFmpeg.");
        }
        if (!SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandles();
            throw std::runtime_error("SetHandleInformation falhou para o FFmpeg.");
        }

        HANDLE null_output = CreateFileW(
            L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (null_output == INVALID_HANDLE_VALUE) {
            CloseHandles();
            throw std::runtime_error("Nao foi possivel abrir NUL para a saida do FFmpeg.");
        }

        const auto arguments = detail::EncodingArguments(output_mkv, width, height, fps, encoder_);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read_;
        startup.hStdOutput = null_output;
        startup.hStdError = null_output;

        PROCESS_INFORMATION process{};
        std::wstring command = BuildCommand(ffmpeg, arguments);
        if (!CreateProcessW(
                nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &process)) {
            const DWORD error = GetLastError();
            CloseHandle(null_output);
            CloseHandles();
            throw std::runtime_error("Nao foi possivel iniciar FFmpeg. Win32=" + std::to_string(error));
        }
        CloseHandle(null_output);
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        CloseHandle(stdin_read_);
        stdin_read_ = nullptr;
    }

    ~FfmpegPipe() {
        CloseHandles();
    }

    void Write(std::span<const std::uint8_t> bgra) {
        DWORD exit_code{};
        if (process_ == nullptr ||
            (GetExitCodeProcess(process_, &exit_code) && exit_code != STILL_ACTIVE)) {
            throw std::runtime_error(
                "FFmpeg encerrou inesperadamente" +
                (process_ != nullptr ? " com codigo " + std::to_string(exit_code) : std::string{}) + ".");
        }
        const auto* data = bgra.data();
        std::size_t remaining = bgra.size();
        while (remaining > 0) {
            DWORD written{};
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1U << 20U));
            if (!WriteFile(stdin_write_, data, chunk, &written, nullptr) || written == 0) {
                DWORD code{};
                if (process_ != nullptr && GetExitCodeProcess(process_, &code) && code != STILL_ACTIVE) {
                    throw std::runtime_error("FFmpeg encerrou inesperadamente com codigo " + std::to_string(code) + ".");
                }
                throw std::runtime_error("Falha escrevendo quadro no pipe do FFmpeg.");
            }
            data += written;
            remaining -= written;
        }
    }

    void Finish() {
        if (stdin_write_ != nullptr) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
        if (process_ == nullptr) {
            return;
        }
        const DWORD wait = WaitForSingleObject(process_, 60000);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(process_, 2);
            WaitForSingleObject(process_, 5000);
            throw std::runtime_error("FFmpeg nao finalizou dentro de 60 segundos.");
        }
        DWORD exit_code{};
        GetExitCodeProcess(process_, &exit_code);
        CloseHandle(process_);
        process_ = nullptr;
        if (exit_code != 0) {
            throw std::runtime_error("FFmpeg terminou com codigo " + std::to_string(exit_code));
        }
    }

private:
    void CloseHandles() noexcept {
        if (stdin_read_ != nullptr) {
            CloseHandle(stdin_read_);
            stdin_read_ = nullptr;
        }
        if (stdin_write_ != nullptr) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
        if (process_ != nullptr) {
            if (WaitForSingleObject(process_, 5000) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 2);
                WaitForSingleObject(process_, 5000);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    std::wstring encoder_;
    HANDLE stdin_read_{};
    HANDLE stdin_write_{};
    HANDLE process_{};
};

void RemuxToMp4(const fs::path& ffmpeg, const fs::path& mkv, const fs::path& mp4) {
    const auto result = RunProcessCapture(ffmpeg, detail::RemuxArguments(mkv, mp4), 60000);
    if (result.exit_code != 0) {
        throw std::runtime_error("Falha ao gerar MP4: " + result.output);
    }
}

double VerifyOutput(const fs::path& ffprobe, const fs::path& output,
                    std::uint64_t frames, int fps) {
    std::wstringstream args;
    args << L"-v error -count_frames -select_streams v:0 "
         << L"-show_entries stream=codec_name,width,height,pix_fmt,r_frame_rate,avg_frame_rate,sample_aspect_ratio,"
            L"color_range,color_space,color_transfer,color_primaries,nb_read_frames -show_entries format=duration "
         << L"-of default=noprint_wrappers=1 " << QuoteCommandArgument(output.wstring());
    const auto result = RunProcessCapture(
        ffprobe, args.str(), detail::MediaVerificationTimeoutMilliseconds(frames, fps));
    if (result.exit_code != 0) {
        throw std::runtime_error("FFprobe falhou ao validar o MP4: " + result.output);
    }
    const double duration = detail::ValidateVideoProbe(result.output, frames, fps);

    std::wstringstream audio_args;
    audio_args << L"-v error -select_streams a -show_entries stream=index -of csv=p=0 "
               << QuoteCommandArgument(output.wstring());
    const auto audio = RunProcessCapture(ffprobe, audio_args.str(), 30000);
    if (audio.exit_code != 0) {
        throw std::runtime_error("FFprobe falhou ao verificar audio: " + audio.output);
    }
    detail::ValidateAudioProbe(audio.output);
    return duration;
}

std::uint64_t ProbeFrameCount(const fs::path& ffprobe, const fs::path& input) {
    std::wstringstream args;
    args << L"-v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames "
            L"-of default=noprint_wrappers=1:nokey=1 "
         << QuoteCommandArgument(input.wstring());
    const auto result = RunProcessCapture(
        ffprobe, args.str(), detail::MediaVerificationTimeoutMilliseconds(0, 30));
    if (result.exit_code != 0) {
        throw std::runtime_error("FFprobe nao conseguiu ler a gravacao temporaria: " + result.output);
    }
    return detail::ParseRecoveryFrameCount(result.output);
}

void EnsureOutputWritable(const fs::path& output, const fs::path& temporary) {
    const fs::path directory = output.parent_path().empty() ? fs::current_path() : output.parent_path();
    fs::create_directories(directory);
    if (!fs::is_directory(directory)) {
        throw std::runtime_error("A pasta de destino nao e um diretorio valido.");
    }
    if (fs::exists(output)) {
        throw std::runtime_error("O arquivo de saida ja existe; escolha um nome unico.");
    }
    if (fs::exists(temporary)) {
        throw std::runtime_error("Ja existe uma gravacao temporaria com esse nome; ela foi preservada.");
    }

    std::error_code space_error;
    const auto space = fs::space(directory, space_error);
    if (!space_error) detail::ValidateAvailableRecordingSpace(space.available, true);

    const fs::path probe = directory /
        (L".yourots-write-probe-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE handle = CreateFileW(
        probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "Sem permissao para gravar na pasta de destino. Win32=" + std::to_string(GetLastError()));
    }
    CloseHandle(handle);
}

} // namespace

struct Recorder::Impl {
    Impl(std::shared_ptr<WindowCapture> source, RecorderOptions settings)
        : capture(std::move(source)), options(std::move(settings)),
          queue(std::clamp<std::size_t>(options.queue_capacity, 1, 32)) {}

    void SetError(std::string value) noexcept {
        {
            std::scoped_lock lock(error_mutex);
            if (error.empty()) {
                error = std::move(value);
            }
        }
        stop_requested.store(true);
        wait_cv.notify_all();
        queue_cv.notify_all();
    }

    std::string Error() const {
        std::scoped_lock lock(error_mutex);
        return error;
    }

    void PushPacket(detail::FramePacket packet) {
        std::scoped_lock lock(queue_mutex);
        queue.Push(std::move(packet));
        queue_cv.notify_one();
    }

    void SamplerLoop() noexcept {
        try {
            auto latest = capture->LatestFrame();
            if (!latest || latest->bgra.empty()) {
                throw std::runtime_error("A captura nao possui um quadro inicial para gravar.");
            }
            ResolveCrop(options.crop, base_width, base_height);

            std::uint64_t last_sequence = 0;
            std::shared_ptr<const std::vector<std::uint8_t>> current_pixels;
            auto update_pixels = [&](const std::shared_ptr<const PreviewFrame>& frame) {
                if (!frame || frame->bgra.empty()) {
                    return true;
                }
                if (frame->width != base_width || frame->height != base_height) {
                    return false;
                }
                if (frame->sequence != last_sequence) {
                    if (last_sequence != 0 && frame->sequence > last_sequence + 1) {
                        source_frames_skipped += frame->sequence - last_sequence - 1;
                    }
                    current_pixels = std::make_shared<const std::vector<std::uint8_t>>(detail::CropRecordingFrame(*frame, options.crop));
                    last_sequence = frame->sequence;
                }
                return true;
            };

            if (!update_pixels(latest)) {
                throw std::runtime_error("O quadro inicial mudou de tamanho durante a preparacao da gravacao.");
            }
            if (!current_pixels) {
                throw std::runtime_error("Nao foi possivel preparar o primeiro quadro recortado.");
            }

            const auto interval = std::chrono::duration<double>(1.0 / static_cast<double>(options.fps));
            std::uint64_t index = 0;
            auto next_tick = Clock::now();
            for (;;) {
                if (stop_requested.load()) {
                    break;
                }

                if (!IsWindow(capture->SourceWindow())) {
                    throw std::runtime_error("A janela fonte foi fechada.");
                }
                if (IsIconic(capture->SourceWindow())) {
                    paused.store(true);
                }
                if (paused.load()) {
                    std::unique_lock lock(wait_mutex);
                    wait_cv.wait(lock, [&] { return stop_requested.load() || !paused.load(); });
                    next_tick = Clock::now();
                    continue;
                }
                const auto capture_error = capture->LastError();
                if (!capture_error.empty()) {
                    throw std::runtime_error(capture_error);
                }
                if (!update_pixels(capture->LatestFrame())) {
                    paused.store(true);
                    continue;
                }
                PushPacket(detail::FramePacket{index, current_pixels});
                ++index;
                timeline_frames.store(index);

                next_tick += std::chrono::duration_cast<Clock::duration>(interval);
                std::unique_lock lock(wait_mutex);
                wait_cv.wait_until(lock, next_tick, [&] { return stop_requested.load() || paused.load(); });
            }
        } catch (const std::exception& ex) {
            SetError(ex.what());
        } catch (...) {
            SetError("Falha desconhecida no controle de cadencia da gravacao.");
        }
        {
            std::scoped_lock lock(queue_mutex);
            sampling_done = true;
        }
        queue_cv.notify_all();
    }

    void WriterLoop() noexcept {
        try {
            detail::FrameTimeline timeline;
            std::uint64_t last_space_check{};
            for (;;) {
                detail::FramePacket packet;
                {
                    std::unique_lock lock(queue_mutex);
                    queue_cv.wait(lock, [&] { return !queue.Empty() || sampling_done; });
                    if (queue.Empty() && sampling_done) {
                        break;
                    }
                    packet = queue.Pop();
                }

                timeline.Write(packet, [&](auto pixels) { ffmpeg->Write(pixels); });
                frames_written = timeline.FramesWritten();
                if (frames_written >= last_space_check + static_cast<std::uint64_t>(options.fps)) {
                    last_space_check = frames_written;
                    const auto directory = options.output_path.parent_path().empty()
                        ? fs::current_path() : options.output_path.parent_path();
                    std::error_code space_error;
                    const auto space = fs::space(directory, space_error);
                    if (!space_error) detail::ValidateAvailableRecordingSpace(space.available, false);
                }
            }
            ffmpeg->Finish();
        } catch (const std::exception& ex) {
            SetError(ex.what());
        } catch (...) {
            SetError("Falha desconhecida ao enviar quadros ao FFmpeg.");
        }
    }

    std::shared_ptr<WindowCapture> capture;
    RecorderOptions options;
    fs::path temporary_mkv;
    std::wstring encoder;
    std::unique_ptr<FfmpegPipe> ffmpeg;
    std::thread sampler_thread;
    std::thread writer_thread;
    std::atomic_bool recording{false};
    std::atomic_bool stop_requested{false};
    std::atomic_bool paused{false};
    std::atomic<std::uint64_t> timeline_frames{0};
    std::uint64_t frames_written{};
    std::uint64_t source_frames_skipped{};
    std::uint32_t base_width{};
    std::uint32_t base_height{};
    mutable std::mutex error_mutex;
    std::string error;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    detail::BoundedFrameQueue queue;
    bool sampling_done{};
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
};

Recorder::Recorder(std::shared_ptr<WindowCapture> capture, RecorderOptions options)
    : impl_(std::make_unique<Impl>(std::move(capture), std::move(options))) {}

Recorder::~Recorder() {
    if (!impl_) {
        return;
    }
    impl_->stop_requested.store(true);
    impl_->wait_cv.notify_all();
    impl_->queue_cv.notify_all();
    if (impl_->sampler_thread.joinable()) {
        impl_->sampler_thread.join();
    }
    if (impl_->writer_thread.joinable()) {
        impl_->writer_thread.join();
    }
}

void Recorder::Start() {
    if (!impl_->capture) {
        throw std::runtime_error("CaptureSession ausente para iniciar a gravacao.");
    }
    if (impl_->recording.exchange(true)) {
        throw std::runtime_error("A gravacao ja esta em andamento.");
    }
    try {
        detail::ValidateRecorderSettings(impl_->options.fps, impl_->options.queue_capacity, impl_->options.output_path);
        if (!fs::is_regular_file(impl_->options.ffmpeg_path)) {
            throw std::runtime_error("FFmpeg nao encontrado: " + WideToUtf8(impl_->options.ffmpeg_path.wstring()));
        }
        if (!fs::is_regular_file(impl_->options.ffprobe_path)) {
            throw std::runtime_error("FFprobe nao encontrado: " + WideToUtf8(impl_->options.ffprobe_path.wstring()));
        }
        const auto initial = impl_->capture->LatestFrame();
        if (!initial || initial->bgra.empty()) {
            throw std::runtime_error("A previa ainda nao recebeu um quadro valido.");
        }
        const auto crop = ResolveCrop(impl_->options.crop, initial->width, initial->height);
        if (static_cast<std::uint64_t>(crop.width) * 16ULL != static_cast<std::uint64_t>(crop.height) * 9ULL) {
            throw std::runtime_error("A regiao de gravacao precisa manter proporcao 9:16.");
        }
        if (!impl_->options.output_path.parent_path().empty()) {
            fs::create_directories(impl_->options.output_path.parent_path());
        }

        impl_->temporary_mkv = impl_->options.output_path;
        impl_->temporary_mkv.replace_extension(L".recording.mkv");
        EnsureOutputWritable(impl_->options.output_path, impl_->temporary_mkv);
        impl_->stop_requested.store(false);
        impl_->paused.store(false);
        impl_->sampling_done = false;
        impl_->timeline_frames.store(0);
        impl_->frames_written = 0;
        impl_->source_frames_skipped = 0;
        impl_->base_width = initial->width;
        impl_->base_height = initial->height;
        impl_->queue.Clear();
        {
            std::scoped_lock lock(impl_->error_mutex);
            impl_->error.clear();
        }
        impl_->encoder = SelectEncoder(impl_->options.ffmpeg_path);
        impl_->ffmpeg = std::make_unique<FfmpegPipe>(
            impl_->options.ffmpeg_path, impl_->temporary_mkv,
            crop.width, crop.height, impl_->options.fps, impl_->encoder);
        impl_->writer_thread = std::thread([state = impl_.get()] { state->WriterLoop(); });
        impl_->sampler_thread = std::thread([state = impl_.get()] { state->SamplerLoop(); });
    } catch (...) {
        impl_->stop_requested.store(true);
        impl_->wait_cv.notify_all();
        if (impl_->sampler_thread.joinable()) { impl_->sampler_thread.join(); }
        {
            std::scoped_lock lock(impl_->queue_mutex);
            impl_->sampling_done = true;
        }
        impl_->queue_cv.notify_all();
        if (impl_->writer_thread.joinable()) { impl_->writer_thread.join(); }
        impl_->ffmpeg.reset();
        impl_->recording.store(false);
        throw;
    }
}

void Recorder::Pause() {
    if (!impl_ || !impl_->recording.load()) {
        throw std::runtime_error("Nao ha gravacao em andamento para pausar.");
    }
    impl_->paused.store(true);
    impl_->wait_cv.notify_all();
}

void Recorder::Resume() {
    if (!impl_ || !impl_->recording.load()) {
        throw std::runtime_error("Nao ha gravacao em andamento para retomar.");
    }
    if (const auto error = impl_->Error(); !error.empty()) {
        throw std::runtime_error(error);
    }
    if (!IsWindow(impl_->capture->SourceWindow()) || IsIconic(impl_->capture->SourceWindow())) {
        throw std::runtime_error("A janela fonte precisa estar aberta e restaurada para retomar.");
    }
    if (const auto error = impl_->capture->LastError(); !error.empty()) {
        throw std::runtime_error(error);
    }
    const auto latest = impl_->capture->LatestFrame();
    if (!latest || latest->bgra.empty()) {
        throw std::runtime_error("A captura nao possui um quadro valido para retomar.");
    }
    if (impl_->base_width != 0 &&
        (latest->width != impl_->base_width || latest->height != impl_->base_height)) {
        throw std::runtime_error("O tamanho da fonte mudou; finalize esta gravacao e recalibre antes de iniciar outra.");
    }
    ResolveCrop(impl_->options.crop, latest->width, latest->height);
    impl_->paused.store(false);
    impl_->wait_cv.notify_all();
}

RecorderResult Recorder::Stop() {
    if (!impl_->recording.exchange(false)) {
        throw std::runtime_error("Nao ha gravacao em andamento.");
    }
    // The sampler may be waiting in Pause, or UI polling may stop before its next tick.
    if (!IsWindow(impl_->capture->SourceWindow())) {
        impl_->SetError("A janela fonte foi fechada.");
    } else if (const auto error = impl_->capture->LastError(); !error.empty()) {
        impl_->SetError(error);
    }
    impl_->stop_requested.store(true);
    impl_->wait_cv.notify_all();
    if (impl_->sampler_thread.joinable()) {
        impl_->sampler_thread.join();
    }
    impl_->queue_cv.notify_all();
    if (impl_->writer_thread.joinable()) {
        impl_->writer_thread.join();
    }
    impl_->ffmpeg.reset();

    if (const auto error = impl_->Error(); !error.empty()) {
        throw std::runtime_error(error);
    }
    if (impl_->frames_written == 0) {
        throw std::runtime_error("A gravacao terminou sem quadros codificados.");
    }

    RemuxToMp4(impl_->options.ffmpeg_path, impl_->temporary_mkv, impl_->options.output_path);
    const double duration = VerifyOutput(
        impl_->options.ffprobe_path, impl_->options.output_path,
        impl_->frames_written, impl_->options.fps);
    std::error_code ignored;
    fs::remove(impl_->temporary_mkv, ignored);

    return RecorderResult{
        impl_->options.output_path,
        impl_->temporary_mkv,
        impl_->encoder,
        impl_->frames_written,
        impl_->source_frames_skipped,
        impl_->queue.Overflows(),
        duration,
    };
}

bool Recorder::IsRecording() const noexcept {
    return impl_ && impl_->recording.load();
}

bool Recorder::IsPaused() const noexcept {
    return impl_ && impl_->recording.load() && impl_->paused.load();
}

double Recorder::RecordedDurationSeconds() const noexcept {
    if (!impl_ || impl_->options.fps <= 0) {
        return 0.0;
    }
    return static_cast<double>(impl_->timeline_frames.load()) / static_cast<double>(impl_->options.fps);
}

fs::path Recorder::TemporaryPath() const {
    return impl_ ? impl_->temporary_mkv : fs::path{};
}

std::string Recorder::LastError() const {
    return impl_ ? impl_->Error() : std::string{};
}

RecorderResult RecoverTemporaryRecording(
    const fs::path& ffmpeg_path,
    const fs::path& ffprobe_path,
    const fs::path& temporary_mkv_path,
    const fs::path& output_path,
    int fps) {
    detail::ValidateRecorderSettings(fps, 1, output_path);
    if (!fs::is_regular_file(ffmpeg_path) || !fs::is_regular_file(ffprobe_path)) {
        throw std::runtime_error("FFmpeg/FFprobe nao estao disponiveis para recuperar a gravacao.");
    }
    if (!fs::is_regular_file(temporary_mkv_path) || fs::file_size(temporary_mkv_path) == 0) {
        throw std::runtime_error("A gravacao temporaria nao existe ou esta vazia.");
    }
    auto recovery_probe = output_path;
    recovery_probe.replace_extension(L".recovery-probe.tmp");
    EnsureOutputWritable(output_path, recovery_probe);
    const auto frames = ProbeFrameCount(ffprobe_path, temporary_mkv_path);
    RemuxToMp4(ffmpeg_path, temporary_mkv_path, output_path);
    const double duration = VerifyOutput(ffprobe_path, output_path, frames, fps);
    std::error_code ignored;
    fs::remove(temporary_mkv_path, ignored);
    return RecorderResult{output_path, temporary_mkv_path, L"recuperado", frames, 0, 0, duration};
}

} // namespace yourots
