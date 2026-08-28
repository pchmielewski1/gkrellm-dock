#!/usr/bin/env python3
"""Unit tests for GB10 CPU / thermal mapping (via ctypes helpers built as .so)."""
from __future__ import annotations

import ctypes
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIBDIR = ROOT / "lib"
BUILD = ROOT / "build"


def build_testlib() -> Path:
    BUILD.mkdir(exist_ok=True)
    so = BUILD / "libgkdock_test.so"
    srcs = [
        LIBDIR / "cpu_map.c",
        LIBDIR / "cpu_stat.c",
        LIBDIR / "thermal_map.c",
    ]
    cmd = [
        "cc",
        "-shared",
        "-fPIC",
        "-O2",
        "-I",
        str(LIBDIR),
        "-o",
        str(so),
        *[str(s) for s in srcs],
    ]
    subprocess.check_call(cmd)
    return so


class GkCpuList(ctypes.Structure):
    _fields_ = [("count", ctypes.c_int), ("cpus", ctypes.c_int * 64)]


class GkCpuTimes(ctypes.Structure):
    _fields_ = [
        ("user", ctypes.c_ulonglong),
        ("nice", ctypes.c_ulonglong),
        ("system", ctypes.c_ulonglong),
        ("idle", ctypes.c_ulonglong),
        ("iowait", ctypes.c_ulonglong),
        ("irq", ctypes.c_ulonglong),
        ("softirq", ctypes.c_ulonglong),
        ("steal", ctypes.c_ulonglong),
    ]


def load(so: Path):
    lib = ctypes.CDLL(str(so))
    lib.gk_cpu_map_from_cpuinfo.argtypes = [ctypes.c_char_p, ctypes.POINTER(GkCpuList)]
    lib.gk_cpu_map_from_cpuinfo.restype = ctypes.c_int
    lib.gk_cpu_stat_parse.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(GkCpuTimes),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.gk_cpu_stat_parse.restype = ctypes.c_int
    lib.gk_cpu_stat_busy_pct.argtypes = [
        ctypes.POINTER(GkCpuList),
        ctypes.POINTER(GkCpuTimes),
        ctypes.POINTER(GkCpuTimes),
    ]
    lib.gk_cpu_stat_busy_pct.restype = ctypes.c_double
    return lib


def test_cpu_clusters_from_fixture(lib):
    text = (ROOT / "tests/fixtures/cpuinfo_gb10.txt").read_bytes()
    out = (GkCpuList * 2)()
    assert lib.gk_cpu_map_from_cpuinfo(text, out) == 0
    x925 = sorted(out[0].cpus[i] for i in range(out[0].count))
    a725 = sorted(out[1].cpus[i] for i in range(out[1].count))
    assert x925 == [5, 6, 7, 8, 9, 15, 16, 17, 18, 19], x925
    assert a725 == [0, 1, 2, 3, 4, 10, 11, 12, 13, 14], a725


def test_busy_pct_all_idle_to_busy(lib):
    lst = GkCpuList()
    lst.count = 2
    lst.cpus[0] = 0
    lst.cpus[1] = 1
    prev = (GkCpuTimes * 64)()
    cur = (GkCpuTimes * 64)()
    for c in (0, 1):
        prev[c].user = 0
        prev[c].idle = 100
        cur[c].user = 50
        cur[c].idle = 150
    pct = lib.gk_cpu_stat_busy_pct(ctypes.byref(lst), prev, cur)
    assert abs(pct - 50.0) < 0.01, pct


def test_thermal_fixture_names(lib):
    class GkThermalZone(ctypes.Structure):
        _fields_ = [
            ("name", ctypes.c_char * 16),
            ("zone", ctypes.c_int),
            ("temp_mC", ctypes.c_int),
        ]

    class GkThermalMap(ctypes.Structure):
        _fields_ = [("count", ctypes.c_int), ("zones", GkThermalZone * 16)]

    lib.gk_thermal_map_from_text.argtypes = [ctypes.c_char_p, ctypes.POINTER(GkThermalMap)]
    lib.gk_thermal_map_from_text.restype = ctypes.c_int
    lib.gk_thermal_temp_by_name.argtypes = [ctypes.POINTER(GkThermalMap), ctypes.c_char_p]
    lib.gk_thermal_temp_by_name.restype = ctypes.c_int

    text = (ROOT / "tests/fixtures/thermal_zones.txt").read_bytes()
    m = GkThermalMap()
    assert lib.gk_thermal_map_from_text(text, ctypes.byref(m)) == 0
    assert m.count == 7
    assert lib.gk_thermal_temp_by_name(ctypes.byref(m), b"TSOC") > 0
    assert lib.gk_thermal_temp_by_name(ctypes.byref(m), b"TGPU") > 0
    assert lib.gk_thermal_temp_by_name(ctypes.byref(m), b"NOPE") == -1


def main() -> int:
    so = build_testlib()
    lib = load(so)
    test_cpu_clusters_from_fixture(lib)
    test_busy_pct_all_idle_to_busy(lib)
    test_thermal_fixture_names(lib)
    print("OK: all unit tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
