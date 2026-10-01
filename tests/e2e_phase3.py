"""Exercise the production Recorder through WGC, real media tools and fault injection."""

from __future__ import annotations

import argparse
import configparser
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from e2e_phase0 import mp4_atoms, require, run
from e2e_phase1 import CROP, WIDTH, HEIGHT, SourceWindow, check_pixels, diagnostics, user32

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
kernel32.CreateJobObjectW.restype = wintypes.HANDLE
kernel32.SetInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
user32.GetDpiForWindow.argtypes = (wintypes.HWND,)
user32.GetWindowTextW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)


class BasicLimits(ctypes.Structure):
    _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                ('flags', wintypes.DWORD), ('min_working', ctypes.c_size_t),
                ('max_working', ctypes.c_size_t), ('active', wintypes.DWORD),
                ('affinity', ctypes.c_size_t), ('priority', wintypes.DWORD), ('scheduling', wintypes.DWORD)]


class ExtendedLimits(ctypes.Structure):
    _fields_ = [('basic', BasicLimits), ('io', ctypes.c_uint64 * 6),
                ('process_memory', ctypes.c_size_t), ('job_memory', ctypes.c_size_t),
                ('peak_process', ctypes.c_size_t), ('peak_job', ctypes.c_size_t)]


class ProcessTree:
    """Close the entire test process tree, including encoders, on success or failure."""
    def __init__(self, command, **options):
        self.command, self.options = command, options
        self.job = self.process = None

    def __enter__(self):
        self.job = kernel32.CreateJobObjectW(None, None)
        require(self.job, 'CreateJobObjectW falhou.')
        try:
            limits = ExtendedLimits()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            require(kernel32.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)),
                    'SetInformationJobObject falhou.')
            self.process = subprocess.Popen(self.command, creationflags=subprocess.CREATE_NO_WINDOW, **self.options)
            require(kernel32.AssignProcessToJobObject(self.job, wintypes.HANDLE(self.process._handle)),
                    'AssignProcessToJobObject falhou.')
            return self.process
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *unused):
        if self.job:
            kernel32.CloseHandle(self.job)
            self.job = None
        if self.process and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)


def execute(args, name, command, *, environment, success=True, message=None, executable=None):
    if '--output' in command:
        path = Path(command[command.index('--output') + 1])
        if path.is_file(): path.unlink()
        path.with_suffix('.recording.mkv').unlink(missing_ok=True)
    with ProcessTree([str(executable or args.app), *map(str, command)], env=environment,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=args.app.parent) as process:
        output, error = process.communicate(timeout=30)
        text = (output + error).decode('utf-8', errors='replace')
        (args.artifacts / f'{name}.log').write_text(f'exit_code={process.returncode}\n{text}', encoding='utf-8')
        if success:
            require(process.returncode == 0, f'{name}: codigo {process.returncode}: {text}')
        else:
            require(process.returncode in (1, 2), f'{name}: codigo de falha inesperado: {process.returncode}: {text}')
            require('Erro:' in text, f'{name}: falha sem diagnostico: {text}')
        if message:
            require(message in text, f'{name}: diagnostico esperado {message!r} ausente: {text}')
        return diagnostics(output.decode('utf-8', errors='replace'))


