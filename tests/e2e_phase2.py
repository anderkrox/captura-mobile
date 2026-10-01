"""Operate the real calibration UI, WGC preview and isolated settings."""

from __future__ import annotations

import argparse
import configparser
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time

from e2e_phase0 import require
from e2e_phase1 import CALLBACK, SourceWindow, user32

gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)
user32.SendMessageTimeoutW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
    wintypes.LPARAM, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t))
user32.SendMessageTimeoutW.restype = wintypes.LPARAM
user32.GetDlgItem.argtypes = (wintypes.HWND, ctypes.c_int)
user32.GetDlgItem.restype = wintypes.HWND
user32.GetClientRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
user32.IsWindowEnabled.argtypes = (wintypes.HWND,)
user32.GetWindowDpiAwarenessContext.argtypes = (wintypes.HWND,)
user32.GetWindowDpiAwarenessContext.restype = wintypes.HANDLE
user32.AreDpiAwarenessContextsEqual.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
user32.GetDpiForWindow.argtypes = (wintypes.HWND,)
user32.ShowWindow.argtypes = (wintypes.HWND, ctypes.c_int)
user32.GetDC.argtypes = (wintypes.HWND,)
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = (wintypes.HWND, wintypes.HDC)
user32.PrintWindow.argtypes = (wintypes.HWND, wintypes.HDC, wintypes.UINT)
gdi32.CreateCompatibleDC.argtypes = (wintypes.HDC,)
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleBitmap.argtypes = (wintypes.HDC, ctypes.c_int, ctypes.c_int)
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = (wintypes.HDC, wintypes.HANDLE)
gdi32.SelectObject.restype = wintypes.HANDLE
gdi32.BitBlt.argtypes = (wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD)
gdi32.GetDIBits.argtypes = (wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
    ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT)
gdi32.DeleteObject.argtypes = (wintypes.HANDLE,)
gdi32.DeleteDC.argtypes = (wintypes.HDC,)

SOURCE, REFRESH, OPEN, STATUS, CROP = 100, 101, 102, 103, 104
EDITS = (105, 106, 107, 108)
APPLY, LEFT, RIGHT, UP, DOWN, SMALLER, LARGER, REFERENCE, SAVE = range(109, 118)
TITLE = 'Yourots E2E — calibração 🎮'


def send(hwnd, message, wparam=0, lparam=0) -> int:
    response = ctypes.c_size_t()
    require(bool(user32.SendMessageTimeoutW(hwnd, message, wparam, lparam, 2, 2000,
                                           ctypes.byref(response))),
            f'Interface sem resposta: mensagem {message:#x}.')
    return ctypes.c_ssize_t(response.value).value


def text(hwnd) -> str:
    buffer = ctypes.create_unicode_buffer(16384)
    send(hwnd, 0x000D, len(buffer), ctypes.addressof(buffer))
    return buffer.value


def set_text(hwnd, value: str) -> None:
    buffer = ctypes.create_unicode_buffer(value)
    require(send(hwnd, 0x000C, 0, ctypes.addressof(buffer)), 'WM_SETTEXT falhou.')


def wait_for(predicate, message: str, *, timeout: float = 8) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.04)
    raise AssertionError(message)


