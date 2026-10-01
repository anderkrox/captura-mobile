"""Drive Phase 4 Win32 controls, global hotkeys, WGC and real media exports."""

from __future__ import annotations

import argparse
import configparser
import csv
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

from e2e_phase0 import require, run
from e2e_phase1 import CALLBACK, SourceWindow, user32
from e2e_phase2 import Application, APPLY, SAVE, SOURCE, send, set_text, text, wait_for
from e2e_phase3 import ExtendedLimits, kernel32, verify_video

STATE, DURATION, FOLDER, START, PAUSE, RESUME, STOP, CHOOSE, OPEN_FOLDER, RECOVER = range(118, 128)
HOTKEY_EDITS = (128, 129, 130)
APPLY_HOTKEYS = 131
HOTKEYS = ('Ctrl+Alt+F18', 'Ctrl+Alt+F19', 'Ctrl+Alt+F20')
user32.RegisterHotKey.argtypes = (wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT)
user32.UnregisterHotKey.argtypes = (wintypes.HWND, ctypes.c_int)
user32.keybd_event.argtypes = (wintypes.BYTE, wintypes.BYTE, wintypes.DWORD, ctypes.c_size_t)


def preferences(local, output, hotkeys=HOTKEYS):
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    config['application'] = dict(zip(('outputFolder', 'hotkeyStart', 'hotkeyPauseResume', 'hotkeyStop'),
                                    (str(output), *hotkeys)))
    path = local / 'YourotsCapture' / 'preferences.ini'
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w', encoding='utf-16') as stream:
        config.write(stream, space_around_delimiters=False)
    return path


def global_hotkey(number):
    # Deliver real keyboard events so Windows performs RegisterHotKey dispatch.
    keys = (0x11, 0x12, 0x70 + number - 1)  # Ctrl, Alt, Fn
    try:
        for key in keys:
            user32.keybd_event(key, 0, 0, 0)
        time.sleep(0.05)
    finally:
        for key in reversed(keys):
            user32.keybd_event(key, 0, 2, 0)


