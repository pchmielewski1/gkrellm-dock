#!/usr/bin/env python3
"""Unit tests for Headroom /stats + /health JSON parsing."""
from __future__ import annotations

import ctypes
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"

STATS_FIXTURE = r"""
{
  "summary": {"primary_model": "qwen3.8-27b", "api_requests": 735},
  "requests": {"total": 735, "failed": 1, "by_model": {"qwen3.8-27b": 735}},
  "tokens": {
    "input": 18839637,
    "output": 0,
    "saved": 419151,
    "savings_percent": 2.18,
    "active_savings_percent": 2.18
  },
  "latency": {"average_ms": 693.9, "min_ms": 9.09, "max_ms": 10102.81},
  "display_session": {"savings_percent": 99.0, "tokens_saved": 1}
}
"""

HEALTH_FIXTURE = r"""
{
  "service": "headroom-proxy",
  "status": "healthy",
  "ready": true,
  "checks": {"kompress": {"backend": "onnx", "status": "healthy"}},
  "runtime": {
    "compression_executor": {
      "max_workers": 20,
      "queued": 3,
      "running": 1,
      "in_flight": 2
    }
  }
}
"""


class GkHeadroomStats(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_int),
        ("healthy", ctypes.c_int),
        ("primary_model", ctypes.c_char * 64),
        ("savings_percent", ctypes.c_double),
        ("latency_avg_ms", ctypes.c_double),
        ("tokens_saved", ctypes.c_double),
        ("requests_total", ctypes.c_double),
        ("requests_failed", ctypes.c_double),
        ("workers_in_flight", ctypes.c_double),
        ("workers_queued", ctypes.c_double),
    ]


def build_testlib() -> Path:
    BUILD.mkdir(exist_ok=True)
    so = BUILD / "libgkdock_test.so"
    srcs = [
        ROOT / "lib" / "cpu_map.c",
        ROOT / "lib" / "cpu_stat.c",
        ROOT / "lib" / "thermal_map.c",
        ROOT / "lib" / "headroom_stats.c",
    ]
    cmd = [
        "cc",
        "-shared",
        "-fPIC",
        "-O2",
        "-I",
        str(ROOT / "lib"),
        "-o",
        str(so),
        *[str(s) for s in srcs],
    ]
    subprocess.check_call(cmd)
    return so


def main() -> int:
    so = build_testlib()
    lib = ctypes.CDLL(str(so))
    lib.gk_headroom_parse_stats.argtypes = [ctypes.c_char_p, ctypes.POINTER(GkHeadroomStats)]
    lib.gk_headroom_parse_stats.restype = ctypes.c_int
    lib.gk_headroom_parse_health.argtypes = [ctypes.c_char_p, ctypes.POINTER(GkHeadroomStats)]
    lib.gk_headroom_parse_health.restype = ctypes.c_int

    st = GkHeadroomStats()
    rc = lib.gk_headroom_parse_stats(STATS_FIXTURE.encode(), ctypes.byref(st))
    assert rc == 0, rc
    assert st.ok == 1
    assert st.primary_model.decode() == "qwen3.8-27b"
    assert abs(st.savings_percent - 2.18) < 0.001, st.savings_percent
    assert abs(st.latency_avg_ms - 693.9) < 0.01, st.latency_avg_ms
    assert abs(st.tokens_saved - 419151) < 0.5, st.tokens_saved
    assert abs(st.requests_total - 735) < 0.5
    assert abs(st.requests_failed - 1) < 0.5

    rc = lib.gk_headroom_parse_health(HEALTH_FIXTURE.encode(), ctypes.byref(st))
    assert rc == 0, rc
    assert st.healthy == 1
    assert abs(st.workers_in_flight - 2) < 0.5, st.workers_in_flight
    assert abs(st.workers_queued - 3) < 0.5, st.workers_queued

    bad = GkHeadroomStats()
    assert lib.gk_headroom_parse_stats(b"{}", ctypes.byref(bad)) != 0
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
