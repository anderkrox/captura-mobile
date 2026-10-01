#include <windows.h>
#include <windowsx.h>

#include "capture_core.h"
#include "calibration.h"
#include "capture_preview.h"

#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"YourotsCaptureWindow";
constexpr wchar_t kWindowTitle[] = L"Yourots Capture - Calibracao";
constexpr UINT_PTR kPreviewTimer = 1;
constexpr UINT kPreviewIntervalMs = 33;
constexpr UINT kStartCaptureMessage = WM_APP + 1;

enum ControlId : int {
    kSourceCombo = 100,
    kRefreshButton,
    kCaptureButton,
    kStatusText,
    kCropText,
    kXEdit,
    kYEdit,
    kWidthEdit,
    kHeightEdit,
    kApplyButton,
    kLeftButton,
    kRightButton,
    kUpButton,
    kDownButton,
    kSmallerButton,
    kLargerButton,
    kReferenceButton,
    kSaveButton,
};

using yourots::Calibration;
using yourots::FitImageRect;
using yourots::RatioRectFromDrag;
using yourots::LoadCalibration;

struct AppState {
    HINSTANCE instance{};
    HWND hwnd{};
    HWND source_combo{};
    HWND refresh_button{};
    HWND capture_button{};
    HWND status_text{};
    HWND crop_text{};
    HWND x_edit{};
    HWND y_edit{};
    HWND width_edit{};
    HWND height_edit{};
    HWND apply_button{};
    HWND left_button{};
    HWND right_button{};
    HWND up_button{};
    HWND down_button{};
    HWND smaller_button{};
    HWND larger_button{};
    HWND reference_button{};
    HWND save_button{};

    std::vector<yourots::WindowInfo> windows;
    std::shared_ptr<yourots::WindowCapture> capture;
    std::shared_ptr<const yourots::PreviewFrame> frame;
    yourots::WindowInfo source_info{};
    Calibration saved{};
    fs::path settings_path;

    RECT preview_rect{};
    RECT image_rect{};
    yourots::CropRect crop{};
    bool crop_set{};
    bool calibration_valid{};
    bool basis_invalidated{};
    bool saved_source_unique{};
    std::uint32_t crop_frame_width{};
    std::uint32_t crop_frame_height{};
    UINT crop_dpi{};
    std::uint64_t displayed_sequence{};
    bool dragging{};
    POINT drag_anchor{};
    std::wstring last_status;
};

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return L"Erro de conversao de texto.";
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
    return result;
}

fs::path SettingsPath() {
    std::wstring local_app_data(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(), static_cast<DWORD>(local_app_data.size()));
    fs::path base;
    if (length > 0 && length < local_app_data.size()) {
        local_app_data.resize(length);
        base = local_app_data;
    } else {
        base = fs::current_path();
    }
    return base / L"YourotsCapture" / L"settings.ini";
}

void SaveCalibration(const AppState& state) {
    if (!state.calibration_valid || !state.frame || state.source_info.hwnd == nullptr) {
        throw std::runtime_error("A calibracao atual nao esta valida para ser salva.");
    }
    yourots::SaveCalibration(state.settings_path, Calibration{
        true, state.source_info.process, state.source_info.title, state.source_info.class_name,
        state.crop_frame_width, state.crop_frame_height, state.crop_dpi, state.crop});
}

void SetStatus(AppState& state, std::wstring text) {
    if (state.last_status == text) {
        return;
    }
    state.last_status = std::move(text);
    SetWindowTextW(state.status_text, state.last_status.c_str());
}

void SetEditUInt(HWND edit, std::uint32_t value) {
    SetWindowTextW(edit, std::to_wstring(value).c_str());
}

std::optional<std::uint32_t> ReadEditUInt(HWND edit) {
    wchar_t buffer[64]{};
    if (GetWindowTextLengthW(edit) >= static_cast<int>(std::size(buffer))) {
        return std::nullopt;
    }
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    if (buffer[0] == L'\0') {
        return std::nullopt;
    }
    return yourots::ParseCalibrationUInt(buffer);
}

