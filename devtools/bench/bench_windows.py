"""Windows parts of devtools/bench/bench.py: P-core mask, pinned launch, OS counters, power plan, build.

Everything here only reads system settings; the runner changes nothing but its own affinity.
"""

import ctypes
import ctypes.wintypes as w
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# The benchmark power plan (Bitsum Highest Performance), matched by GUID: the name prints mangled.
PLAN_GUID = "fe1217fb-3f66-46bf-adc6-e48c27825f80"
PARKING_SETTINGS = ("CPMINCORES", "CPMINCORES1")

_k = ctypes.WinDLL("kernel32", use_last_error=True)
_psapi = ctypes.WinDLL("psapi", use_last_error=True)
_k.GetCurrentProcess.restype = w.HANDLE
_k.GetProcessAffinityMask.argtypes = [w.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
_k.SetProcessAffinityMask.argtypes = [w.HANDLE, ctypes.c_size_t]
_k.GetLogicalProcessorInformationEx.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(w.DWORD)]
_k.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(ctypes.c_uint64)] * 4


class _MemoryCounters(ctypes.Structure):
    _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD)] + [
        (name, ctypes.c_size_t)
        for name in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
            "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage",
        )
    ]


_psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.POINTER(_MemoryCounters), w.DWORD]


def p_core_mask():
    """P-cores 1-7 with both hyperthreads: the highest EfficiencyClass, minus the core with the lowest CPU."""
    size = w.DWORD(0)
    _k.GetLogicalProcessorInformationEx(0, None, ctypes.byref(size))  # 0 = RelationProcessorCore
    buf = ctypes.create_string_buffer(size.value)
    if not _k.GetLogicalProcessorInformationEx(0, buf, ctypes.byref(size)):
        raise OSError(ctypes.get_last_error(), "GetLogicalProcessorInformationEx failed")
    raw, cores, offset = buf.raw, [], 0
    while offset < size.value:
        record_size = int.from_bytes(raw[offset + 4 : offset + 8], "little")
        # PROCESSOR_RELATIONSHIP: Flags, EfficiencyClass, Reserved[20], GroupCount, GROUP_AFFINITY (Mask, Group).
        base = offset + 8
        group = int.from_bytes(raw[base + 32 : base + 34], "little")
        if group != 0:
            raise RuntimeError("More than one processor group; the runner only handles group 0")
        cores.append((raw[base + 1], int.from_bytes(raw[base + 24 : base + 32], "little")))
        offset += record_size
    top = max(efficiency for efficiency, _ in cores)
    p_cores = sorted((m for efficiency, m in cores if efficiency == top), key=lambda m: m & -m)
    if len(p_cores) < 2:
        raise RuntimeError(f"Need at least two P-cores, found {len(p_cores)}")
    mask = 0
    for core in p_cores[1:]:  # P-core 0 stays free for the OS and interrupts
        mask |= core
    return mask


def run_pinned(args, cwd, mask, timeout):
    """Start args with the runner's affinity set to mask (inherited at creation), then read the OS counters."""
    me = _k.GetCurrentProcess()
    previous, system = ctypes.c_size_t(), ctypes.c_size_t()
    if not _k.GetProcessAffinityMask(me, ctypes.byref(previous), ctypes.byref(system)):
        raise OSError(ctypes.get_last_error(), "GetProcessAffinityMask failed")
    if not _k.SetProcessAffinityMask(me, mask):
        raise OSError(ctypes.get_last_error(), "SetProcessAffinityMask failed")
    try:
        proc = subprocess.Popen(
            args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
            errors="replace",
        )
    finally:
        _k.SetProcessAffinityMask(me, previous.value)
    # Private Popen handle: the only place that touches it, so a Python change breaks one function.
    handle = w.HANDLE(int(proc._handle))
    child, _ = ctypes.c_size_t(), ctypes.c_size_t()
    _k.GetProcessAffinityMask(handle, ctypes.byref(child), ctypes.byref(_))
    timed_out = False
    try:
        output, _unused = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()  # our own child only
        output, _unused = proc.communicate()
        timed_out = True
    counters = _MemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    if not _psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
        raise OSError(ctypes.get_last_error(), "GetProcessMemoryInfo failed")
    times = [ctypes.c_uint64() for _ in range(4)]  # creation, exit, kernel, user (100 ns units)
    if not _k.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)):
        raise OSError(ctypes.get_last_error(), "GetProcessTimes failed")
    return {
        "output": output,
        "returncode": proc.returncode,
        "timed_out": timed_out,
        "child_mask": child.value,
        "peak_ws_bytes": counters.PeakWorkingSetSize,
        "cpu_s": (times[2].value + times[3].value) / 1e7,
    }


def _powercfg(*args):
    return subprocess.run(["powercfg", *args], capture_output=True, text=True, errors="replace", check=True).stdout


def check_power_plan():
    """Read the active plan and core parking; raise with the fix commands on a mismatch (never set anything)."""
    found = {"scheme": None}
    problems = []
    match = re.search(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", _powercfg("/getactivescheme"))
    found["scheme"] = match.group(0).lower() if match else None
    if found["scheme"] != PLAN_GUID:
        problems.append(f"active power plan is {found['scheme']}, not {PLAN_GUID}")
    for setting in PARKING_SETTINGS:
        text = _powercfg("/qh", "SCHEME_CURRENT", "SUB_PROCESSOR", setting)
        ac = re.search(r"Current AC Power Setting Index:\s*0x([0-9a-fA-F]+)", text)
        found[setting] = int(ac.group(1), 16) if ac else None
        if found[setting] != 100:
            problems.append(f"{setting} (AC) is {found[setting]}, not 100: core parking is on")
    if problems:
        fix = [f"powercfg /setacvalueindex {PLAN_GUID} SUB_PROCESSOR {s} 100" for s in PARKING_SETTINGS]
        fix.append(f"powercfg /setactive {PLAN_GUID}")
        raise RuntimeError(
            "Machine not in benchmark state: " + "; ".join(problems)
            + ". Nothing was changed. Fix it with:\n  " + "\n  ".join(fix)
        )
    return found


def ensure_idle():
    # A build running next to a measurement shows up as a regression. Refuse; never kill.
    query = (
        "Get-CimInstance Win32_Process | Where-Object { "
        "$_.Name -in @('cl.exe', 'link.exe', 'clang.exe', 'clang-cl.exe') -or "
        "($_.Name -like 'python*' -and $_.CommandLine -match '(?i)\\bSCons\\b') "
        "} | Select-Object -ExpandProperty Name"
    )
    active = subprocess.run(["powershell", "-NoProfile", "-Command", query], capture_output=True, text=True).stdout
    if active.strip():
        raise RuntimeError(f"Compiler or SCons still running ({', '.join(active.split())}); stop it and run again.")


def build_args(target, variant):
    args = ["-Bench", "-Target", target]
    return args + ["-Variant", variant] if variant else args


def build(target, variant):
    command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "devtools/build/build.ps1"]
    result = subprocess.run(command + build_args(target, variant), cwd=ROOT)
    if result.returncode:
        raise RuntimeError(f"Build failed: build.ps1 {' '.join(build_args(target, variant))}")


def engine_exe(target, suffix):
    # The real exe: the .console.exe wrapper is another process with its own PID and memory.
    return ROOT / "bin" / f"godot.windows.{target}.x86_64.{suffix}.exe"
