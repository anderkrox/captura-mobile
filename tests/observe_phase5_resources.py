"""Observe an existing encoder without controlling its lifetime."""

import argparse
import ctypes
from ctypes import wintypes
import csv
import json
from pathlib import Path
import time
from types import SimpleNamespace

from e2e_phase0 import require
from phase5_resources import kernel32, resource_sample
from phase5_validation import memory_trend

kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--warmup-seconds', type=int, default=120)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'pid': args.pid, 'scope': 'encoder_steady_state_observation', 'samples': []}
    handle = kernel32.OpenProcess(0x410, False, args.pid)
    require(handle, 'Cannot query the encoder process.')
    start = time.monotonic()
    try:
        with (args.artifacts / 'resources.csv').open('w', newline='', encoding='utf-8') as stream:
            writer = None
            while True:
                code = wintypes.DWORD()
                require(kernel32.GetExitCodeProcess(handle, ctypes.byref(code)), 'Cannot query process status.')
                if code.value != 259:
                    report['exit_code'] = code.value
                    break
                sample = resource_sample(SimpleNamespace(_handle=handle), time.monotonic() - start)
                report['samples'].append(sample)
                if writer is None:
                    writer = csv.DictWriter(stream, fieldnames=list(sample))
                    writer.writeheader()
                writer.writerow(sample)
                stream.flush()
                (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
                time.sleep(5)
        report['memory'] = memory_trend(report['samples'], warmup_seconds=args.warmup_seconds)
        require(report['memory']['passed'], f'Encoder memory grew: {report["memory"]}.')
        report['status'] = 'passed'
        print(json.dumps(report['memory']), flush=True)
        return 0
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        kernel32.CloseHandle(handle)
        (args.artifacts / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    raise SystemExit(main())
