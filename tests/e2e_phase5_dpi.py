"""Opt-in physical DPI matrix; restore the original first-monitor scale in finally."""

from __future__ import annotations

import argparse
import ctypes
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
import traceback

from e2e_phase0 import require
from e2e_phase1 import SourceWindow, user32
from e2e_phase2 import set_text
from e2e_phase4 import RecordingApplication, START, STOP, RESUME, preferences, newly_created, video
from e2e_phase5 import monitors


def main():
    parser = argparse.ArgumentParser()
    for name in ('app', 'fixture', 'ffmpeg', 'ffprobe', 'scale-helper', 'artifacts'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    args = parser.parse_args()
    for name in vars(args): setattr(args, name, getattr(args, name).resolve())
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'scope': 'physical_windows_settings_dpi_with_real_wgc_ui_and_video', 'scales': []}
    original = None

    def scale(value=None):
        result = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            str(args.scale_helper), *(['-ReadOnly'] if value is None else ['-Percent', str(value)])],
            capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        require(result.returncode == 0, result.stderr.decode('utf-8', errors='replace'))
        return json.loads(result.stdout.decode('utf-8-sig'))

    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        initial = scale()
        original = int(re.match(r'\d+', initial['selected'])[0])
        require(original in (100, 125, 150), 'Original scale cannot be restored by this matrix.')
        displays = monitors()
        require(len(displays) >= 2 and displays[0]['left'] == 0, 'Expected the observed two-monitor arrangement.')
        with tempfile.TemporaryDirectory(prefix='yourots-phase5-dpi-') as folder:
            local = Path(folder)
            output = args.artifacts / 'videos'
            output.mkdir(exist_ok=True)
            preferences(local, output)
            for percent in (100, 125, 150):
                selection = scale(percent)
                with SourceWindow(args.fixture, static=True) as source:
                    title = f'Yourots physical DPI {percent}'
                    set_text(source.hwnd, title)
                    time.sleep(.3)
                    dpi = user32.GetDpiForWindow(source.hwnd)
                    require(dpi == percent * 96 // 100, f'Physical source DPI {dpi}, expected {percent}%.')
                    with RecordingApplication(args.app, local) as app:
                        app.prepare(title)
                        app.check_preview(args.artifacts / f'preview-{percent}.bmp')
                        before = set(output.glob('*.mp4'))
                        app.command(START)
                        app.wait_state('Gravando')
                        time.sleep(1.1)
                        other = displays[1]
                        require(user32.SetWindowPos(source.hwnd, None, other['left'] + 20,
                            other['top'] + 20, 0, 0, 0x0015), 'Cross-DPI move failed.')
                        time.sleep(.7)
                        changed_dpi = user32.GetDpiForWindow(source.hwnd)
                        if changed_dpi != dpi:
                            app.wait_state('Pausado')
                            require(not app.enabled(RESUME), 'DPI change allowed resume without calibration.')
                        else:
                            require(app.state() == 'Gravando', 'Same-DPI move paused recording.')
                            app.check_preview(args.artifacts / 'preview-100-other-monitor.bmp')
                        require(user32.SetWindowPos(source.hwnd, None, 20, 20, 0, 0, 0x0015), 'Return move failed.')
                        time.sleep(.5)
                        if changed_dpi != dpi:
                            require(not app.enabled(RESUME), 'Returning to original DPI cleared required calibration.')
                        app.command(STOP)
                        app.wait_state('Pronto')
                        exported = video(args, newly_created(output, before))
                        app.apply((57, 50, 486, 864))
                        app.wait_valid()
                        app.check_preview(args.artifacts / f'preview-{percent}-recalibrated.bmp')
                        report['scales'].append({'percent': percent, 'source_dpi': dpi,
                            'other_monitor_dpi': changed_dpi, 'settings': selection, 'video': exported,
                            'recalibration_required_on_dpi_change': changed_dpi != dpi})
                        print(f'PASS physical scale {percent}%: source DPI {dpi}, video corners and recalibration', flush=True)
        report['status'] = 'passed'
        report['limits'] = ['DPI and monitor movement use a controlled Win32 source.',
                           'At 125/150%, a 486x864 CSS browser viewport at 100% preview cannot fit a 1080px-high monitor.']
        return 0
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
        traceback.print_exc()
        return 1
    finally:
        restore_failure = None
        if original in (100, 125, 150):
            try:
                report['restored_scale'] = scale(original)
            except Exception as error:
                report.update(status='failed', restore_error=str(error))
                restore_failure = error
        (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        if restore_failure: raise restore_failure


if __name__ == '__main__':
    raise SystemExit(main())