void SyncCropControls(AppState& state) {
    if (!state.crop_set) {
        SetWindowTextW(state.crop_text, L"Regiao: ainda nao selecionada");
        return;
    }
    SetEditUInt(state.x_edit, state.crop.x);
    SetEditUInt(state.y_edit, state.crop.y);
    SetEditUInt(state.width_edit, state.crop.width);
    SetEditUInt(state.height_edit, state.crop.height);
    std::wstringstream text;
    text << L"Regiao fisica: X=" << state.crop.x << L"  Y=" << state.crop.y
         << L"  " << state.crop.width << L" x " << state.crop.height;
    SetWindowTextW(state.crop_text, text.str().c_str());
}

bool SameSavedSource(const AppState& state, const yourots::WindowInfo& source) {
    return yourots::SameCalibrationSource(state.saved, source.process, source.title, source.class_name);
}

UINT SourceDpi(const AppState& state) {
    if (state.source_info.hwnd == nullptr || !IsWindow(state.source_info.hwnd)) {
        return 0;
    }
    return GetDpiForWindow(state.source_info.hwnd);
}

void ValidateCalibrationState(AppState& state) {
    state.calibration_valid = false;
    if (!state.frame) { return; }
    if (!state.crop_set) {
        SetStatus(state, L"Arraste sobre a previa para selecionar a area Mobile em 9:16.");
        return;
    }
    if (!IsWindow(state.source_info.hwnd) || IsIconic(state.source_info.hwnd)) {
        SetStatus(state, L"A fonte esta indisponivel ou minimizada. Restaure a janela para validar.");
        return;
    }
    const UINT dpi = SourceDpi(state);
    const Calibration basis{true, {}, {}, {}, state.crop_frame_width, state.crop_frame_height, state.crop_dpi, state.crop};
    const auto check = yourots::CheckCalibration(basis, state.frame->width, state.frame->height, dpi);
    if (check == yourots::CalibrationCheck::FrameChanged) {
        state.basis_invalidated = true;
        std::wstringstream message;
        message << L"Recalibracao necessaria: o quadro mudou de "
                << state.crop_frame_width << L"x" << state.crop_frame_height << L" para "
                << state.frame->width << L"x" << state.frame->height << L".";
        SetStatus(state, message.str());
        return;
    }
    if (check == yourots::CalibrationCheck::DpiChanged) {
        state.basis_invalidated = true;
        std::wstringstream message;
        message << L"Recalibracao necessaria: o DPI da fonte mudou de " << state.crop_dpi << L" para " << dpi << L".";
        SetStatus(state, message.str());
        return;
    }
    if (check == yourots::CalibrationCheck::OutsideFrame || check == yourots::CalibrationCheck::EmptyFrame) {
        SetStatus(state, L"Recalibracao necessaria: a regiao ficou fora do quadro capturado.");
        return;
    }
    if (check == yourots::CalibrationCheck::InvalidRatio) {
        SetStatus(state, L"Recalibracao necessaria: a regiao precisa manter proporcao 9:16.");
        return;
    }
    if (state.basis_invalidated) {
        SetStatus(state, L"Recalibracao necessaria: confirme novamente a regiao apos a alteracao da fonte.");
        return;
    }
    state.calibration_valid = true;
    std::wstringstream message;
    message << L"Calibracao valida - " << state.frame->width << L"x" << state.frame->height
            << L" fisicos, DPI " << dpi << L". Mover a janela preserva o recorte.";
    SetStatus(state, message.str());
}

void ValidateCalibration(AppState& state) {
    ValidateCalibrationState(state);
    if ((IsWindowEnabled(state.save_button) != FALSE) != state.calibration_valid) {
        EnableWindow(state.save_button, state.calibration_valid);
    }
}

void AdoptCurrentBasis(AppState& state) {
    if (!state.frame) {
        return;
    }
    state.crop_frame_width = state.frame->width;
    state.crop_frame_height = state.frame->height;
    state.crop_dpi = SourceDpi(state);
    state.basis_invalidated = false;
    ValidateCalibration(state);
    SyncCropControls(state);
    InvalidateRect(state.hwnd, &state.preview_rect, FALSE);
}

std::optional<POINT> PreviewToFrame(const AppState& state, POINT point) {
    if (!state.frame) {
        return std::nullopt;
    }
    return yourots::PreviewToFrame(state.image_rect, state.frame->width, state.frame->height, point);
}

