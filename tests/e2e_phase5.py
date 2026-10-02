"""Repeat real UI recording cycles and move a captured source across physical monitors."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import tempfile
import time
import traceback

from e2e_phase0 import require, run
from e2e_phase1 import SourceWindow, user32
from e2e_phase2 import set_text
from e2e_phase4 import RecordingApplication, START, STOP, PAUSE, RESUME, preferences, newly_created, video
from phase5_resources import resource_sample

MONITOR_CALLBACK = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE, wintypes.HDC,
                                     ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)
user32.EnumDisplayMonitors.argtypes = (wintypes.HDC, ctypes.POINTER(wintypes.RECT), MONITOR_CALLBACK,
                                     wintypes.LPARAM)


def monitors():
    result = []
    @MONITOR_CALLBACK
    def collect(handle, dc, rect, data):
        result.append({'left': rect.contents.left, 'top': rect.contents.top,
                       'right': rect.contents.right, 'bottom': rect.contents.bottom})
        return True
    require(user32.EnumDisplayMonitors(None, None, collect, 0), 'Monitor enumeration failed.')
    return result


def main():
    parser = argparse.ArgumentParser()
    for name in ('app', 'fixture', 'ffmpeg', 'ffprobe', 'artifacts'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'scope': 'real_win32_ui_wgc_physical_monitors_and_cpu_stress',
              'cases': [], 'cycles': []}
    started = time.monotonic()
    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        report['monitors'] = displays = monitors()
        with tempfile.TemporaryDirectory(prefix='yourots-phase5-') as folder:
            local = Path(folder)
            output = args.artifacts / 'cycles'
            output.mkdir(exist_ok=True)
            preferences(local, output)
            for mode in ('static', 'motion', 'stress'):
                with SourceWindow(args.fixture, static=mode == 'static', stress=mode == 'stress') as source:
                    title = f'Yourots Phase 5 {mode}'
                    set_text(source.hwnd, title)
                    with RecordingApplication(args.app, local) as app:
                        app.prepare(title)
                        before_resources = resource_sample(app.process, 0)
                        for cycle in range(4):
                            before = set(output.glob('*.mp4'))
                            initial_crop = app.crop()
                            wall_start = time.monotonic()
                            app.command(START)
                            app.wait_state('Gravando')
                            time.sleep(.6)
                            if cycle == 0 and len(displays) >= 2:
                                dpi = user32.GetDpiForWindow(source.hwnd)
                                for display in displays + displays[:1]:
                                    require(user32.SetWindowPos(source.hwnd, None,
                                        display['left'] + 20, display['top'] + 20, 0, 0, 0x0015),
                                        'Moving source across monitors failed.')
                                    time.sleep(.4)
                                    if user32.GetDpiForWindow(source.hwnd) == dpi:
                                        require(app.state() == 'Gravando', 'Same-DPI monitor move paused recording.')
                                        require(app.crop() == initial_crop, 'Monitor move changed crop.')
                                        app.check_preview(args.artifacts / f'{mode}-monitor-{display["left"]}.bmp')
                                    else:
                                        app.wait_state('Pausado')
                                        require(not app.enabled(RESUME), 'DPI change allowed unsafe resume.')
                                        report['cases'].append(f'{mode}_cross_monitor_dpi_change_requires_recalibration')
                                        raise AssertionError('Run matching-DPI monitor cycles before the separate DPI matrix.')
                                report['cases'].append(f'{mode}_physical_monitor_movement_preserves_pixels_and_crop')
                            if cycle == 1:
                                for _ in range(3):
                                    app.command(PAUSE)
                                    app.wait_state('Pausado')
                                    time.sleep(.3)
                                    app.command(RESUME)
                                    app.wait_state('Gravando')
                                    time.sleep(.2)
                            time.sleep(.6)
                            app.command(STOP)
                            app.wait_state('Pronto')
                            path = newly_created(output, before)
                            checked = video(args, path, moving=None if mode == 'stress' else mode != 'static')
                            wall = time.monotonic() - wall_start
                            if cycle == 1:
                                require(wall - checked['duration_seconds'] >= .8, 'Paused intervals included in output.')
                            if mode == 'stress':
                                # The busy source must survive encoding, not just keep a moving marker.
                                raw = run([str(args.ffmpeg), '-v', 'error', '-i', str(path), '-vf',
                                    'fps=10,scale=54:96', '-frames:v', '8', '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
                                size = 54 * 96 * 3
                                require(len(raw) == size * 8, 'Stress frames missing.')
                                changes = [sum(abs(a - b) > 20 for a, b in zip(
                                    raw[index * size:(index + 1) * size],
                                    raw[(index + 1) * size:(index + 2) * size])) / size for index in range(7)]
                                require(max(changes) > .1, f'Intense animation froze: changed fractions {changes}.')
                                checked['changed_component_fractions'] = changes
                            after = resource_sample(app.process, time.monotonic() - started)
                            report['cycles'].append({'mode': mode, 'cycle': cycle + 1,
                                'media': checked, 'resources': after, 'file': str(path.relative_to(args.artifacts))})
                            print(f'PASS {mode} cycle {cycle + 1}: {checked["frames"]} frames', flush=True)
                        # Compare after several completed sessions, while the same capture/UI remain alive.
                        for name, bound in (('handles', 32), ('gdi_objects', 8), ('user_objects', 8)):
                            require(after[name] - before_resources[name] <= bound,
                                    f'{mode}: {name} increased across recording cycles.')
                        require(after['private_bytes'] - before_resources['private_bytes'] <= 32 * 1048576,
                                f'{mode}: private memory increased across recording cycles.')
                        report['cases'].extend([f'{mode}_four_recordings_release_resources',
                                                f'{mode}_repeated_pause_excluded_from_timeline'])
        require(len(report['cycles']) == 12, 'Some recording cycles were not exercised.')
        report['status'] = 'passed'
        report['physical_monitor_status'] = 'passed' if len(displays) >= 2 else 'unavailable_single_monitor'
        report['limits'] = ['Intense animations use a deterministic Win32 source, not a game combat scene.',
                           'Physical 125/150 percent scales use the separate DPI matrix; DevTools layout needs manual calibration.']
        return 0
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
        traceback.print_exc()
        return 1
    finally:
        report['wall_seconds'] = time.monotonic() - started
        (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    raise SystemExit(main())
