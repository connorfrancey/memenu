"""Which processes have a file open (Windows Restart Manager; no admin rights needed). Used to explain LNK1168
("cannot open medge_menu.dll for writing")."""

from __future__ import annotations

import ctypes
import uuid
from ctypes import wintypes
from pathlib import Path

_CCH_RM_MAX_APP_NAME = 255
_CCH_RM_MAX_SVC_NAME = 63
_ERROR_MORE_DATA = 234


class _UniqueProcess(ctypes.Structure):
    _fields_ = [("dwProcessId", wintypes.DWORD), ("ProcessStartTime", wintypes.FILETIME)]


class _ProcessInfo(ctypes.Structure):
    _fields_ = [("Process", _UniqueProcess),
                ("strAppName", ctypes.c_wchar * (_CCH_RM_MAX_APP_NAME + 1)),
                ("strServiceShortName", ctypes.c_wchar * (_CCH_RM_MAX_SVC_NAME + 1)),
                ("ApplicationType", ctypes.c_int), ("AppStatus", wintypes.ULONG),
                ("TSSessionId", wintypes.DWORD), ("bRestartable", wintypes.BOOL)]


def holders(path: Path) -> list[tuple[int, str]]:
    """(pid, application name) of every process that has `path` open. Empty when none, or when it can't be asked."""
    try:
        rm = ctypes.WinDLL("rstrtmgr")
    except OSError:
        return []
    session = wintypes.DWORD()
    key = ctypes.create_unicode_buffer(str(uuid.uuid4()), 64)
    if rm.RmStartSession(ctypes.byref(session), 0, key) != 0:
        return []
    try:
        files = (wintypes.LPCWSTR * 1)(str(path))
        if rm.RmRegisterResources(session, 1, files, 0, None, 0, None) != 0:
            return []
        needed, count, reasons = wintypes.UINT(), wintypes.UINT(0), wintypes.DWORD()
        rc = rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), None, ctypes.byref(reasons))
        if rc not in (0, _ERROR_MORE_DATA) or needed.value == 0:
            return []
        info = (_ProcessInfo * needed.value)()
        count = wintypes.UINT(needed.value)
        if rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), info, ctypes.byref(reasons)) != 0:
            return []
        return [(info[i].Process.dwProcessId, info[i].strAppName) for i in range(count.value)]
    finally:
        rm.RmEndSession(session)