def verify_video(args, path, frames, seconds, *, moving):
    metadata = json.loads(run([str(args.ffprobe), '-v', 'error', '-count_frames',
                              '-show_streams', '-show_format', '-of', 'json', str(path)]))
    require(len(metadata['streams']) == 1, 'MP4 deve conter somente video, sem audio.')
    stream = metadata['streams'][0]
    expected = {'codec_type': 'video', 'codec_name': 'h264', 'width': 1080, 'height': 1920,
                'pix_fmt': 'yuv420p', 'r_frame_rate': '30/1', 'avg_frame_rate': '30/1',
                'sample_aspect_ratio': '1:1', 'display_aspect_ratio': '9:16',
                'color_range': 'tv', 'color_space': 'bt709', 'color_transfer': 'bt709',
                'color_primaries': 'bt709', 'nb_read_frames': str(frames)}
    for key, value in expected.items():
        require(stream.get(key) == value, f'{path.name} {key}: {stream.get(key)}, esperado {value}.')
    duration = float(metadata['format']['duration'])
    require(abs(duration - frames / 30) <= 1 / 30, f'Duracao diverge dos quadros: {duration}, {frames}.')
    require(seconds <= duration <= seconds + 0.15, f'Duracao diverge do relogio: {duration}, {seconds}.')
    require('mp4' in metadata['format']['format_name'], 'Container final nao e MP4.')
    atoms = mp4_atoms(path)
    require(atoms.index('moov') < atoms.index('mdat'), 'MP4 sem faststart.')
    run([str(args.ffmpeg), '-v', 'error', '-xerror', '-i', str(path), '-f', 'null', '-'])
    packets = json.loads(run([str(args.ffprobe), '-v', 'error', '-show_packets',
        '-show_entries', 'packet=pts,dts,duration', '-of', 'json', str(path)]))['packets']
    require(stream['time_base'] == '1/15360', 'Timebase do MP4 incorreta.')
    require(sorted(p['pts'] for p in packets) == list(range(0, frames * 512, 512)), 'PTS fora da cadencia de 30 FPS.')
    require(all(b['dts'] - a['dts'] == 512 for a, b in zip(packets, packets[1:])), 'DTS irregular.')
    require(all(p['duration'] == 512 for p in packets), 'Duracao de pacote irregular.')
    indices = (0, frames // 2, frames - 1)
    select = '+'.join(f'eq(n\\,{i})' for i in indices)
    decoded = run([str(args.ffmpeg), '-v', 'error', '-i', str(path), '-vf', f'select={select},scale={WIDTH}:{HEIGHT}',
                   '-fps_mode', 'passthrough', '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
    size = WIDTH * HEIGHT * 3
    require(len(decoded) == size * 3, 'Tres quadros nao foram decodificados.')
    centers = []
    for index in range(3):
        frame = decoded[index * size:(index + 1) * size]
        check_pixels(frame, tolerance=25)
        points = [x for x in range(40, 400) if min(frame[(432 * WIDTH + x) * 3:(432 * WIDTH + x) * 3 + 3]) > 220]
        require(len(points) >= 24, 'Marcador ausente.')
        centers.append(sum(points) / len(points))
    if moving:
        require(max(centers) - min(centers) > 8, 'Video congelado: movimento ausente.')
    else:
        require(max(centers) - min(centers) <= 1, 'Cena estatica alterada.')
    path.with_suffix('.ffprobe.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    return {'frames': frames, 'duration_seconds': duration, 'marker_centers': centers,
            'faststart': True, 'full_decode': True, 'four_corners': True, 'audio_streams': 0}


def saved_settings(local, source, **overrides):
    title = ctypes.create_unicode_buffer(1024)
    user32.GetWindowTextW(source.hwnd, title, len(title))
    values = {'process': 'CaptureFixture.exe', 'title': title.value, 'class': 'YourotsCaptureE2EFixture',
              'frameWidth': '600', 'frameHeight': '964', 'dpi': str(user32.GetDpiForWindow(source.hwnd)),
              'cropX': '57', 'cropY': '50', 'cropWidth': '486', 'cropHeight': '864', **overrides}
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    config['calibration'] = values
    path = local / 'YourotsCapture' / 'settings.ini'
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w', encoding='utf-16') as stream:
        config.write(stream, space_around_delimiters=False)
    return path


def source_failure(args, environment, kind):
    with SourceWindow(args.fixture) as source:
        path = args.artifacts / f'{kind}.mp4'
        path.unlink(missing_ok=True)
        path.with_suffix('.recording.mkv').unlink(missing_ok=True)
        log = args.artifacts / f'{kind}.log'
        with log.open('wb') as stream, ProcessTree([str(args.app), '--hwnd', str(source.hwnd), '--crop', CROP,
                '--ffmpeg', str(args.ffmpeg), '--ffprobe', str(args.ffprobe), '--output', str(path),
                '--seconds', '10'], env=environment, stdout=stream, stderr=stream) as process:
            deadline = time.monotonic() + 10
            while 'recording_started=true' not in log.read_text(encoding='utf-8', errors='replace'):
                require(process.poll() is None and time.monotonic() < deadline, f'{kind}: captura nao iniciou.')
                time.sleep(0.03)
            time.sleep(0.3)
            if kind == 'source_closed':
                require(user32.PostMessageW(source.hwnd, 0x0010, 0, 0), 'WM_CLOSE falhou.')
                source.process.wait(timeout=5)
            else:
                # Grow while keeping crop in bounds: any size change invalidates calibration.
                require(user32.SetWindowPos(source.hwnd, None, 0, 0, 640, 1000, 0x0016), 'Resize falhou.')
            require(process.wait(timeout=10) in (1, 2), f'{kind}: sucesso incorreto.')
            text = log.read_text(encoding='utf-8', errors='replace')
            require('Erro:' in text, f'{kind}: diagnostico ausente.')
            require(not path.exists(), f'{kind}: MP4 final nao deveria existir.')
            require(path.with_suffix('.recording.mkv').stat().st_size > 0, f'{kind}: MKV nao preservado.')


def main():
    parser = argparse.ArgumentParser()
    for name in ('app', 'lifecycle', 'tool-fixture', 'fixture', 'ffmpeg', 'ffprobe', 'artifacts'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'scope': 'phase3_production_recorder_real_wgc_and_media_tools', 'cases': []}

    def passed(name):
        report['cases'].append(name)
        print(f'PASS {name}', flush=True)

    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        for name in ('app', 'lifecycle', 'tool_fixture', 'fixture', 'ffmpeg', 'ffprobe'):
            require(getattr(args, name).is_file(), f'Dependencia ausente: {name}.')
        with tempfile.TemporaryDirectory(prefix='yourots-phase3-') as temp:
            local = Path(temp)
            env = {**os.environ, 'LOCALAPPDATA': str(local), 'YOUROTS_TEST_FFMPEG': str(args.ffmpeg),
                   'YOUROTS_TEST_FFPROBE': str(args.ffprobe)}
            tools = ['--ffmpeg', str(args.ffmpeg), '--ffprobe', str(args.ffprobe)]
            execute(args, 'no_calibration', [*tools], environment=env, success=False, message='Nao ha calibracao salva')
            passed('missing_saved_calibration_rejected')
            with SourceWindow(args.fixture) as source:
                base = ['--hwnd', str(source.hwnd), '--crop', CROP, *tools]
                path = args.artifacts / 'pasta com ação 🎮' / 'gravação com espaços.MP4'
                diag = execute(args, 'moving', [*base, '--output', path, '--seconds', '2'], environment=env)
                require(diag['encoder'] in ('libx264', 'h264_nvenc'), 'Encoder inesperado.')
                require(not path.with_suffix('.recording.mkv').exists(), 'MKV nao removido apos sucesso.')
                report['moving_video'] = verify_video(args, path, int(diag['frames']), 2, moving=True)
                report['encoder'] = diag['encoder']
                passed('moving_native_capture_unicode_path_metadata_pixels_and_faststart')
                saved = saved_settings(local, source)
                saved_before = saved.read_bytes()
                path = args.artifacts / 'saved.mp4'
                diag = execute(args, 'saved', [*tools, '--output', path, '--seconds', '1'], environment=env)
                verify_video(args, path, int(diag['frames']), 1, moving=True)
                require(saved.read_bytes() == saved_before, 'Calibracao salva foi alterada.')
                passed('saved_source_identity_crop_frame_and_dpi_used_without_modification')
                for name, overrides in [('saved_dpi', {'dpi': str(user32.GetDpiForWindow(source.hwnd) + 48)}),
                                        ('saved_size', {'frameWidth': '640'})]:
                    saved_settings(local, source, **overrides)
                    execute(args, name, [*tools], environment=env, success=False, message='calibracao salva nao e valida')
                saved_settings(local, source)
                with SourceWindow(args.fixture):
                    execute(args, 'ambiguous', [*tools], environment=env, success=False, message='exatamente uma janela')
                saved.unlink()
                passed('saved_dpi_frame_and_ambiguous_identity_rejected')
                for name, command, message in (
                    ('bad_ratio', ['--hwnd', str(source.hwnd), '--crop', '57,50,486,863', *tools], 'proporcao 9:16'),
                    ('bad_bounds', ['--hwnd', str(source.hwnd), '--crop', '57,101,486,864', *tools], 'fora do quadro'),
                    ('missing_ffmpeg', [*base, '--ffmpeg', args.artifacts / 'absent.exe'], 'FFmpeg nao encontrado'),
                    ('missing_ffprobe', [*base, '--ffprobe', args.artifacts / 'absent.exe'], 'FFprobe nao encontrado'),
                    ('bad_output', [*base, '--output', args.artifacts / 'bad.mkv'], 'extensao .mp4'),
                    ('bad_hwnd_suffix', ['--hwnd', str(source.hwnd) + 'junk', '--crop', CROP], 'HWND invalido'),
                    ('hwnd_without_crop', ['--hwnd', str(source.hwnd)], 'juntos'),
                    ('crop_without_hwnd', ['--crop', CROP], 'juntos'),
                    ('bad_seconds', ['--seconds', '0'], 'entre 1 e 3600'),
                    ('bad_seconds_suffix', ['--seconds', '1junk'], 'digitos'),
                    ('missing_value', ['--crop'], 'sem valor'),
                    ('unknown_argument', ['--unknown'], 'desconhecido'),
                    ('invalid_hwnd', ['--hwnd', '1', '--crop', CROP], 'janela valida'),
                ):
                    execute(args, name, command, environment=env, success=False, message=message)
                passed('invalid_cli_crop_tools_output_and_source_rejected')
                for mode in ('cycles', 'pressure', 'recovery', 'destructor'):
                    tool_mode = {'cycles': 'cpu', 'pressure': 'slow_cpu', 'recovery': 'encoder_exit', 'destructor': 'cpu'}[mode]
                    path = args.artifacts / f'lifecycle-{mode}.mp4'
                    path.unlink(missing_ok=True)
                    path.with_suffix('.recording.mkv').unlink(missing_ok=True)
                    diag = execute(args, f'lifecycle_{mode}', [str(source.hwnd), args.tool_fixture,
                        args.ffprobe, path, mode], environment={**env, 'YOUROTS_TEST_TOOL_MODE': tool_mode},
                        executable=args.lifecycle)
                    if mode != 'destructor':
                        for i in range(1, 3 if mode == 'cycles' else 2):
                            verify_video(args, path.with_suffix(f'.cycle{i}.mp4'), int(diag[f'cycle{i}_frames']), 1.1, moving=True)
                        require(diag['cycle1_encoder'] == 'libx264', 'Fallback CPU nao selecionado.')
                    else:
                        run([str(args.ffmpeg), '-v', 'error', '-xerror', '-i', str(path.with_suffix('.recording.mkv')), '-f', 'null', '-'])
                    if mode == 'pressure':
                        report['queue_overflows'] = int(diag['cycle1_overflows'])
                        require(report['queue_overflows'] > 0, 'Fila nao transbordou no teste de pressao.')
                    passed(f'recorder_lifecycle_{mode}')
                for mode in ('verbose_cpu', 'all_encoders_fail', 'encoder_exit', 'remux_fail', 'probe_fail',
                             'audio_fail', 'unexpected_audio', 'probe_nan'):
                    path = args.artifacts / f'{mode}.mp4'
                    diag = execute(args, mode, ['--hwnd', str(source.hwnd), '--crop', CROP,
                        '--ffmpeg', args.tool_fixture, '--ffprobe', args.tool_fixture, '--output', path, '--seconds', '1'],
                        environment={**env, 'YOUROTS_TEST_TOOL_MODE': mode}, success=mode == 'verbose_cpu',
                        message='duracao diverge' if mode == 'probe_nan' else None)
                    if mode == 'verbose_cpu':
                        require(diag['encoder'] == 'libx264', 'Fallback apos diagnostico extenso falhou.')
                        verify_video(args, path, int(diag['frames']), 1, moving=True)
                    elif mode not in ('all_encoders_fail', 'encoder_exit'):
                        require(path.with_suffix('.recording.mkv').stat().st_size > 0, f'{mode}: MKV nao preservado.')
                        if mode == 'probe_fail':
                            def hashes(video):
                                data = json.loads(run([str(args.ffprobe), '-v', 'error', '-show_packets',
                                    '-show_data_hash', 'sha256', '-show_entries', 'packet=data_hash', '-of', 'json', str(video)]))
                                return [p['data_hash'] for p in data['packets']]
                            require(hashes(path) == hashes(path.with_suffix('.recording.mkv')),
                                    'Remux alterou os pacotes H.264: video foi recodificado.')
                    else:
                        require(not path.exists(), f'{mode}: MP4 incorreto.')
                    passed(f'media_tool_{mode}')
                blocked = args.artifacts / 'blocked.mp4'
                blocked.mkdir(exist_ok=True)
                execute(args, 'output_is_directory', [*base, '--output', blocked, '--seconds', '1'],
                        environment=env, success=False, message='arquivo de saida ja existe')
                require(not blocked.with_suffix('.recording.mkv').exists(), 'Destino invalido criou MKV.')
                passed('existing_destination_directory_rejected_before_recording')
            with SourceWindow(args.fixture, static=True) as source:
                path = args.artifacts / 'static.mp4'
                diag = execute(args, 'static', ['--hwnd', str(source.hwnd), '--crop', CROP, *tools,
                    '--output', path, '--seconds', '1'], environment=env)
                report['static_video'] = verify_video(args, path, int(diag['frames']), 1, moving=False)
                passed('static_source_repeats_frames_and_preserves_duration')
            for kind in ('source_closed', 'source_resized'):
                source_failure(args, env, kind)
                passed(f'{kind}_stops_with_error_and_preserves_mkv')
        report['process_tree_cleanup'] = 'Windows jobs closed for every recorder process'
        report['status'] = 'passed'
        print(f'E2E Fase 3 aprovado: {len(report["cases"])} grupos.', flush=True)
        return 0
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        print(f'E2E Fase 3 reprovado: {error}', file=sys.stderr, flush=True)
        return 1
    finally:
        (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')


if __name__ == '__main__':
    sys.exit(main())