class Application:
    def __init__(self, app: Path, local: Path, *, environment=None):
        self.app, self.local = app, local
        self.environment = environment or os.environ
        self.process = None
        self.hwnd = None

    def __enter__(self):
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 4  # SW_SHOWNOACTIVATE
        self.process = subprocess.Popen([str(self.app)], startupinfo=startup,
            env={**self.environment, 'LOCALAPPDATA': str(self.local)}, cwd=self.app.parent)
        try:
            require(user32.WaitForInputIdle(wintypes.HANDLE(self.process._handle), 5000) == 0,
                    'Aplicativo nao concluiu a inicializacao antes da automacao.')
            @CALLBACK
            def collect(hwnd, _):
                pid = wintypes.DWORD()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value == self.process.pid:
                    name = ctypes.create_unicode_buffer(128)
                    user32.GetClassNameW(hwnd, name, len(name))
                    if name.value == 'YourotsCaptureWindow':
                        self.hwnd = hwnd
                return True

            def find():
                require(self.process.poll() is None, 'Aplicativo encerrou durante a inicializacao.')
                require(user32.EnumWindows(collect, 0), 'EnumWindows falhou.')
                return self.hwnd is not None

            wait_for(find, 'Janela principal ausente.')
            require(text(self.hwnd) == 'Yourots Capture', 'Titulo inesperado.')
            require(user32.SetWindowPos(self.hwnd, None, 650, 100, 1240, 820, 0x0010),
                    'Posicionamento da interface falhou.')
            send(self.hwnd, 0)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, kind, *unused):
        if self.process is None:
            return
        if kind is not None and self.hwnd and self.process.poll() is None:
            try:
                print(f'UI status on failure: {self.status()}', file=sys.stderr)
            except BaseException:
                pass
        try:
            if self.process.poll() is None:
                if self.hwnd:
                    user32.PostMessageW(self.hwnd, 0x0010, 0, 0)
                self.process.wait(timeout=8)
            if kind is None:
                require(self.process.returncode == 0, f'Fechamento falhou: {self.process.returncode}.')
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=5)

    def control(self, identifier):
        hwnd = user32.GetDlgItem(self.hwnd, identifier)
        require(hwnd, f'Controle ausente: {identifier}.')
        return hwnd

    def click(self, identifier):
        send(self.control(identifier), 0x00F5)  # BM_CLICK

    def status(self):
        return text(self.control(STATUS))

    def crop(self):
        return tuple(int(text(self.control(i))) for i in EDITS)

    def valid(self):
        return 'Calibracao valida' in self.status() and bool(user32.IsWindowEnabled(self.control(SAVE)))

    def wait_valid(self):
        wait_for(self.valid, f'Calibracao nao validou. Estado: {self.status()}')

    def select(self, title=TITLE):
        combo = self.control(SOURCE)
        found = []
        for index in range(send(combo, 0x0146)):  # CB_GETCOUNT
            buffer = ctypes.create_unicode_buffer(32768)
            require(send(combo, 0x0148, index, ctypes.addressof(buffer)) >= 0, 'CB_GETLBTEXT falhou.')
            if title in buffer.value:
                found.append(index)
        require(len(found) == 1, f'Selecao da fonte ambigua ou ausente: {found}.')
        require(send(combo, 0x014E, found[0]) == found[0], 'CB_SETCURSEL falhou.')
        self.click(OPEN)
        wait_for(lambda: 'Arraste sobre a previa' in self.status() or self.valid(),
                 f'Previa nao iniciou: {self.status()}')

    def apply(self, crop):
        for identifier, value in zip(EDITS, crop):
            set_text(self.control(identifier), str(value))
        self.click(APPLY)

    def drag(self, start, end):
        def packed(point):
            return (point[1] << 16) | (point[0] & 0xffff)
        send(self.hwnd, 0x0201, 1, packed(start))
        send(self.hwnd, 0x0200, 1, packed(end))
        send(self.hwnd, 0x0202, 0, packed(end))

    def preview_geometry(self, frame_width=600, frame_height=964):
        rect = wintypes.RECT()
        require(user32.GetClientRect(self.hwnd, ctypes.byref(rect)), 'GetClientRect falhou.')
        right = max(620, rect.right - 376) - 16
        bottom = max(120, rect.bottom - 16)
        scale = min((right - 16) / frame_width, (bottom - 72) / frame_height)
        width, height = int(frame_width * scale), int(frame_height * scale)
        return (16 + (right - 16 - width) // 2, 72 + (bottom - 72 - height) // 2, width, height)

    def screenshot(self, path: Path):
        rect = wintypes.RECT()
        require(user32.GetClientRect(self.hwnd, ctypes.byref(rect)), 'GetClientRect falhou.')
        width, height = rect.right, rect.bottom
        dc = user32.GetDC(self.hwnd)
        require(dc, 'GetDC falhou.')
        memory = bitmap = previous = None
        try:
            memory = gdi32.CreateCompatibleDC(dc)
            require(memory, 'CreateCompatibleDC falhou.')
            bitmap = gdi32.CreateCompatibleBitmap(dc, width, height)
            require(bitmap, 'CreateCompatibleBitmap falhou.')
            previous = gdi32.SelectObject(memory, bitmap)
            # Ask the UI thread to paint a complete snapshot into our bitmap.
            # Reading the screen DC can catch the preview between FillRect/StretchDIBits.
            require(user32.PrintWindow(self.hwnd, memory, 1), 'PrintWindow falhou.')
            gdi32.SelectObject(memory, previous)
            previous = None
            info = struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
            header = ctypes.create_string_buffer(info)
            pixels = ctypes.create_string_buffer(width * height * 4)
            require(gdi32.GetDIBits(memory, bitmap, 0, height, pixels, header, 0) == height,
                    'GetDIBits incompleto.')
            path.write_bytes(struct.pack('<2sIHHI', b'BM', 54 + len(pixels.raw), 0, 0, 54) + info + pixels.raw)
            return width, height, pixels.raw
        finally:
            if previous:
                gdi32.SelectObject(memory, previous)
            if bitmap:
                gdi32.DeleteObject(bitmap)
            if memory:
                gdi32.DeleteDC(memory)
            user32.ReleaseDC(self.hwnd, dc)

    def check_preview(self, path: Path):
        time.sleep(0.15)
        width, _, pixels = self.screenshot(path)
        left, top, draw_width, draw_height = self.preview_geometry()
        colors = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0))
        for (fx, fy), color in zip(((69, 62), (530, 62), (69, 901), (530, 901)), colors):
            x, y = left + fx * draw_width // 600, top + fy * draw_height // 964
            offset = (y * width + x) * 4
            b, g, r = pixels[offset:offset + 3]
            require(max(abs(a - b) for a, b in zip((r, g, b), color)) <= 20,
                    f'Previa: canto {fx},{fy} possui {(r,g,b)}, esperado {color}.')
        return pixels