RECT CropToPreviewRect(const AppState& state) {
    if (!state.frame || !state.crop_set) {
        return {};
    }
    return yourots::CropToPreviewRect(state.image_rect, state.frame->width, state.frame->height, state.crop);
}

void RefreshWindows(AppState& state) {
    state.windows = yourots::EnumerateCaptureWindows(state.hwnd);
    SendMessageW(state.source_combo, CB_RESETCONTENT, 0, 0);
    int saved_exact = -1;
    int saved_matches = 0;
    int first_edge = -1;
    for (std::size_t index = 0; index < state.windows.size(); ++index) {
        const auto& window = state.windows[index];
        std::wstringstream label;
        label << window.process << L" - " << window.title;
        const auto label_text = label.str();
        SendMessageW(state.source_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label_text.c_str()));
        if (SameSavedSource(state, window)) {
            saved_exact = static_cast<int>(index);
            ++saved_matches;
        }
        if (first_edge < 0 && _wcsicmp(window.process.c_str(), L"msedge.exe") == 0) {
            first_edge = static_cast<int>(index);
        }
    }
    state.saved_source_unique = saved_matches == 1;
    const int selection = state.saved_source_unique ? saved_exact : first_edge;
    if (selection >= 0) {
        SendMessageW(state.source_combo, CB_SETCURSEL, selection, 0);
    }
    if (state.windows.empty()) {
        SetStatus(state, L"Nenhuma janela capturavel foi encontrada.");
    }
}

void StopCapture(AppState& state) {
    if (state.capture) {
        state.capture->Stop();
    }
    state.capture.reset();
    state.frame.reset();
    state.displayed_sequence = 0;
    state.source_info = {};
    state.crop_set = false;
    state.calibration_valid = false;
    state.basis_invalidated = false;
    EnableWindow(state.save_button, FALSE);
    SyncCropControls(state);
    InvalidateRect(state.hwnd, &state.preview_rect, FALSE);
}

void StartSelectedCapture(AppState& state) {
    const LRESULT selected = SendMessageW(state.source_combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR || static_cast<std::size_t>(selected) >= state.windows.size()) {
        SetStatus(state, L"Selecione uma janela antes de iniciar a previa.");
        return;
    }
    const auto source = state.windows[static_cast<std::size_t>(selected)];
    StopCapture(state);
    try {
        state.source_info = source;
        state.capture = std::make_shared<yourots::WindowCapture>(source.hwnd);
        state.capture->Start();
        if (SameSavedSource(state, source)) {
            state.crop = state.saved.crop;
            state.crop_set = true;
            state.crop_frame_width = state.saved.frame_width;
            state.crop_frame_height = state.saved.frame_height;
            state.crop_dpi = state.saved.dpi;
            SyncCropControls(state);
            SetStatus(state, L"Fonte reconhecida. Conferindo a calibracao salva...");
        } else {
            state.crop_set = false;
            SetStatus(state, L"Previa iniciada. Arraste sobre a imagem para calibrar a area Mobile.");
        }
    } catch (const winrt::hresult_error& error) {
        StopCapture(state);
        SetStatus(state, L"Falha ao iniciar captura: " + std::wstring(error.message()));
    } catch (const std::exception& error) {
        StopCapture(state);
        SetStatus(state, L"Falha ao iniciar captura: " + Utf8ToWide(error.what()));
    }
}

void PollCapture(AppState& state) {
    if (!state.capture) {
        return;
    }
    if (!IsWindow(state.source_info.hwnd)) {
        StopCapture(state);
        SetStatus(state, L"A janela fonte foi fechada. Selecione outra janela.");
        return;
    }
    const auto error = state.capture->LastError();
    if (!error.empty()) {
        StopCapture(state);
        SetStatus(state, L"Falha na captura: " + Utf8ToWide(error));
        return;
    }
    const auto latest = state.capture->LatestFrame();
    if (latest && latest->sequence != state.displayed_sequence) {
        state.frame = latest;
        state.displayed_sequence = latest->sequence;
        state.image_rect = FitImageRect(state.preview_rect, latest->width, latest->height);
        ValidateCalibration(state);
        InvalidateRect(state.hwnd, &state.preview_rect, FALSE);
    } else if (state.frame && state.crop_set) {
        ValidateCalibration(state);
    }
}

