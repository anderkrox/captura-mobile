"""E2E da Fase 0: executavel Win32 e pipeline externo com quadros sinteticos."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
import tempfile
from pathlib import Path
import struct
import subprocess
import sys
import time


WIDTH, HEIGHT = 486, 864
OUTPUT_WIDTH, OUTPUT_HEIGHT = 1080, 1920
FPS, FRAME_COUNT = 30, 60
CORNER_COLORS = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str], *, data: bytes | None = None, timeout: int = 30) -> bytes:
    result = subprocess.run(
        command, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW,
    )
    if result.returncode:
        raise RuntimeError(
            f"{Path(command[0]).name} terminou com {result.returncode}: "
            + result.stderr.decode('utf-8', errors='replace')
        )
    return result.stdout


def test_application(app: Path) -> list[dict]:
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = (enum_callback, wintypes.LPARAM)
    user32.EnumWindows.restype = wintypes.BOOL
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.GetWindowTextW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    user32.GetWindowTextW.restype = ctypes.c_int
    user32.GetClassNameW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    user32.GetClassNameW.restype = ctypes.c_int
    user32.SendMessageTimeoutW.argtypes = (
        wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
        wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t),
    )
    user32.SendMessageTimeoutW.restype = wintypes.LPARAM
    user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    user32.PostMessageW.restype = wintypes.BOOL

    with tempfile.TemporaryDirectory(prefix='yourots-phase0-') as local_app_data:
        environment = {**os.environ, 'LOCALAPPDATA': local_app_data}
        results = []
        for cycle in range(2):
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
            process = subprocess.Popen([str(app)], startupinfo=startup, cwd=app.parent, env=environment)
            try:
                windows = []

                @enum_callback
                def collect(hwnd, _):
                    pid = wintypes.DWORD()
                    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                    if pid.value == process.pid:
                        name = ctypes.create_unicode_buffer(128)
                        user32.GetClassNameW(hwnd, name, len(name))
                        if name.value == 'YourotsCaptureWindow':
                            windows.append(hwnd)
                    return True

                deadline = time.monotonic() + 8
                while not windows and time.monotonic() < deadline:
                    require(process.poll() is None, 'Aplicativo encerrou antes de criar a janela.')
                    require(bool(user32.EnumWindows(collect, 0)), 'EnumWindows falhou.')
                    if not windows:
                        time.sleep(0.05)

                require(len(windows) == 1, 'A janela principal nao foi criada de forma unica.')
                title = ctypes.create_unicode_buffer(128)
                user32.GetWindowTextW(windows[0], title, len(title))
                require(title.value == 'Yourots Capture', f'Titulo inesperado: {title.value}')
                response = ctypes.c_size_t()
                require(
                    bool(user32.SendMessageTimeoutW(windows[0], 0, 0, 0, 2, 2000, ctypes.byref(response))),
                    'A thread da interface nao respondeu a WM_NULL.',
                )
                require(bool(user32.PostMessageW(windows[0], 0x0010, 0, 0)), 'Envio de WM_CLOSE falhou.')
                require(process.wait(timeout=5) == 0, 'Fechamento normal terminou com erro.')
                results.append({'cycle': cycle + 1, 'title': title.value, 'exit_code': process.returncode})
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
    return results


def fill_rectangle(frame: bytearray, x: int, y: int, w: int, h: int, rgb: tuple[int, int, int]) -> None:
    red, green, blue = rgb
    row = bytes((blue, green, red, 255)) * w
    for row_index in range(y, y + h):
        start = (row_index * WIDTH + x) * 4
        frame[start:start + len(row)] = row


def synthetic_frames() -> bytes:
    base = bytearray(bytes((24, 24, 24, 255)) * (WIDTH * HEIGHT))
    corners = ((0, 0), (WIDTH - 32, 0), (0, HEIGHT - 32), (WIDTH - 32, HEIGHT - 32))
    for (x, y), color in zip(corners, CORNER_COLORS):
        fill_rectangle(base, x, y, 32, 32, color)
    frames = []
    for index in range(FRAME_COUNT):
        frame = base.copy()
        fill_rectangle(frame, 80 + 4 * index, 416, 32, 32, (255, 255, 255))
        frames.append(frame)
    return b''.join(frames)


def mp4_atoms(path: Path) -> list[str]:
    atoms = []
    length = path.stat().st_size
    with path.open('rb') as stream:
        while stream.tell() < length:
            position = stream.tell()
            header = stream.read(8)
            require(len(header) == 8, 'Cabecalho MP4 truncado.')
            size, atom_type = struct.unpack('>I4s', header)
            header_size = 8
            if size == 1:
                extended = stream.read(8)
                require(len(extended) == 8, 'Cabecalho MP4 estendido truncado.')
                size = struct.unpack('>Q', extended)[0]
                header_size = 16
            elif size == 0:
                size = length - position
            require(header_size <= size <= length - position, 'Tamanho invalido de atom MP4.')
            atoms.append(atom_type.decode('ascii'))
            stream.seek(position + size)
    return atoms


def pixel(frame: bytes, x: int, y: int) -> tuple[int, ...]:
    offset = (y * OUTPUT_WIDTH + x) * 3
    return tuple(frame[offset:offset + 3])


def assert_color(actual: tuple[int, ...], expected: tuple[int, ...], location: str) -> None:
    require(
        len(actual) == 3 and max(abs(a - b) for a, b in zip(actual, expected)) <= 25,
        f'Cor incorreta em {location}: {actual}, esperado {expected}.',
    )


def test_video(args, artifacts: Path) -> dict:
    for path, name in ((args.ffmpeg, 'ffmpeg'), (args.ffprobe, 'ffprobe')):
        version = run([str(path), '-hide_banner', '-version']).decode('utf-8').splitlines()[0]
        require(version.startswith(f'{name} version {args.expected_version}-'), f'Versao inesperada: {version}')

    selection = json.loads(run([
        str(args.powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', str(args.encoder_check), '-FfmpegPath', str(args.ffmpeg),
    ]).decode('utf-8-sig'))
    require(selection['CpuAvailable'], 'Fallback CPU indisponivel.')
    encoder = selection['SelectedEncoder']
    require(encoder in ('h264_nvenc', 'libx264'), f'Encoder inesperado: {encoder}')
    require(selection['NvencAvailable'] == (encoder == 'h264_nvenc'), 'Escolha inconsistente com o probe NVENC.')
    print(f'Encoder real selecionado: {encoder}', flush=True)

    temporary = artifacts / 'recording.mkv'
    output = artifacts / 'recording.mp4'
    encoder_arguments = ['-preset', 'veryfast', '-crf', '18'] if encoder == 'libx264' else ['-preset', 'p4', '-cq', '18']
    run([
        str(args.ffmpeg), '-hide_banner', '-loglevel', 'error', '-y',
        '-f', 'rawvideo', '-pixel_format', 'bgra', '-video_size', f'{WIDTH}x{HEIGHT}',
        '-framerate', str(FPS), '-i', 'pipe:0', '-map', '0:v:0', '-an',
        '-vf', (
            f'scale={OUTPUT_WIDTH}:{OUTPUT_HEIGHT}:flags=lanczos:out_color_matrix=bt709:out_range=tv,'
            'setsar=1,format=yuv420p,'
            'setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709'
        ),
        '-c:v', encoder, *encoder_arguments, '-color_range', 'tv',
        '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709',
        str(temporary),
    ], data=synthetic_frames())
    require(temporary.stat().st_size > 0, 'MKV temporario vazio.')

    run([
        str(args.ffmpeg), '-hide_banner', '-loglevel', 'error', '-y',
        '-i', str(temporary), '-map', '0:v:0', '-an', '-c:v', 'copy',
        '-movflags', '+faststart', str(output),
    ])
    metadata = json.loads(run([
        str(args.ffprobe), '-v', 'error', '-count_frames', '-show_streams', '-show_format',
        '-of', 'json', str(output),
    ]))
    require(len(metadata['streams']) == 1, 'MP4 deve conter somente uma faixa de video e nenhum audio.')
    video = metadata['streams'][0]
    expected_values = {
        'codec_type': 'video', 'codec_name': 'h264', 'width': OUTPUT_WIDTH,
        'height': OUTPUT_HEIGHT, 'pix_fmt': 'yuv420p', 'r_frame_rate': '30/1',
        'avg_frame_rate': '30/1', 'sample_aspect_ratio': '1:1',
        'display_aspect_ratio': '9:16', 'color_range': 'tv', 'color_space': 'bt709',
        'color_transfer': 'bt709', 'color_primaries': 'bt709', 'nb_read_frames': str(FRAME_COUNT),
    }
    for key, expected in expected_values.items():
        require(video.get(key) == expected, f'{key}: obtido {video.get(key)}, esperado {expected}.')
    duration = float(metadata['format']['duration'])
    require(abs(duration - FRAME_COUNT / FPS) <= 1 / FPS, f'Duracao incorreta: {duration}.')
    require('mp4' in metadata['format']['format_name'], 'Arquivo final nao e MP4.')
    atoms = mp4_atoms(output)
    require('moov' in atoms and 'mdat' in atoms, 'Estrutura do MP4 incompleta.')
    require(atoms.index('moov') < atoms.index('mdat'), 'faststart nao foi aplicado.')

    # Decodifica todos os quadros: erros de reproducao falham o teste.
    run([str(args.ffmpeg), '-v', 'error', '-xerror', '-i', str(output), '-map', '0:v:0', '-f', 'null', '-'])
    decoded = run([
        str(args.ffmpeg), '-v', 'error', '-i', str(output),
        '-vf', f'select=eq(n\\,0)+eq(n\\,{FRAME_COUNT - 1})', '-fps_mode', 'passthrough',
        '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1',
    ])
    frame_size = OUTPUT_WIDTH * OUTPUT_HEIGHT * 3
    require(len(decoded) == frame_size * 2, 'Nao foi possivel recuperar primeiro e ultimo quadros.')
    samples = ((12, 12), (OUTPUT_WIDTH - 13, 12), (12, OUTPUT_HEIGHT - 13), (OUTPUT_WIDTH - 13, OUTPUT_HEIGHT - 13))
    scale = OUTPUT_WIDTH / WIDTH
    for output_index, source_index in enumerate((0, FRAME_COUNT - 1)):
        frame = decoded[output_index * frame_size:(output_index + 1) * frame_size]
        for corner_index, ((x, y), color) in enumerate(zip(samples, CORNER_COLORS)):
            assert_color(pixel(frame, x, y), color, f'quadro {source_index}, canto {corner_index}')
        marker_x = round((80 + 4 * source_index + 16) * scale)
        marker_y = round(432 * scale)
        assert_color(pixel(frame, marker_x, marker_y), (255, 255, 255), f'marcador do quadro {source_index}')
        old_marker_x = round((80 + 4 * (FRAME_COUNT - 1 - source_index) + 16) * scale)
        assert_color(pixel(frame, old_marker_x, marker_y), (24, 24, 24), f'fundo do quadro {source_index}')

    (artifacts / 'ffprobe.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    return {
        'encoder_selection': selection, 'frames': FRAME_COUNT, 'duration_seconds': duration,
        'resolution': f'{OUTPUT_WIDTH}x{OUTPUT_HEIGHT}', 'audio_streams': 0,
        'faststart': True, 'corners_and_motion_verified': True,
        'mkv': str(temporary), 'mp4': str(output),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ('app', 'ffmpeg', 'ffprobe', 'powershell', 'encoder-check', 'artifacts'):
        parser.add_argument(f'--{name}', required=True, type=Path)
    parser.add_argument('--expected-version', required=True)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'scope': 'phase0_application_and_external_synthetic_video'}
    try:
        for name in ('app', 'ffmpeg', 'ffprobe', 'powershell', 'encoder_check'):
            path = getattr(args, name)
            require(path.is_file(), f'Dependencia ausente: {path}')
        report['application'] = test_application(args.app)
        print('Aplicativo: dois ciclos de abertura, resposta e fechamento aprovados.', flush=True)
        report['video'] = test_video(args, args.artifacts)
        report['status'] = 'passed'
        print('E2E aprovado: BGRA via pipe -> MKV -> MP4; FFprobe, decodificacao e quatro cantos conferidos.', flush=True)
        return 0
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        print(f'E2E reprovado: {error}', file=sys.stderr, flush=True)
        return 1
    finally:
        (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    sys.exit(main())