class RecordingApplication(Application):
    def __enter__(self):
        self.job = kernel32.CreateJobObjectW(None, None)
        require(self.job, 'CreateJobObjectW falhou.')
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000
        require(kernel32.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)),
                'SetInformationJobObject falhou.')
        try:
            super().__enter__()
            require(kernel32.AssignProcessToJobObject(self.job, wintypes.HANDLE(self.process._handle)),
                    'AssignProcessToJobObject falhou.')
            require(user32.SetWindowPos(self.hwnd, None, 650, 80, 1400, 920, 0x0010), 'Resize UI falhou.')
            return self
        except BaseException:
            self.__exit__(Exception)
            raise

    def __exit__(self, kind, *unused):
        try:
            if self.process and self.process.poll() is None and kind is None:
                self.close(6)
            if self.process and kind is None:
                require(self.process.returncode == 0, 'Aplicativo nao encerrou normalmente.')
            if self.process and kind is not None:
                print(f'UI failure: {unused}; state={self.state()} status={self.status()}', file=sys.stderr)
        finally:
            if getattr(self, 'job', None):
                kernel32.CloseHandle(self.job)
                self.job = None
            if self.process and self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=5)
            if self.process:
                self.process.wait(timeout=5)
            # Windows may retain image sections briefly after Job termination.
            time.sleep(0.2)

    def state(self):
        return text(self.control(STATE)).removeprefix('Estado: ')

    def wait_state(self, value):
        wait_for(lambda: self.state() == value, f'Estado esperado {value}; atual {self.state()}.', timeout=15)

    def enabled(self, identifier):
        return bool(user32.IsWindowEnabled(self.control(identifier)))

    def command(self, identifier):
        require(user32.PostMessageW(self.hwnd, 0x0111, identifier, self.control(identifier)), 'WM_COMMAND falhou.')

    def dialog(self):
        found = []
        @CALLBACK
        def collect(hwnd, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            name = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(hwnd, name, len(name))
            if pid.value == self.process.pid and name.value == '#32770':
                found.append(hwnd)
            return True
        require(user32.EnumWindows(collect, 0), 'EnumWindows falhou.')
        return found[0] if found else None

    def close(self, answer):
        require(user32.PostMessageW(self.hwnd, 0x0010, 0, 0), 'WM_CLOSE falhou.')
        wait_for(lambda: self.dialog() or self.process.poll() is not None, 'Fechamento sem resposta.')
        dialog = self.dialog()
        if dialog:
            require(user32.PostMessageW(dialog, 0x0111, answer, 0), 'Resposta ao dialogo falhou.')
        if answer == 6:
            self.process.wait(timeout=15)
            require(self.process.returncode == 0, 'Falha ao finalizar no fechamento.')
        else:
            wait_for(lambda: self.dialog() is None, 'Dialogo nao fechou.')
            require(self.process.poll() is None, 'Recusa do fechamento foi ignorada.')

    def prepare(self, title):
        self.select(title)
        self.apply((57, 50, 486, 864))
        self.wait_valid()
        require(self.enabled(START), 'Iniciar nao habilitou com calibracao valida.')


def video(args, path):
    metadata = json.loads(run([str(args.ffprobe), '-v', 'error', '-count_frames', '-show_streams',
                              '-show_format', '-of', 'json', str(path)]))
    frames = int(metadata['streams'][0]['nb_read_frames'])
    return verify_video(args, path, frames, frames / 30 - 0.000001, moving=False)


def newly_created(folder, before, suffix='*.mp4'):
    paths = set(folder.glob(suffix)) - before
    require(len(paths) == 1, f'Esperado um novo arquivo {suffix}: {paths}.')
    return paths.pop()


def main():
    parser = argparse.ArgumentParser()
    for name in ('app', 'tool-fixture', 'fixture', 'ffmpeg', 'ffprobe', 'artifacts'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'scope': 'phase4_real_ui_global_hotkeys_wgc_and_media', 'cases': []}

    def passed(name):
        report['cases'].append(name)
        print(f'PASS {name}', flush=True)

    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        for name in ('app', 'tool_fixture', 'fixture', 'ffmpeg', 'ffprobe'):
            require(getattr(args, name).is_file(), f'Dependencia ausente: {name}.')
        with tempfile.TemporaryDirectory(prefix='yourots-phase4-') as directory:
            temp = Path(directory)
            # Package a copy of production UI with test media proxies; no production test hooks.
            app_path = temp / 'package' / 'YourotsCapture.exe'
            tools = app_path.parent / 'third_party' / 'ffmpeg' / 'bin'
            tools.mkdir(parents=True)
            shutil.copy2(args.app, app_path)
            shutil.copy2(args.tool_fixture, tools / 'ffmpeg.exe')
            shutil.copy2(args.tool_fixture, tools / 'ffprobe.exe')
            mode_file = temp / 'mode.txt'
            mode_file.write_text('cpu', encoding='ascii')
            environment = {**os.environ, 'YOUROTS_TEST_TOOL_MODE_FILE': str(mode_file),
                           'YOUROTS_TEST_FFMPEG': str(args.ffmpeg), 'YOUROTS_TEST_FFPROBE': str(args.ffprobe)}
            title = 'Yourots E2E — fase 4 🎮'
            with SourceWindow(args.fixture, static=True) as source:
                set_text(source.hwnd, title)
                local = temp / 'buttons'
                output = args.artifacts / 'vídeos com ação 🎮'
                output.mkdir(exist_ok=True)
                initial_exports = set(output.glob('*.mp4'))
                pref = preferences(local, output)
                with RecordingApplication(app_path, local, environment=environment) as app:
                    app.wait_state('Pronto')
                    require(not app.enabled(START) and not app.enabled(PAUSE) and not app.enabled(RECOVER),
                            'Controles iniciais incorretos.')
                    require(str(output) in text(app.control(FOLDER)), 'Destino Unicode nao exibido.')
                    initial_temporaries = set(output.glob('*.recording.mkv'))
                    app.command(START)
                    time.sleep(0.1)
                    app.wait_state('Pronto')
                    require(set(output.glob('*.recording.mkv')) == initial_temporaries, 'Inicio sem calibracao criou arquivo.')
                    passed('ready_destination_and_start_requires_calibration')
                    app.prepare(title)
                    app.check_preview(args.artifacts / 'preview-phase4.bmp')
                    before = set(output.glob('*.mp4'))
                    wall_start = time.monotonic()
                    app.command(START); app.wait_state('Gravando')
                    require(app.enabled(PAUSE) and app.enabled(STOP) and not app.enabled(START), 'Botoes gravando incorretos.')
                    for control in (SOURCE, APPLY, SAVE, CHOOSE):
                        require(not app.enabled(control), f'Controle {control} nao bloqueado.')
                    app.command(START)  # Duplicate starts must keep the same session.
                    time.sleep(1.1)
                    for _ in range(3):
                        app.command(PAUSE); app.wait_state('Pausado')
                        time.sleep(0.1)
                        frozen = text(app.control(DURATION))
                        time.sleep(0.6)
                        require(text(app.control(DURATION)) == frozen, 'Contador avancou durante pausa.')
                        require(app.enabled(RESUME) and app.enabled(STOP) and not app.enabled(PAUSE), 'Botoes pausado incorretos.')
                        app.command(RESUME); app.wait_state('Gravando')
                        time.sleep(0.35)
                    app.command(STOP); app.wait_state('Pronto')
                    wall = time.monotonic() - wall_start
                    path = newly_created(output, before)
                    verified = video(args, path)
                    require(1.9 <= verified['duration_seconds'] <= 3.0, f'Duracao ativa incorreta: {verified}.')
                    require(wall - verified['duration_seconds'] >= 1.8, 'Intervalos pausados presentes no MP4.')
                    require(not path.with_suffix('.recording.mkv').exists(), 'MKV nao removido apos validacao.')
                    report['pause_video'] = {**verified, 'wall_seconds': wall, 'file': str(path.relative_to(args.artifacts))}
                    passed('buttons_repeated_pause_frozen_counter_and_mp4_excludes_pauses')
                    # Parser/duplicate rejection must preserve saved preferences.
                    saved = pref.read_bytes()
                    set_text(app.control(HOTKEY_EDITS[0]), 'Ctrl++F18')
                    app.click(APPLY_HOTKEYS)
                    require('invalido' in app.status(), 'Atalho invalido aceito.')
                    require(pref.read_bytes() == saved, 'Atalho invalido alterou preferencias.')
                    set_text(app.control(HOTKEY_EDITS[0]), HOTKEYS[1])
                    app.click(APPLY_HOTKEYS)
                    require('Conflito' in app.status(), 'Atalhos duplicados aceitos.')
                    passed('invalid_and_duplicate_hotkeys_preserve_preferences')
                    require(user32.RegisterHotKey(None, 0x1234, 0x0003, 0x70 + 22), 'Reserva do atalho F23 falhou.')
                    try:
                        set_text(app.control(HOTKEY_EDITS[0]), 'Ctrl+Alt+F23')
                        app.click(APPLY_HOTKEYS)
                        require('em uso' in app.status(), 'Conflito externo nao detectado.')
                    finally:
                        user32.UnregisterHotKey(None, 0x1234)
                    require(pref.read_bytes() == saved, 'Registro recusado alterou preferencias.')
                    before = set(output.glob('*.mp4'))
                    global_hotkey(18); app.wait_state('Gravando')
                    time.sleep(0.7)
                    global_hotkey(19); app.wait_state('Pausado')
                    global_hotkey(19); app.wait_state('Gravando')
                    time.sleep(0.5)
                    global_hotkey(20); app.wait_state('Pronto')
                    video(args, newly_created(output, before))
                    passed('external_hotkey_conflict_rolls_back_and_real_global_keys_control_recording')
                    for identifier, value in zip(HOTKEY_EDITS, (' alt + ctrl + f18 ', 'Alt+Ctrl+F19', 'Ctrl+Alt+F20')):
                        set_text(app.control(identifier), value)
                    app.click(APPLY_HOTKEYS)
                    require(tuple(text(app.control(i)) for i in HOTKEY_EDITS) == HOTKEYS, 'Atalhos nao canonicalizados.')
                    passed('hotkeys_canonicalized_and_saved')
                    before = set(output.glob('*.mp4'))
                    app.command(START); app.wait_state('Gravando'); time.sleep(0.7)
                    user32.ShowWindow(source.hwnd, 6)
                    app.wait_state('Pausado')
                    require(not app.enabled(RESUME), 'Retomar habilitado com fonte minimizada.')
                    time.sleep(0.2); frozen = text(app.control(DURATION)); time.sleep(1.0)
                    require(text(app.control(DURATION)) == frozen, 'Minimizacao nao congelou tempo.')
                    user32.ShowWindow(source.hwnd, 9)
                    wait_for(lambda: app.enabled(RESUME), 'Retomar nao habilitou apos restaurar.')
                    require(app.state() == 'Pausado', 'Restauracao retomou sem acao do usuario.')
                    app.click(RESUME)
                    app.wait_state('Gravando'); time.sleep(0.5)
                    app.command(STOP); app.wait_state('Pronto')
                    require(video(args, newly_created(output, before))['duration_seconds'] < 1.8, 'Minimizacao entrou no video.')
                    passed('minimize_auto_pause_restore_requires_explicit_resume')
                    before = set(output.glob('*.mp4'))
                    app.command(START); app.wait_state('Gravando'); time.sleep(0.7)
                    require(user32.SetWindowPos(source.hwnd, None, 0, 0, 640, 1000, 0x0016), 'Resize falhou.')
                    app.wait_state('Pausado')
                    require(not app.enabled(RESUME), 'Retomar habilitado apos invalidar calibracao.')
                    app.command(RESUME); time.sleep(0.1); app.wait_state('Pausado')
                    app.command(STOP); app.wait_state('Pronto')
                    video(args, newly_created(output, before))
                    require(not app.enabled(START), 'Nova sessao habilitada sem recalibrar.')
                    require(user32.SetWindowPos(source.hwnd, None, 0, 0, 600, 964, 0x0016), 'Restaurar tamanho falhou.')
                    time.sleep(0.2); app.apply((57, 50, 486, 864)); app.wait_valid()
                    passed('resize_auto_pause_blocks_resume_and_requires_recalibration')
                    before = set(output.glob('*.mp4'))
                    app.command(START); app.wait_state('Gravando'); time.sleep(0.7)
                    app.close(7)
                    app.wait_state('Gravando')
                    require(set(output.glob('*.mp4')) == before, 'Recusa finalizou gravacao.')
                    app.command(PAUSE); app.wait_state('Pausado')
                    app.close(6)
                    video(args, newly_created(output, before))
                    passed('close_active_no_keeps_session_yes_finalizes_paused_session')
                # Restart confirms persistence and that all global registrations were released.
                with RecordingApplication(app_path, local, environment=environment) as app:
                    require(tuple(text(app.control(i)) for i in HOTKEY_EDITS) == HOTKEYS, 'Atalhos nao restaurados.')
                    require('conflito' not in app.status().lower(), 'Atalhos nao liberados ao sair.')
                    require(str(output) in text(app.control(FOLDER)), 'Destino nao restaurado.')
                    passed('restart_restores_preferences_and_releases_hotkeys')
                # Existing MP4 and temporary MKV names are preserved by production naming.
                hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.glob('*.mp4')}
                require(len(set(hashes) - initial_exports) == 5, 'As cinco sessoes devem criar cinco arquivos distintos.')
                passed('successive_sessions_have_distinct_output_names')
                folder = args.artifacts / 'late-collision'; folder.mkdir(exist_ok=True)
                local = temp / 'late-collision'; preferences(local, folder)
                before_mkv = set(folder.glob('*.recording.mkv'))
                with RecordingApplication(app_path, local, environment=environment) as app:
                    app.prepare(title); app.command(START); app.wait_state('Gravando'); time.sleep(0.8)
                    mkv = newly_created(folder, before_mkv, '*.recording.mkv')
                    collision = mkv.with_name(mkv.name.removesuffix('.recording.mkv') + '.mp4')
                    collision.write_bytes(b'preserve existing file')
                    app.command(STOP); app.wait_state('Erro')
                    require(collision.read_bytes() == b'preserve existing file' and mkv.stat().st_size > 0,
                            'Colisao apos inicio sobrescreveu arquivo ou perdeu MKV.')
                    before = set(folder.glob('*.mp4'))
                    app.command(RECOVER); app.wait_state('Pronto')
                    video(args, newly_created(folder, before))
                    require(collision.read_bytes() == b'preserve existing file', 'Recuperacao sobrescreveu colisao.')
                passed('destination_created_during_recording_is_preserved_and_mkv_recovers')
                # Finalization faults retain real encoded MKV; recovery must also be validated.
                for fault in ('remux_fail', 'probe_fail', 'unexpected_audio', 'probe_nan', 'encoder_abort'):
                    mode_file.write_text('cpu', encoding='ascii')
                    local = temp / fault
                    folder = args.artifacts / fault; folder.mkdir(exist_ok=True)
                    preferences(local, folder)
                    before_mkv = set(folder.glob('*.recording.mkv'))
                    before_mp4 = set(folder.glob('*.mp4'))
                    with RecordingApplication(app_path, local, environment=environment) as app:
                        app.prepare(title)
                        app.command(START); app.wait_state('Gravando'); time.sleep(1.2)
                        mode_file.write_text(fault, encoding='ascii')
                        if fault != 'encoder_abort': app.command(STOP)
                        app.wait_state('Erro')
                        time.sleep(0.15)
                        require('Falha ao finalizar' in app.status(), 'Previa apagou o diagnostico de falha.')
                        require(app.enabled(RECOVER) and not app.enabled(STOP), f'{fault}: recuperacao indisponivel.')
                        mkv = newly_created(folder, before_mkv, '*.recording.mkv')
                        if fault != 'encoder_abort': require(mkv.stat().st_size > 0, f'{fault}: MKV vazio.')
                        digest = hashlib.sha256(mkv.read_bytes()).hexdigest()
                        if fault == 'encoder_abort':
                            report['encoder_abort_temporary_bytes'] = mkv.stat().st_size
                            if mkv.stat().st_size == 0:
                                app.command(RECOVER); app.wait_state('Erro')
                                require(mkv.exists() and 'vazia' in app.status(), 'MKV vazio nao foi rejeitado com diagnostico.')
                            app.close(7)
                            require(mkv.is_file(), 'Aviso de saida apagou temporario.')
                            app.close(6)
                        else:
                            app.command(RECOVER); app.wait_state('Erro')
                            require(mkv.exists() and hashlib.sha256(mkv.read_bytes()).hexdigest() == digest,
                                    'Recuperacao falha alterou MKV.')
                            mode_file.write_text('cpu', encoding='ascii')
                            partial = set(folder.glob('*.mp4'))
                            app.command(RECOVER); app.wait_state('Pronto')
                            recovered = newly_created(folder, partial)
                            video(args, recovered)
                            require(not mkv.exists() and not app.enabled(RECOVER), 'Recuperacao valida nao limpou temporario.')
                    passed(f'{fault}_preserves_mkv_and_' + ('warns_on_close' if fault == 'encoder_abort' else 'validated_recovery'))
                # A real invalid destination rejects start before any encoder process is launched.
                mode_file.write_text('cpu', encoding='ascii')
                blocker = temp / 'destination-is-file'; blocker.write_text('preserve', encoding='ascii')
                local = temp / 'blocked'; preferences(local, blocker)
                with RecordingApplication(app_path, local, environment=environment) as app:
                    app.prepare(title); app.command(START); app.wait_state('Erro')
                    require(blocker.read_text() == 'preserve' and not app.enabled(RECOVER), 'Destino invalido foi alterado.')
                passed('invalid_destination_reports_error_and_preserves_existing_file')
                denied = temp / 'write-denied'; denied.mkdir()
                account = subprocess.run(['whoami', '/user', '/fo', 'csv', '/nh'], capture_output=True,
                                         text=True, check=True, timeout=5)
                sid = next(csv.reader(account.stdout.splitlines()))[1]
                subprocess.run(['icacls', str(denied), '/deny', f'*{sid}:(W)'], capture_output=True,
                               check=True, timeout=5)
                try:
                    local = temp / 'no-permission'; preferences(local, denied)
                    with RecordingApplication(app_path, local, environment=environment) as app:
                        app.prepare(title); app.command(START); app.wait_state('Erro')
                        require(not app.enabled(RECOVER) and 'Sem permissao' in app.status(),
                                'Negacao de escrita nao foi diagnosticada.')
                finally:
                    subprocess.run(['icacls', str(denied), '/remove:d', f'*{sid}'], capture_output=True,
                                   check=True, timeout=5)
                passed('real_write_permission_denial_rejected_before_encoding')
                require(not list(denied.iterdir()), 'Pasta sem permissao criou gravacao.')
                for path, digest in hashes.items():
                    require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, 'Gravacao anterior foi sobrescrita.')
                passed('prior_recordings_unchanged_after_faults_and_recovery')
            # Closing the source while paused must be an error, then recoverable from flushed MKV.
            with SourceWindow(args.fixture, static=True) as source:
                set_text(source.hwnd, title)
                mode_file.write_text('cpu', encoding='ascii')
                local = temp / 'source-closed'; folder = args.artifacts / 'source-closed'; folder.mkdir(exist_ok=True)
                preferences(local, folder)
                before = set(folder.glob('*.mp4'))
                with RecordingApplication(app_path, local, environment=environment) as app:
                    app.prepare(title); app.command(START); app.wait_state('Gravando'); time.sleep(0.8)
                    app.command(PAUSE); app.wait_state('Pausado')
                    require(user32.PostMessageW(source.hwnd, 0x0010, 0, 0), 'Fechar fonte falhou.')
                    source.process.wait(timeout=5); app.wait_state('Erro')
                    require(set(folder.glob('*.mp4')) == before, 'Fonte fechada foi apresentada como sucesso.')
                    require(app.enabled(RECOVER) and not app.enabled(START), 'Controles apos fechamento incorretos.')
                    app.command(RECOVER); app.wait_state('Pronto')
                    video(args, newly_created(folder, before))
                passed('source_closed_while_paused_is_error_with_validated_recovery')
            report['process_tree_cleanup'] = 'All application jobs closed, including FFmpeg descendants'
            report['disk_space_coverage'] = '64/32 MiB boundary unit tests; no disk filling'
            report['encoder'] = 'libx264'
            report['status'] = 'passed'
    except BaseException as error:
        report['status'] = 'failed'; report['error'] = f'{type(error).__name__}: {error}'
        traceback.print_exc()
        print(report['error'], file=sys.stderr)
    report['case_count'] = len(report['cases'])
    (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