void ApplyCropFromEdits(AppState& state) {
    if (!state.frame) {
        SetStatus(state, L"Inicie uma previa antes de editar o recorte.");
        return;
    }
    const auto x = ReadEditUInt(state.x_edit);
    const auto y = ReadEditUInt(state.y_edit);
    const auto width = ReadEditUInt(state.width_edit);
    const auto height = ReadEditUInt(state.height_edit);
    if (!x || !y || !width || !height || *width == 0 || *height == 0) {
        SetStatus(state, L"Informe X, Y, largura e altura com numeros validos.");
        return;
    }
    if (static_cast<std::uint64_t>(*width) * 16ULL != static_cast<std::uint64_t>(*height) * 9ULL) {
        SetStatus(state, L"Largura e altura precisam formar exatamente a proporcao 9:16.");
        return;
    }
    yourots::CropRect candidate{*x, *y, *width, *height};
    try {
        yourots::ResolveCrop(candidate, state.frame->width, state.frame->height);
    } catch (...) {
        SetStatus(state, L"O recorte informado ultrapassa os limites do quadro atual.");
        return;
    }
    state.crop = candidate;
    state.crop_set = true;
    AdoptCurrentBasis(state);
}

void NudgeCrop(AppState& state, int dx, int dy) {
    if (!state.frame || !state.crop_set) {
        SetStatus(state, L"Selecione uma regiao antes do ajuste fino.");
        return;
    }
    if (state.crop.width > state.frame->width || state.crop.height > state.frame->height) {
        SetStatus(state, L"O recorte atual nao cabe no quadro. Recalibre a regiao.");
        return;
    }
    const auto max_x = state.frame->width - state.crop.width;
    const auto max_y = state.frame->height - state.crop.height;
    const auto next_x = std::clamp<long long>(static_cast<long long>(state.crop.x) + dx, 0, max_x);
    const auto next_y = std::clamp<long long>(static_cast<long long>(state.crop.y) + dy, 0, max_y);
    state.crop.x = static_cast<std::uint32_t>(next_x);
    state.crop.y = static_cast<std::uint32_t>(next_y);
    AdoptCurrentBasis(state);
}

void ResizeCrop(AppState& state, int units_delta) {
    if (!state.frame || !state.crop_set) {
        SetStatus(state, L"Selecione uma regiao antes de alterar o tamanho.");
        return;
    }
    const int current_units = static_cast<int>(state.crop.width / 9U);
    const int max_units = static_cast<int>(std::min(state.frame->width / 9U, state.frame->height / 16U));
    if (max_units <= 0) {
        return;
    }
    const int next_units = std::clamp(current_units + units_delta, 1, max_units);
    const std::uint32_t next_width = static_cast<std::uint32_t>(next_units * 9);
    const std::uint32_t next_height = static_cast<std::uint32_t>(next_units * 16);
    const long long center_x = static_cast<long long>(state.crop.x) + state.crop.width / 2;
    const long long center_y = static_cast<long long>(state.crop.y) + state.crop.height / 2;
    state.crop.width = next_width;
    state.crop.height = next_height;
    state.crop.x = static_cast<std::uint32_t>(std::clamp<long long>(
        center_x - next_width / 2, 0, state.frame->width - next_width));
    state.crop.y = static_cast<std::uint32_t>(std::clamp<long long>(
        center_y - next_height / 2, 0, state.frame->height - next_height));
    AdoptCurrentBasis(state);
}

void SetReferenceCrop(AppState& state) {
    if (!state.frame) {
        SetStatus(state, L"Inicie uma previa antes de usar o recorte 486 x 864.");
        return;
    }
    constexpr std::uint32_t width = 486;
    constexpr std::uint32_t height = 864;
    if (state.frame->width < width || state.frame->height < height) {
        SetStatus(state, L"O quadro atual e menor que o recorte de referencia 486 x 864.");
        return;
    }
    state.crop = {
        (state.frame->width - width) / 2,
        (state.frame->height - height) / 2,
        width,
        height,
    };
    state.crop_set = true;
    AdoptCurrentBasis(state);
}

