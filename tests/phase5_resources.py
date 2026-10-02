"""Read process resources during short recording cycles or passive observation."""

import ctypes
from ctypes import wintypes

from e2e_phase0 import require
from e2e_phase1 import user32
from e2e_phase3 import kernel32

psapi = ctypes.WinDLL('psapi', use_last_error=True)


class MemoryCounters(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('page_faults', wintypes.DWORD),
                ('peak_working', ctypes.c_size_t), ('working', ctypes.c_size_t),
                ('peak_paged_pool', ctypes.c_size_t), ('paged_pool', ctypes.c_size_t),
                ('peak_nonpaged_pool', ctypes.c_size_t), ('nonpaged_pool', ctypes.c_size_t),
                ('pagefile', ctypes.c_size_t), ('peak_pagefile', ctypes.c_size_t),
                ('private', ctypes.c_size_t)]


psapi.GetProcessMemoryInfo.argtypes = (wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD)
kernel32.GetProcessHandleCount.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
kernel32.GetProcessTimes.argtypes = (wintypes.HANDLE, *([ctypes.POINTER(wintypes.FILETIME)] * 4))
user32.GetGuiResources.argtypes = (wintypes.HANDLE, wintypes.DWORD)


def resource_sample(process, elapsed):
    handle = wintypes.HANDLE(process._handle)
    memory = MemoryCounters()
    memory.cb = ctypes.sizeof(memory)
    require(psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb), 'Memory query failed.')
    count = wintypes.DWORD()
    require(kernel32.GetProcessHandleCount(handle, ctypes.byref(count)), 'Handle query failed.')
    times = [wintypes.FILETIME() for _ in range(4)]
    require(kernel32.GetProcessTimes(handle, *(ctypes.byref(value) for value in times)), 'CPU query failed.')
    cpu_seconds = sum((value.dwHighDateTime << 32) + value.dwLowDateTime for value in times[2:]) / 10000000
    return {'elapsed_seconds': round(elapsed, 3), 'private_bytes': memory.private,
            'working_set_bytes': memory.working, 'handles': count.value,
            'gdi_objects': user32.GetGuiResources(handle, 0),
            'user_objects': user32.GetGuiResources(handle, 1), 'cpu_seconds': cpu_seconds}
