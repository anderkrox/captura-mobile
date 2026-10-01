"""Real WGC -> D3D11 crop -> PNG / FFmpeg with a deterministic native window."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import sys
import time

from e2e_phase0 import mp4_atoms, require, run


WIDTH, HEIGHT, CROP = 486, 864, '57,50,486,864'
CORNER_COLORS = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0))
user32 = ctypes.WinDLL('user32', use_last_error=True)
CALLBACK = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = (CALLBACK, wintypes.LPARAM)
user32.EnumWindows.restype = wintypes.BOOL
user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
user32.GetClassNameW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
user32.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, wintypes.UINT)
user32.SetProcessDpiAwarenessContext.argtypes = (wintypes.HANDLE,)


class SourceWindow:
    def __init__(self, fixture: Path, *, static: bool = False):
        self.fixture, self.static = fixture, static
        self.process = None
        self.hwnd = None

    def __enter__(self):
        self.process = subprocess.Popen(
            [str(self.fixture), *(['--static'] if self.static else [])],
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        try:
            @CALLBACK
            def collect(hwnd, _):
                pid = wintypes.DWORD()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value == self.process.pid:
                    name = ctypes.create_unicode_buffer(128)
                    user32.GetClassNameW(hwnd, name, len(name))
                    if name.value == 'YourotsCaptureE2EFixture':
                        self.hwnd = hwnd
                return True

            deadline = time.monotonic() + 8
            while self.hwnd is None and time.monotonic() < deadline:
                require(self.process.poll() is None, 'Fixture encerrou antes da janela.')
                require(user32.EnumWindows(collect, 0), 'EnumWindows falhou.')
                time.sleep(0.05)
            require(self.hwnd is not None, 'Janela fonte ausente.')
            time.sleep(0.15)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *unused):
        if self.process is not None and self.process.poll() is None:
            if self.hwnd:
                user32.PostMessageW(self.hwnd, 0x0010, 0, 0)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)


def diagnostics(output: str) -> dict[str, str]:
    return dict(line.split('=', 1) for line in output.splitlines() if '=' in line)


def check_pixels(frame: bytes, *, width: int = WIDTH, height: int = HEIGHT,
                 tolerance: int = 0) -> None:
    require(len(frame) == width * height * 3, 'Buffer de pixels incompleto.')
    points = ((12, 12), (width - 13, 12), (12, height - 13), (width - 13, height - 13))
    for (x, y), expected in zip(points, CORNER_COLORS):
        offset = (y * width + x) * 3
        actual = tuple(frame[offset:offset + 3])
        require(max(abs(a - b) for a, b in zip(actual, expected)) <= tolerance,
                f'Canto {x},{y}: obtido {actual}; esperado {expected}.')
    magenta = sum(frame[i] > 245 and frame[i + 1] < 10 and frame[i + 2] > 245
                  for i in range(0, len(frame), 3))
    require(magenta == 0, f'{magenta} pixels de fora do recorte/overlay apareceram.')


def decode_png(args, path: Path) -> bytes:
    metadata = json.loads(run([str(args.ffprobe), '-v', 'error', '-show_streams', '-of', 'json', str(path)]))
    stream = metadata['streams'][0]
    require((stream['width'], stream['height']) == (WIDTH, HEIGHT), 'Dimensoes do PNG incorretas.')
    return run([str(args.ffmpeg), '-v', 'error', '-i', str(path), '-frames:v', '1',
                '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])


def verify_video(args, path: Path, seconds: int, *, moving: bool) -> dict:
    metadata = json.loads(run([str(args.ffprobe), '-v', 'error', '-count_frames',
                              '-show_streams', '-show_format', '-of', 'json', str(path)]))
    require(len(metadata['streams']) == 1, 'Video deve ter uma faixa de video e nenhum audio.')
    stream = metadata['streams'][0]
    expected = {'codec_type': 'video', 'codec_name': 'h264', 'width': 1080, 'height': 1920,
                'pix_fmt': 'yuv420p', 'r_frame_rate': '30/1', 'avg_frame_rate': '30/1',
                'sample_aspect_ratio': '1:1', 'display_aspect_ratio': '9:16',
                'color_range': 'tv', 'color_space': 'bt709',
                'color_transfer': 'bt709', 'color_primaries': 'bt709',
                'nb_read_frames': str(seconds * 30)}
    for key, value in expected.items():
        require(stream.get(key) == value, f'{key}: {stream.get(key)}, esperado {value}.')
    require(abs(float(metadata['format']['duration']) - seconds) < 1 / 30, 'Duracao incorreta.')
    atoms = mp4_atoms(path)
    require(atoms.index('moov') < atoms.index('mdat'), 'faststart ausente.')
    run([str(args.ffmpeg), '-v', 'error', '-xerror', '-i', str(path), '-f', 'null', '-'])
    indices = (0, seconds * 15, seconds * 30 - 1)
    select = '+'.join(f'eq(n\\,{index})' for index in indices)
    decoded = run([str(args.ffmpeg), '-v', 'error', '-i', str(path),
                   '-vf', f'select={select},scale={WIDTH}:{HEIGHT}', '-fps_mode', 'passthrough',
                   '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
    frame_size = WIDTH * HEIGHT * 3
    require(len(decoded) == frame_size * 3, 'Nao foram recuperados os tres quadros.')
    marker_centers = []
    for index in range(3):
        frame = decoded[index * frame_size:(index + 1) * frame_size]
        check_pixels(frame, tolerance=25)
        positions = [x for x in range(40, 400)
                     if min(frame[(432 * WIDTH + x) * 3:(432 * WIDTH + x) * 3 + 3]) > 220]
        require(len(positions) >= 24, 'Marcador branco nao encontrado.')
        marker_centers.append(sum(positions) / len(positions))
    if moving:
        require(max(marker_centers) - min(marker_centers) > 8, 'Captura congelada: movimento ausente.')
    else:
        require(max(marker_centers) - min(marker_centers) <= 1, 'Cena estatica alterada.')
    path.with_suffix('.ffprobe.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    return {'seconds': seconds, 'frames': seconds * 30, 'audio_streams': 0,
            'marker_centers': marker_centers, 'resolution': '1080x1920', 'faststart': True}


def execute(args, name: str, command: list[str], *, success: bool = True, timeout: int = 30) -> str:
    result = subprocess.run([str(args.app), *command], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW)
    output = result.stdout.decode('utf-8', errors='replace')
    error = result.stderr.decode('utf-8', errors='replace')
    (args.artifacts / f'{name}.log').write_text(
        f'exit_code={result.returncode}\n{output}{error}', encoding='utf-8')
    if success:
        require(result.returncode == 0, f'{name}: codigo {result.returncode}: {output}{error}')
    else:
        require(result.returncode in (1, 2), f'{name}: esperada falha controlada, codigo {result.returncode}.')
        require(bool(error.strip()), f'{name}: falha sem diagnostico.')
    return output


def recording_failure(args, source: SourceWindow, kind: str) -> None:
    log = args.artifacts / f'{kind}.log'
    output = args.artifacts / f'{kind}.mp4'
    with log.open('wb') as stream:
        process = subprocess.Popen(
            [str(args.app), '--hwnd', str(source.hwnd), '--crop', CROP,
             '--ffmpeg', str(args.ffmpeg), '--record', str(output), '--seconds', '10'],
            stdout=stream, stderr=stream, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 10
            while 'record_started=true' not in log.read_text(encoding='utf-8', errors='replace'):
                require(process.poll() is None, f'{kind}: captura nao inicializou.')
                require(time.monotonic() < deadline, f'{kind}: tempo excedido antes da captura.')
                time.sleep(0.05)
            if kind == 'source_closed':
                require(user32.PostMessageW(source.hwnd, 0x0010, 0, 0), 'WM_CLOSE falhou.')
                source.process.wait(timeout=5)
            else:
                require(user32.SetWindowPos(source.hwnd, None, 0, 0, 300, 500, 0x0016),
                        'Redimensionamento da fonte falhou.')
            require(process.wait(timeout=15) in (1, 2), f'{kind}: gravacao indicou sucesso incorreto.')
            text = log.read_text(encoding='utf-8', errors='replace')
            require('recording=' not in text and 'Erro:' in text, f'{kind}: falha sem diagnostico.')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ('app', 'fixture', 'ffmpeg', 'ffprobe', 'artifacts'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    args = parser.parse_args()
    for name in ('app', 'fixture', 'ffmpeg', 'ffprobe', 'artifacts'):
        setattr(args, name, getattr(args, name).resolve())
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'scope': 'phase1_real_windows_graphics_capture', 'cases': []}
    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        for name in ('app', 'fixture', 'ffmpeg', 'ffprobe'):
            require(getattr(args, name).is_file(), f'Dependencia ausente: {name}.')
        with SourceWindow(args.fixture) as source:
            base = ['--hwnd', str(source.hwnd), '--crop', CROP]
            listed = execute(args, 'list_windows', ['--list-windows'])
            require(str(source.hwnd) in listed and 'Yourots capture E2E source' in listed,
                    'Seletor nao lista a janela fonte.')
            report['cases'].append('window_selection')
            for cycle in range(6):
                png = args.artifacts / f'snapshot-{cycle}.png'
                diag = diagnostics(execute(args, f'snapshot_{cycle}', [*base, '--snapshot', str(png)]))
                require(diag.get('graphics_capture_supported') == 'true', 'WGC nao suportado.')
                require(diag.get('callback_thread') == 'free_threaded_frame_pool', 'Pool inesperado.')
                require(int(diag['callback_thread_id']) > 0 and
                        diag['callback_thread_id'] != diag['control_thread_id'],
                        'Quadro recebido na thread de controle.')
                check_pixels(decode_png(args, png))
            report['cases'].append('six_capture_start_stop_cycles_and_png_corners')
            overlay = args.artifacts / 'overlay.png'
            diag = diagnostics(execute(args, 'overlay', [*base, '--overlay-snapshot', str(overlay)]))
            require(diag.get('overlay_visible') == 'true', 'Overlay nao estava visivel no desktop.')
            require(diag.get('overlay_magenta_pixels') == '0', 'Overlay incorporado a captura.')
            check_pixels(decode_png(args, overlay))
            report['cases'].append('visible_overlay_excluded')
            video = args.artifacts / 'gravação com espaços.mp4'
            execute(args, 'moving_video', [*base, '--record', str(video), '--seconds', '3',
                                           '--ffmpeg', str(args.ffmpeg)])
            report['moving_video'] = verify_video(args, video, 3, moving=True)
            report['cases'].append('native_capture_moving_video_and_unicode_path')
            execute(args, 'invalid_bounds', ['--hwnd', str(source.hwnd), '--crop', '57,101,486,864'], success=False)
            execute(args, 'missing_ffmpeg', [*base, '--record', str(args.artifacts / 'missing.mp4'),
                                            '--ffmpeg', str(args.artifacts / 'missing.exe')], success=False)
            execute(args, 'encoder_failure', [*base, '--record', str(args.artifacts / 'bad.unknown'),
                                             '--ffmpeg', str(args.ffmpeg), '--seconds', '1'], success=False)
            report['cases'].append('invalid_crop_missing_ffmpeg_encoder_failure')
        with SourceWindow(args.fixture, static=True) as source:
            video = args.artifacts / 'static.mp4'
            execute(args, 'static_video', ['--hwnd', str(source.hwnd), '--crop', CROP,
                '--overlay-snapshot', str(args.artifacts / 'static-overlay.png'),
                '--record', str(video), '--seconds', '2', '--ffmpeg', str(args.ffmpeg)])
            check_pixels(decode_png(args, args.artifacts / 'static-overlay.png'))
            report['static_video'] = verify_video(args, video, 2, moving=False)
            report['cases'].append('static_source_overlay_and_duration')
        for kind in ('source_closed', 'source_resized'):
            with SourceWindow(args.fixture) as source:
                recording_failure(args, source, kind)
            report['cases'].append(kind + '_reported_as_failure')
        for name, command in (
            ('bad_crop', ['--crop', '0,0,486,864junk']),
            ('bad_seconds', ['--seconds', '-1']),
            ('missing_argument', ['--crop']),
            ('unknown_argument', ['--does-not-exist']),
            ('invalid_hwnd', ['--hwnd', '1']),
        ):
            execute(args, name, command, success=False)
        report['cases'].append('invalid_cli_arguments')
        report['status'] = 'passed'
        print(f'E2E Fase 1 aprovado: {len(report["cases"])} cenarios com captura nativa real.', flush=True)
        return 0
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        print(f'E2E Fase 1 reprovado: {error}', file=sys.stderr, flush=True)
        return 1
    finally:
        (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')


if __name__ == '__main__':
    sys.exit(main())