HWND CreateControl(AppState& state, const wchar_t* class_name, const wchar_t* text, DWORD style,
                   int id) {
    return CreateWindowExW(
        0,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0, 0, 10, 10,
        state.hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        state.instance,
        nullptr);
}

void CreateControls(AppState& state) {
    state.source_combo = CreateControl(state, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, kSourceCombo);
    state.refresh_button = CreateControl(state, L"BUTTON", L"Atualizar", BS_PUSHBUTTON, kRefreshButton);
    state.capture_button = CreateControl(state, L"BUTTON", L"Abrir previa", BS_DEFPUSHBUTTON, kCaptureButton);
    state.status_text = CreateControl(state, L"STATIC", L"", SS_LEFT, kStatusText);
    state.crop_text = CreateControl(state, L"STATIC", L"Regiao: ainda nao selecionada", SS_LEFT, kCropText);
    state.x_edit = CreateControl(state, L"EDIT", L"0", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, kXEdit);
    state.y_edit = CreateControl(state, L"EDIT", L"0", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, kYEdit);
    state.width_edit = CreateControl(state, L"EDIT", L"486", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, kWidthEdit);
    state.height_edit = CreateControl(state, L"EDIT", L"864", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, kHeightEdit);
    state.apply_button = CreateControl(state, L"BUTTON", L"Aplicar X/Y/W/H", BS_PUSHBUTTON, kApplyButton);
    state.left_button = CreateControl(state, L"BUTTON", L"<- 1 px", BS_PUSHBUTTON, kLeftButton);
    state.right_button = CreateControl(state, L"BUTTON", L"1 px ->", BS_PUSHBUTTON, kRightButton);
    state.up_button = CreateControl(state, L"BUTTON", L"^ 1 px", BS_PUSHBUTTON, kUpButton);
    state.down_button = CreateControl(state, L"BUTTON", L"v 1 px", BS_PUSHBUTTON, kDownButton);
    state.smaller_button = CreateControl(state, L"BUTTON", L"- 9 x 16", BS_PUSHBUTTON, kSmallerButton);
    state.larger_button = CreateControl(state, L"BUTTON", L"+ 9 x 16", BS_PUSHBUTTON, kLargerButton);
    state.reference_button = CreateControl(state, L"BUTTON", L"Usar 486 x 864", BS_PUSHBUTTON, kReferenceButton);
    state.save_button = CreateControl(state, L"BUTTON", L"Salvar calibracao", BS_PUSHBUTTON, kSaveButton);
    EnableWindow(state.save_button, FALSE);
}

