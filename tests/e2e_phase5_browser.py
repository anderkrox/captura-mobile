"""Opt-in lifecycle tests on an existing game window and an owned blank Edge window."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import tempfile
import time
import traceback

from e2e_phase0 import require, run, mp4_atoms
from e2e_phase1 import CALLBACK, user32
from e2e_phase2 import REFRESH, wait_for
from e2e_phase4 import RecordingApplication, START, PAUSE, RESUME, STOP, RECOVER, preferences, newly_created
from phase5_validation import check_media, corner_errors


class Placement(ctypes.Structure):
    _fields_ = [('length', wintypes.UINT), ('flags', wintypes.UINT), ('show', wintypes.UINT),
                ('minimum', wintypes.POINT), ('maximum', wintypes.POINT), ('normal', wintypes.RECT)]


user32.GetWindowPlacement.argtypes = (wintypes.HWND, ctypes.POINTER(Placement))
user32.SetWindowPlacement.argtypes = (wintypes.HWND, ctypes.POINTER(Placement))
user32.GetWindowRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))


def restore_placement(hwnd, placement):
    # SetWindowPos can resize a maximized HWND without clearing WS_MAXIMIZE.
    # SetWindowPlacement alone then sees it as already maximized and keeps that size.
    user32.ShowWindow(hwnd, 9)
    return bool(user32.SetWindowPlacement(hwnd, ctypes.byref(placement)))


def browser_windows():
    result = {}
    @CALLBACK
    def collect(hwnd, unused):
        name = ctypes.create_unicode_buffer(128)
        user32.GetClassNameW(hwnd, name, len(name))
        if name.value == 'Chrome_WidgetWin_1' and user32.IsWindowVisible(hwnd):
            title = ctypes.create_unicode_buffer(2048)
            user32.GetWindowTextW(hwnd, title, len(title))
            result[hwnd] = title.value
        return True
    require(user32.EnumWindows(collect, 0), 'Window enumeration failed.')
    return result


def inspect(args, path, reference=None):
    data = json.loads(run([str(args.ffprobe), '-v', 'error', '-count_frames', '-show_streams',
                           '-show_format', '-of', 'json', str(path)]))
    result = check_media(data, int(data['streams'][0]['nb_read_frames']))
    run([str(args.ffmpeg), '-v', 'error', '-xerror', '-i', str(path), '-f', 'null', '-'])
    atoms = mp4_atoms(path)
    require(atoms.index('moov') < atoms.index('mdat'), 'MP4 missing faststart.')
    result.update(full_decode=True, faststart=True)
    if reference:
        ref = run([str(args.ffmpeg), '-v', 'error', '-i', str(reference), '-frames:v', '1',
                   '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
        frames = result['frames']
        select = '+'.join(f'eq(n\\,{index})' for index in (0, frames // 2, frames - 1))
        actual = run([str(args.ffmpeg), '-v', 'error', '-i', str(path), '-vf',
                      f'select={select},scale=486:864', '-fps_mode', 'passthrough',
                      '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
        size = 486 * 864 * 3
        require(len(actual) == size * 3, 'Browser reference frames are missing.')
        errors = [corner_errors(ref, actual[index * size:(index + 1) * size]) for index in range(3)]
        require(max(max(row) for row in errors) <= 15, f'Browser video differs at the four corners: {errors}.')
        result['corner_mean_absolute_errors_first_middle_last'] = errors
    path.with_suffix('.ffprobe.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser()
    for name in ('app', 'edge', 'ffmpeg', 'ffprobe', 'reference', 'artifacts'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    parser.add_argument('--hwnd', type=int, required=True)
    parser.add_argument('--crop', required=True)
    args = parser.parse_args()
    for name in ('app', 'edge', 'ffmpeg', 'ffprobe', 'reference', 'artifacts'):
        setattr(args, name, getattr(args, name).resolve())
    args.artifacts.mkdir(parents=True, exist_ok=True)
    placement = Placement()
    placement.length = ctypes.sizeof(placement)
    require(user32.GetWindowPlacement(args.hwnd, ctypes.byref(placement)), 'Source placement query failed.')
    report = {'status': 'running', 'scope': 'real_edge_game_ui_lifecycle_and_owned_browser_recovery', 'cases': []}
    owned = None
    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        with tempfile.TemporaryDirectory(prefix='yourots-phase5-browser-') as folder:
            local = Path(folder)
            output = args.artifacts / 'videos'
            output.mkdir(exist_ok=True)
            preferences(local, output)
            with RecordingApplication(args.app, local) as app:
                app.select(browser_windows()[args.hwnd])
                crop = tuple(map(int, args.crop.split(',')))
                app.apply(crop)
                app.wait_valid()
                before = set(output.glob('*.mp4'))
                start = time.monotonic()
                app.command(START)
                app.wait_state('Gravando')
                time.sleep(1)
                for _ in range(3):
                    app.command(PAUSE)
                    app.wait_state('Pausado')
                    time.sleep(.3)
                    app.command(RESUME)
                    app.wait_state('Gravando')
                    time.sleep(.3)
                rect = wintypes.RECT()
                require(user32.GetWindowRect(args.hwnd, ctypes.byref(rect)), 'Source rectangle query failed.')
                require(user32.SetWindowPos(args.hwnd, None, rect.left - 1920, rect.top, 0, 0, 0x0015),
                        'Moving Edge to the first monitor failed.')
                time.sleep(.6)
                require(app.state() == 'Gravando' and app.crop() == crop, 'Moving Edge changed recording/calibration.')
                require(user32.SetWindowPos(args.hwnd, None, rect.left, rect.top, 0, 0, 0x0015),
                        'Returning Edge to the second monitor failed.')
                time.sleep(.4)
                user32.ShowWindow(args.hwnd, 6)
                app.wait_state('Pausado')
                time.sleep(.4)
                require(restore_placement(args.hwnd, placement),
                        'Restoring original browser placement after minimization failed.')
                wait_for(lambda: app.enabled(RESUME), 'Restored browser did not allow resume.', timeout=10)
                app.command(RESUME)
                app.wait_state('Gravando')
                time.sleep(.4)
                app.command(STOP)
                app.wait_state('Pronto')
                verified = inspect(args, newly_created(output, before), args.reference)
                require(time.monotonic() - start - verified['duration_seconds'] >= 1.2, 'Pauses included in game MP4.')
                report['game_video'] = verified
                report['cases'].extend(['game_repeated_pause_resume_excludes_pauses',
                    'game_edge_moved_across_both_monitors_preserves_crop', 'game_edge_minimize_restore_requires_resume'])
                # Resize the real game browser, then restore its exact original placement.
                app.command(START)
                app.wait_state('Gravando')
                time.sleep(.6)
                require(user32.SetWindowPos(args.hwnd, None, 0, 0, 1700, 950, 0x0016), 'Browser resize failed.')
                app.wait_state('Pausado')
                require(not app.enabled(RESUME), 'Browser resize allowed unsafe resume.')
                require(restore_placement(args.hwnd, placement), 'Browser placement restore failed.')
                time.sleep(.6)
                require(not app.enabled(RESUME), 'Restoring browser size silently cleared recalibration.')
                app.command(STOP)
                app.wait_state('Pronto')
                app.select(browser_windows()[args.hwnd])
                app.apply(crop)
                app.wait_valid()
                report['cases'].append('game_edge_resize_requires_explicit_recalibration')

                prior = browser_windows()
                startup = subprocess.STARTUPINFO()
                startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = 0
                subprocess.Popen([str(args.edge), '--new-window', 'about:blank'], startupinfo=startup)
                created = {}
                def find_owned():
                    nonlocal created
                    created = {hwnd: title for hwnd, title in browser_windows().items() if hwnd not in prior}
                    return len(created) == 1
                wait_for(find_owned, 'Dedicated Edge window was not created.', timeout=15)
                owned, title = next(iter(created.items()))
                require(user32.SetWindowPos(owned, None, 20, 20, 1100, 1040, 0x0010), 'Owned browser resize failed.')
                time.sleep(.5)
                title = browser_windows()[owned]
                app.click(REFRESH)
                app.select(title)
                app.apply((100, 147, 486, 864))
                app.wait_valid()
                before = set(output.glob('*.mp4'))
                app.command(START)
                app.wait_state('Gravando')
                time.sleep(2)
                require(user32.PostMessageW(owned, 0x0010, 0, 0), 'Owned browser close failed.')
                wait_for(lambda: not user32.IsWindow(owned), 'Owned browser did not close.')
                owned = None
                app.wait_state('Erro')
                require(app.enabled(RECOVER), 'Browser close did not preserve a recovery candidate.')
                require(set(output.glob('*.mp4')) == before, 'Closed source falsely reported successful export.')
                temporaries = set(output.glob('*.recording.mkv'))
                require(any(path.stat().st_size > 0 for path in temporaries), 'Browser close lost its temporary recording.')
                app.command(RECOVER)
                app.wait_state('Pronto')
                report['recovered_video'] = inspect(args, newly_created(output, before))
                report['cases'].extend(['owned_edge_close_reports_error_and_preserves_mkv',
                                         'owned_edge_interrupted_recording_recovers_with_real_ffmpeg'])
        report['status'] = 'passed'
        print('PASS real game browser pause/move/minimize/resize and owned Edge close/recovery', flush=True)
        return 0
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
        traceback.print_exc()
        return 1
    finally:
        if owned and user32.IsWindow(owned): user32.PostMessageW(owned, 0x0010, 0, 0)
        report['source_placement_restored'] = restore_placement(args.hwnd, placement)
        (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    raise SystemExit(main())