def read_settings(path: Path):
    require(path.is_file(), 'Calibracao nao foi salva.')
    content = path.read_text(encoding='utf-16')
    require('hwnd' not in content.lower(), 'HWND persistido indevidamente.')
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_string(content)
    return {key: value[1:-1] if value.startswith('"') and value.endswith('"') else value
            for key, value in parser['calibration'].items()}


def change_setting(path: Path, key: str, value: str):
    content = path.read_text(encoding='utf-16')
    content, count = re.subn(rf'^{key}=.*$', f'{key}={value}', content, flags=re.MULTILINE)
    require(count == 1, f'Campo ausente: {key}.')
    path.write_text(content, encoding='utf-16')


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ('app', 'fixture', 'artifacts'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    args = parser.parse_args()
    args.app, args.fixture, args.artifacts = args.app.resolve(), args.fixture.resolve(), args.artifacts.resolve()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'scope': 'phase2_real_win32_ui_and_wgc', 'cases': [],
              'dpi_coverage': 'real PMv2 awareness and saved/current DPI mismatch; no physical monitor scale change'}

    def passed(name):
        report['cases'].append(name)
        print(f'PASS {name}', flush=True)

    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        require(args.app.is_file() and args.fixture.is_file(), 'Binarios ausentes.')
        with tempfile.TemporaryDirectory(prefix='local-app-data-', dir=args.artifacts) as directory:
            local = Path(directory)
            settings = local / 'YourotsCapture' / 'settings.ini'
            with SourceWindow(args.fixture) as source:
                set_text(source.hwnd, TITLE)
                with Application(args.app, local) as app:
                    require(user32.AreDpiAwarenessContextsEqual(
                        user32.GetWindowDpiAwarenessContext(app.hwnd), ctypes.c_void_p(-4)), 'PMv2 nao declarado.')
                    require(not user32.IsWindowEnabled(app.control(SAVE)), 'Salvar habilitado sem calibracao.')
                    app.click(APPLY)
                    require(not settings.exists(), 'Configuracao criada sem fonte.')
                    passed('startup_pm_v2_and_unavailable_calibration')
                    app.select()
                    app.check_preview(args.artifacts / 'preview-full.bmp')
                    passed('window_selector_and_native_preview_four_corners')
                    app.apply((57, 50, 486, 864))
                    app.wait_valid()
                    require(app.crop() == (57, 50, 486, 864), 'Recorte fisico incorreto.')
                    app.check_preview(args.artifacts / 'preview-calibrated.bmp')
                    passed('explicit_physical_crop_and_preview')
                    baseline = text(app.control(CROP))
                    for invalid in ((57, 50, 486, 863), (115, 50, 486, 864),
                                    ('4294967296', 50, 486, 864), (57, 50, 0, 0), ('', 50, 486, 864),
                                    ('0' * 63 + '1', 50, 486, 864)):
                        app.apply(invalid)
                        require(text(app.control(CROP)) == baseline, f'Entrada invalida alterou recorte: {invalid}.')
                    app.apply((57, 50, 486, 864))
                    app.wait_valid()
                    passed('invalid_ratio_bounds_overflow_and_empty_edits_rejected')
                    app.click(RIGHT); require(app.crop() == (58, 50, 486, 864), 'Ajuste X incorreto.')
                    app.click(DOWN); require(app.crop() == (58, 51, 486, 864), 'Ajuste Y incorreto.')
                    app.click(LEFT); app.click(UP)
                    require(app.crop() == (57, 50, 486, 864), 'Ajuste inverso incorreto.')
                    app.apply((0, 0, 486, 864)); app.click(LEFT); app.click(UP)
                    require(app.crop() == (0, 0, 486, 864), 'Ajuste ultrapassou origem.')
                    app.click(SMALLER)
                    require(app.crop()[2:] == (477, 848), 'Reducao nao preservou 9:16.')
                    app.click(LARGER)
                    require(app.crop()[2:] == (486, 864), 'Ampliacao nao preservou 9:16.')
                    app.click(REFERENCE)
                    require(app.crop() == (57, 50, 486, 864), 'Referencia nao centralizou 486x864.')
                    passed('fine_position_size_and_reference_controls')
                    left, top, width, height = app.preview_geometry()
                    app.drag((left + width // 4, top + height // 4),
                             (left + width // 2, top + height // 2))
                    app.wait_valid()
                    x, y, w, h = app.crop()
                    require(abs(x - 150) <= 2 and abs(y - 241) <= 2 and w * 16 == h * 9,
                            f'Arraste nao mapeou para textura: {app.crop()}.')
                    before = app.crop()
                    app.drag((16, 72), (25, 85))
                    require(app.crop() == before, 'Clique no letterbox alterou selecao.')
                    app.click(REFERENCE)
                    passed('scaled_preview_drag_and_letterbox_exclusion')
                    require(user32.SetWindowPos(source.hwnd, None, 35, 40, 0, 0, 0x0015), 'Mover fonte falhou.')
                    app.wait_valid()
                    require(app.crop() == (57, 50, 486, 864), 'Mover fonte deslocou recorte.')
                    app.check_preview(args.artifacts / 'preview-after-move.bmp')
                    passed('source_movement_preserves_physical_crop_and_pixels')
                    app.click(SAVE)
                    saved = read_settings(settings)
                    require(saved['title'] == TITLE and saved['process'] == args.fixture.name,
                            'Identidade Unicode nao foi preservada.')
                    require([int(saved[k]) for k in ('cropx', 'cropy', 'cropwidth', 'cropheight')] == [57, 50, 486, 864],
                            'Recorte persistido incorreto.')
                    require(int(saved['dpi']) == user32.GetDpiForWindow(source.hwnd), 'DPI persistido incorreto.')
                    passed('unicode_settings_identity_crop_dpi_without_hwnd')
                saved_bytes = settings.read_bytes()
                with Application(args.app, local) as app:
                    app.wait_valid()
                    require(app.crop() == (57, 50, 486, 864), 'Restauracao incorreta.')
                    app.check_preview(args.artifacts / 'preview-restored.bmp')
                    passed('restart_rechecks_and_restores_saved_source')
                    require(user32.SetWindowPos(source.hwnd, None, 0, 0, 620, 980, 0x0016), 'Resize falhou.')
                    wait_for(lambda: 'Recalibracao necessaria' in app.status(), 'Resize nao invalidou calibracao.')
                    require(not user32.IsWindowEnabled(app.control(SAVE)), 'Resize deixou salvar habilitado.')
                    require(user32.SetWindowPos(source.hwnd, None, 0, 0, 600, 964, 0x0016), 'Restore size falhou.')
                    time.sleep(0.5)
                    require(not app.valid(), 'Voltar ao tamanho anterior aprovou calibracao sem confirmacao.')
                    app.click(REFERENCE); app.wait_valid()
                    passed('resize_invalidates_until_explicit_recalibration')
                    require(user32.SetWindowPos(source.hwnd, None, 0, 0, 300, 500, 0x0016), 'Shrink falhou.')
                    wait_for(lambda: 'Recalibracao necessaria' in app.status(), 'Fonte menor nao invalidou recorte.')
                    previous_crop = text(app.control(CROP))
                    app.click(REFERENCE)
                    require(text(app.control(CROP)) == previous_crop and
                            not user32.IsWindowEnabled(app.control(SAVE)), 'Referencia fora do quadro foi aceita.')
                    app.apply((10, 10, 270, 480)); app.wait_valid()
                    require(app.crop() == (10, 10, 270, 480), 'Recalibracao em fonte menor falhou.')
                    require(user32.SetWindowPos(source.hwnd, None, 0, 0, 600, 964, 0x0016), 'Restore shrink falhou.')
                    wait_for(lambda: 'Recalibracao necessaria' in app.status(), 'Crescimento nao invalidou recorte.')
                    app.click(REFERENCE); app.wait_valid()
                    passed('shrink_rejects_reference_and_accepts_smaller_mobile_crop')
                    user32.ShowWindow(source.hwnd, 6)
                    wait_for(lambda: not user32.IsWindowEnabled(app.control(SAVE)), 'Fonte minimizada continua valida.')
                    require(settings.read_bytes() == saved_bytes, 'Minimizar alterou arquivo salvo.')
                    user32.ShowWindow(source.hwnd, 4)
                    app.wait_valid()
                    passed('minimized_source_blocks_save_and_restores')
                    require(user32.PostMessageW(source.hwnd, 0x0010, 0, 0), 'Fechar fonte falhou.')
                    source.process.wait(timeout=5)
                    wait_for(lambda: 'fonte foi fechada' in app.status(), 'Fechamento nao informado.')
                    require(not user32.IsWindowEnabled(app.control(SAVE)), 'Fonte fechada continua valida.')
                    app.click(OPEN)  # stale source in the combo must fail without crashing
                    wait_for(lambda: 'Falha ao iniciar captura' in app.status(), 'HWND expirado nao foi rejeitado.')
                    passed('source_close_and_stale_selection_fail_without_crash')
            with Application(args.app, local) as app:
                require('fonte exata nao esta disponivel' in app.status(), 'Fonte ausente foi restaurada.')
                require(not user32.IsWindowEnabled(app.control(SAVE)), 'Fonte ausente permite salvar.')
                passed('missing_saved_source_requires_selection')
            with SourceWindow(args.fixture) as source:
                set_text(source.hwnd, TITLE)
                with Application(args.app, local) as app:
                    app.wait_valid()
                    require(app.crop() == (57, 50, 486, 864), 'Nova instancia da fonte nao foi reconhecida.')
                    passed('new_source_process_restores_by_identity')
                with SourceWindow(args.fixture) as duplicate:
                    set_text(duplicate.hwnd, TITLE)
                    with Application(args.app, local) as app:
                        require('fonte exata nao esta disponivel' in app.status(), 'Fonte ambigua foi escolhida automaticamente.')
                        require(not user32.IsWindowEnabled(app.control(SAVE)), 'Fonte ambigua permite salvar.')
                    passed('ambiguous_saved_identity_does_not_auto_select')
                change_setting(settings, 'dpi', str(user32.GetDpiForWindow(source.hwnd) + 48))
                with Application(args.app, local) as app:
                    wait_for(lambda: 'DPI da fonte mudou' in app.status(), 'DPI divergente foi aprovado.')
                    require(not user32.IsWindowEnabled(app.control(SAVE)), 'DPI divergente permite salvar.')
                    app.click(REFERENCE); app.wait_valid()
                passed('saved_dpi_mismatch_requires_recalibration')
                settings.write_bytes(saved_bytes)
                change_setting(settings, 'frameWidth', '620')
                with Application(args.app, local) as app:
                    wait_for(lambda: 'o quadro mudou' in app.status(), 'Dimensao divergente foi aprovada.')
                    require(not user32.IsWindowEnabled(app.control(SAVE)), 'Dimensao divergente permite salvar.')
                passed('saved_frame_mismatch_requires_recalibration')
                settings.write_bytes(saved_bytes)
                change_setting(settings, 'cropX', '-1')
                with Application(args.app, local) as app:
                    require('Selecione a janela' in app.status(), 'Arquivo corrompido foi aplicado.')
                    app.select(); app.click(REFERENCE); app.wait_valid()
                    settings.unlink()
                    settings.mkdir()  # force the real atomic replacement to fail
                    app.click(SAVE)
                    require('Falha ao salvar' in app.status(), 'Falha de escrita nao informada.')
                    require(settings.is_dir(), 'Destino da falha foi removido indevidamente.')
                    require(not list(settings.parent.glob('*.tmp')), 'Arquivo temporario vazou.')
                passed('corrupt_settings_and_write_failure_are_controlled')
        report['status'] = 'passed'
    except BaseException as error:
        report['status'] = 'failed'
        report['error'] = f'{type(error).__name__}: {error}'
        print(report['error'], file=sys.stderr)
    report['case_count'] = len(report['cases'])
    (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