void LayoutControls(AppState& state) {
    RECT client{};
    GetClientRect(state.hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    constexpr int margin = 16;
    constexpr int side_width = 300;
    constexpr int button_height = 28;
    const int right_x = std::max(620, width - side_width - margin);
    const int preview_right = std::max(margin + 100, right_x - margin);

    state.preview_rect = {margin, 72, preview_right, std::max(120, height - margin)};
    if (state.frame) {
        state.image_rect = FitImageRect(state.preview_rect, state.frame->width, state.frame->height);
    }

    SetWindowPos(state.source_combo, nullptr, margin, 16, std::max(200, preview_right - margin - 190), 300, SWP_NOZORDER);
    SetWindowPos(state.refresh_button, nullptr, preview_right - 174, 16, 80, button_height, SWP_NOZORDER);
    SetWindowPos(state.capture_button, nullptr, preview_right - 88, 16, 88, button_height, SWP_NOZORDER);

    int y = 20;
    SetWindowPos(state.status_text, nullptr, right_x, y, side_width, 70, SWP_NOZORDER);
    y += 78;
    SetWindowPos(state.crop_text, nullptr, right_x, y, side_width, 38, SWP_NOZORDER);
    y += 46;

    constexpr int label_width = 22;
    constexpr int edit_width = 55;
    constexpr int gap = 8;
    auto make_label = [&](const wchar_t* label, int x, int top) {
        HWND existing = FindWindowExW(state.hwnd, nullptr, L"STATIC", label);
        if (!existing) {
            existing = CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE, x, top + 5, label_width, 22,
                state.hwnd, nullptr, state.instance, nullptr);
        } else {
            SetWindowPos(existing, nullptr, x, top + 5, label_width, 22, SWP_NOZORDER);
        }
    };
    int x = right_x;
    make_label(L"X", x, y); x += label_width;
    SetWindowPos(state.x_edit, nullptr, x, y, edit_width, button_height, SWP_NOZORDER); x += edit_width + gap;
    make_label(L"Y", x, y); x += label_width;
    SetWindowPos(state.y_edit, nullptr, x, y, edit_width, button_height, SWP_NOZORDER);
    y += 36;
    x = right_x;
    make_label(L"W", x, y); x += label_width;
    SetWindowPos(state.width_edit, nullptr, x, y, edit_width, button_height, SWP_NOZORDER); x += edit_width + gap;
    make_label(L"H", x, y); x += label_width;
    SetWindowPos(state.height_edit, nullptr, x, y, edit_width, button_height, SWP_NOZORDER);
    y += 38;
    SetWindowPos(state.apply_button, nullptr, right_x, y, side_width, button_height, SWP_NOZORDER);
    y += 38;
    SetWindowPos(state.left_button, nullptr, right_x, y, 92, button_height, SWP_NOZORDER);
    SetWindowPos(state.right_button, nullptr, right_x + 100, y, 92, button_height, SWP_NOZORDER);
    y += 36;
    SetWindowPos(state.up_button, nullptr, right_x, y, 92, button_height, SWP_NOZORDER);
    SetWindowPos(state.down_button, nullptr, right_x + 100, y, 92, button_height, SWP_NOZORDER);
    y += 36;
    SetWindowPos(state.smaller_button, nullptr, right_x, y, 110, button_height, SWP_NOZORDER);
    SetWindowPos(state.larger_button, nullptr, right_x + 118, y, 110, button_height, SWP_NOZORDER);
    y += 42;
    SetWindowPos(state.reference_button, nullptr, right_x, y, side_width, button_height, SWP_NOZORDER);
    y += 40;
    SetWindowPos(state.save_button, nullptr, right_x, y, side_width, 34, SWP_NOZORDER);

    InvalidateRect(state.hwnd, nullptr, TRUE);
}

