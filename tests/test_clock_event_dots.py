#!/usr/bin/env python3
"""Unit tests for nvidia clock-event status-dot state machine."""
from __future__ import annotations

import ctypes
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
SRC = ROOT / "plugins" / "nvidia" / "clock_event_dots.c"
INC = ROOT / "plugins" / "nvidia"

GK_DOT_OK = 0
GK_DOT_ACTIVE = 1
GK_DOT_UNKNOWN = 2
N = 6

CAP = 0x4
SWTH = 0x20
HWTH = 0x40
HWSL = 0x8
BRK = 0x80
APP = 0x2
ALL = CAP | SWTH | HWTH | HWSL | BRK | APP


class GkClockEventDots(ctypes.Structure):
    _fields_ = [
        ("state", ctypes.c_int * N),
        ("latch_until", ctypes.c_double * N),
    ]


def build_so() -> Path:
    BUILD.mkdir(exist_ok=True)
    so = BUILD / "libclock_event_dots_test.so"
    subprocess.check_call(
        [
            "cc",
            "-shared",
            "-fPIC",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{INC}",
            "-o",
            str(so),
            str(SRC),
        ]
    )
    return so


def load(so: Path):
    lib = ctypes.CDLL(str(so))
    lib.gk_clock_event_dots_init.argtypes = [ctypes.POINTER(GkClockEventDots)]
    lib.gk_clock_event_dots_init.restype = None
    lib.gk_clock_event_dots_update.argtypes = [
        ctypes.POINTER(GkClockEventDots),
        ctypes.c_int,
        ctypes.c_ulonglong,
        ctypes.c_ulonglong,
        ctypes.c_double,
    ]
    lib.gk_clock_event_dots_update.restype = None
    lib.gk_clock_event_dots_pulse.argtypes = [
        ctypes.POINTER(GkClockEventDots),
        ctypes.c_int,
        ctypes.c_double,
    ]
    lib.gk_clock_event_dots_pulse.restype = None
    lib.gk_clock_event_dot_label.argtypes = [ctypes.c_int]
    lib.gk_clock_event_dot_label.restype = ctypes.c_char_p
    lib.gk_clock_event_dot_desc.argtypes = [ctypes.c_int]
    lib.gk_clock_event_dot_desc.restype = ctypes.c_char_p
    return lib


def states(d: GkClockEventDots):
    return [d.state[i] for i in range(N)]


def test_unavailable_all_unknown(lib):
    d = GkClockEventDots()
    lib.gk_clock_event_dots_init(ctypes.byref(d))
    lib.gk_clock_event_dots_update(ctypes.byref(d), 0, 0, ALL, 10.0)
    assert states(d) == [GK_DOT_UNKNOWN] * N


def test_idle_all_ok(lib):
    d = GkClockEventDots()
    lib.gk_clock_event_dots_init(ctypes.byref(d))
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, 0, ALL, 10.0)
    assert states(d) == [GK_DOT_OK] * N


def test_cap_active(lib):
    d = GkClockEventDots()
    lib.gk_clock_event_dots_init(ctypes.byref(d))
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, CAP, ALL, 10.0)
    assert states(d) == [GK_DOT_ACTIVE, GK_DOT_OK, GK_DOT_OK, GK_DOT_OK, GK_DOT_OK, GK_DOT_OK]


def test_latch_holds_then_clears(lib):
    d = GkClockEventDots()
    lib.gk_clock_event_dots_init(ctypes.byref(d))
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, CAP, ALL, 10.0)
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, 0, ALL, 11.0)
    assert d.state[0] == GK_DOT_ACTIVE
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, 0, ALL, 13.0)
    assert d.state[0] == GK_DOT_OK


def test_unsupported_cap_unknown(lib):
    d = GkClockEventDots()
    lib.gk_clock_event_dots_init(ctypes.byref(d))
    supported = ALL & ~CAP
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, 0, supported, 10.0)
    assert d.state[0] == GK_DOT_UNKNOWN
    assert states(d)[1:] == [GK_DOT_OK] * 5


def test_labels(lib):
    expect = [b"Cap", b"SWth", b"HWth", b"HWsl", b"Brk", b"App"]
    for i, name in enumerate(expect):
        assert lib.gk_clock_event_dot_label(i) == name
        assert lib.gk_clock_event_dot_desc(i)


def test_pulse_latches_missed_blip(lib):
    """ViolationStatus-style pulse: bit never seen, still ACTIVE + latch."""
    d = GkClockEventDots()
    lib.gk_clock_event_dots_init(ctypes.byref(d))
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, 0, ALL, 10.0)
    assert d.state[0] == GK_DOT_OK
    lib.gk_clock_event_dots_pulse(ctypes.byref(d), 0, 10.0)  # Cap blip
    assert d.state[0] == GK_DOT_ACTIVE
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, 0, ALL, 11.0)
    assert d.state[0] == GK_DOT_ACTIVE
    lib.gk_clock_event_dots_update(ctypes.byref(d), 1, 0, ALL, 13.0)
    assert d.state[0] == GK_DOT_OK


def test_pulse_skips_unknown(lib):
    d = GkClockEventDots()
    lib.gk_clock_event_dots_init(ctypes.byref(d))
    lib.gk_clock_event_dots_update(ctypes.byref(d), 0, 0, ALL, 10.0)
    lib.gk_clock_event_dots_pulse(ctypes.byref(d), 0, 10.0)
    assert d.state[0] == GK_DOT_UNKNOWN


def main() -> int:
    so = build_so()
    lib = load(so)
    test_unavailable_all_unknown(lib)
    test_idle_all_ok(lib)
    test_cap_active(lib)
    test_latch_holds_then_clears(lib)
    test_unsupported_cap_unknown(lib)
    test_labels(lib)
    test_pulse_latches_missed_blip(lib)
    test_pulse_skips_unknown(lib)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