void PaintPreview(AppState& state, HDC dc) {
    HBRUSH background = CreateSolidBrush(RGB(28, 30, 34));
    FillRect(dc, &state.preview_rect, background);
    DeleteObject(background);

    if (!state.frame || state.frame->bgra.empty()) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(215, 215, 215));
        RECT text_rect = state.preview_rect;
        DrawTextW(dc, L"Selecione uma janela e clique em Abrir previa.", -1, &text_rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    state.image_rect = FitImageRect(state.preview_rect, state.frame->width, state.frame->height);
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = static_cast<LONG>(state.frame->width);
    bitmap.bmiHeader.biHeight = -static_cast<LONG>(state.frame->height);
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(
        dc,
        state.image_rect.left,
        state.image_rect.top,
        state.image_rect.right - state.image_rect.left,
        state.image_rect.bottom - state.image_rect.top,
        0,
        0,
        static_cast<int>(state.frame->width),
        static_cast<int>(state.frame->height),
        state.frame->bgra.data(),
        &bitmap,
        DIB_RGB_COLORS,
        SRCCOPY);

    if (state.crop_set) {
        const RECT crop_rect = CropToPreviewRect(state);
        const COLORREF color = state.calibration_valid ? RGB(0, 220, 120) : RGB(255, 170, 30);
        HPEN pen = CreatePen(PS_SOLID, 3, color);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, crop_rect.left, crop_rect.top, crop_rect.right, crop_rect.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
}

void HandleCommand(AppState& state, int id) {
    switch (id) {
    case kRefreshButton:
        RefreshWindows(state);
        SetStatus(state, L"Lista de janelas atualizada.");
        break;
    case kCaptureButton:
        // WinRT activation cannot make outgoing COM calls while processing
        // a synchronous message sent by another process (UI automation).
        PostMessageW(state.hwnd, kStartCaptureMessage, 0, 0);
        break;
    case kApplyButton:
        ApplyCropFromEdits(state);
        break;
    case kLeftButton:
        NudgeCrop(state, -1, 0);
        break;
    case kRightButton:
        NudgeCrop(state, 1, 0);
        break;
    case kUpButton:
        NudgeCrop(state, 0, -1);
        break;
    case kDownButton:
        NudgeCrop(state, 0, 1);
        break;
    case kSmallerButton:
        ResizeCrop(state, -1);
        break;
    case kLargerButton:
        ResizeCrop(state, 1);
        break;
    case kReferenceButton:
        SetReferenceCrop(state);
        break;
    case kSaveButton:
        ValidateCalibration(state);
        try {
            SaveCalibration(state);
            state.saved = LoadCalibration(state.settings_path);
            SetStatus(state, L"Calibracao salva e vinculada a esta fonte, tamanho fisico e DPI.");
        } catch (const std::exception& error) {
            SetStatus(state, L"Falha ao salvar: " + Utf8ToWide(error.what()));
        }
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<AppState*>(create->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WM_CREATE:
        CreateControls(*state);
        state->settings_path = SettingsPath();
        state->saved = LoadCalibration(state->settings_path);
        RefreshWindows(*state);
        LayoutControls(*state);
        SetTimer(hwnd, kPreviewTimer, kPreviewIntervalMs, nullptr);
        if (state->saved.exists) {
            const LRESULT selected = SendMessageW(state->source_combo, CB_GETCURSEL, 0, 0);
            if (state->saved_source_unique && selected != CB_ERR && static_cast<std::size_t>(selected) < state->windows.size() &&
                SameSavedSource(*state, state->windows[static_cast<std::size_t>(selected)])) {
                StartSelectedCapture(*state);
            } else {
                SetStatus(*state, L"Calibracao salva encontrada, mas a fonte exata nao esta disponivel.");
            }
        } else {
            SetStatus(*state, L"Selecione a janela do Edge usada para jogar e abra a previa.");
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {960, 640};
        return 0;
    }
    case WM_SIZE:
        LayoutControls(*state);
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutControls(*state);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kPreviewTimer) {
            PollCapture(*state);
        }
        return 0;
    case kStartCaptureMessage:
        StartSelectedCapture(*state);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) {
            HandleCommand(*state, LOWORD(wparam));
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const auto frame_point = PreviewToFrame(*state, point);
        if (frame_point) {
            state->dragging = true;
            state->drag_anchor = *frame_point;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (state->dragging && state->frame) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            point.x = std::clamp<LONG>(point.x, state->image_rect.left, state->image_rect.right - 1);
            point.y = std::clamp<LONG>(point.y, state->image_rect.top, state->image_rect.bottom - 1);
            const auto frame_point = PreviewToFrame(*state, point);
            if (frame_point) {
                const auto candidate = RatioRectFromDrag(
                    state->drag_anchor, *frame_point, state->frame->width, state->frame->height);
                if (candidate.width > 0 && candidate.height > 0) {
                    state->crop = candidate;
                    state->crop_set = true;
                    state->crop_frame_width = state->frame->width;
                    state->crop_frame_height = state->frame->height;
                    state->crop_dpi = SourceDpi(*state);
                    ValidateCalibration(*state);
                    SyncCropControls(*state);
                    InvalidateRect(hwnd, &state->preview_rect, FALSE);
                }
            }
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        state->dragging = false;
        return 0;
    case WM_LBUTTONUP:
        if (state->dragging) {
            state->dragging = false;
            ReleaseCapture();
            if (state->crop_set) {
                AdoptCurrentBasis(*state);
            }
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintPreview(*state, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kPreviewTimer);
        StopCapture(*state);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The free-threaded frame pool recreates its buffers on the capture
    // callback. Keep its WinRT objects in the same MTA as that worker.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        return static_cast<int>(GetLastError());
    }

    AppState state;
    state.instance = instance;
    HWND hwnd = CreateWindowExW(
        0,
        kWindowClass,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1240,
        820,
        nullptr,
        nullptr,
        instance,
        &state);
    if (hwnd == nullptr) {
        return static_cast<int>(GetLastError());
    }

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
