"""Windows parts of devtools/bench/bench.py: P-core mask, pinned launch, OS counters, machine checks, build.

Everything here only reads system settings; the runner changes nothing but its own affinity.
"""

import csv
import ctypes
import ctypes.wintypes as w
import io
import msvcrt
import os
import re
import statistics
import subprocess
import threading
import time
import winreg
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


def mask_cpus(mask):
    return [cpu for cpu in range(mask.bit_length()) if mask >> cpu & 1]


def set_home_affinity(bench_mask):
    """Keep the runner and its helpers (typeperf, powershell, git) off the measured CPUs; run_pinned returns here."""
    me = _k.GetCurrentProcess()
    own, system = ctypes.c_size_t(), ctypes.c_size_t()
    if not _k.GetProcessAffinityMask(me, ctypes.byref(own), ctypes.byref(system)):
        raise OSError(ctypes.get_last_error(), "GetProcessAffinityMask failed")
    home = system.value & ~bench_mask
    if not home or not _k.SetProcessAffinityMask(me, home):
        raise OSError(ctypes.get_last_error(), f"SetProcessAffinityMask {hex(home)} failed")
    return home


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
        _k.SetProcessAffinityMask(me, previous.value)  # back to the home mask
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
        "wall_s": (times[1].value - times[0].value) / 1e7,  # process creation to exit
    }


def _powercfg(*args):
    return subprocess.run(["powercfg", *args], capture_output=True, text=True, errors="replace", check=True).stdout


def check_power_plan():
    """Read the active plan and core parking (AC and DC); raise with the fix commands on a mismatch (never set)."""
    found = {"scheme": None}
    problems = []
    match = re.search(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", _powercfg("/getactivescheme"))
    found["scheme"] = match.group(0).lower() if match else None
    if found["scheme"] != PLAN_GUID:
        problems.append(f"active power plan is {found['scheme']}, not {PLAN_GUID}")
    for setting in PARKING_SETTINGS:
        text = _powercfg("/qh", "SCHEME_CURRENT", "SUB_PROCESSOR", setting)
        for power in ("AC", "DC"):
            value = re.search(rf"Current {power} Power Setting Index:\s*0x([0-9a-fA-F]+)", text)
            key = f"{setting}_{power}"
            found[key] = int(value.group(1), 16) if value else None
            if found[key] != 100:
                problems.append(f"{setting} ({power}) is {found[key]}, not 100: core parking is on")
    if problems:
        fix = [
            f"powercfg /set{power}valueindex {PLAN_GUID} SUB_PROCESSOR {setting} 100"
            for setting in PARKING_SETTINGS
            for power in ("ac", "dc")
        ]
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


def typeperf_values(text, columns):
    """typeperf CSV samples as float rows. Only values are kept: the header holds the host name (Pitfall 7)."""
    rows = []
    for row in csv.reader(io.StringIO(text)):
        if len(row) != columns + 1:
            continue  # blank lines, "Exiting, please wait..." and other status lines
        try:
            rows.append([float(value) for value in row[1:]])
        except ValueError:
            continue  # the header with the counter paths, or a blank first sample of a rate counter
    return rows


def bench_cpu_load(mask):
    """Mean % Processor Utility of the bench CPUs over 3 seconds."""
    counters = [rf"\Processor Information(0,{cpu})\% Processor Utility" for cpu in mask_cpus(mask)]
    out = subprocess.run(["typeperf", *counters, "-sc", "3"], capture_output=True, text=True, errors="replace").stdout
    rows = typeperf_values(out, len(counters))
    if not rows:
        raise RuntimeError("typeperf returned no samples")
    return statistics.mean(value for row in rows for value in row)


class ClockSampler:
    """Effective clock of the bench CPUs during a round: mean % Processor Performance x Processor Frequency.

    typeperf samples once a second, so a round shorter than that gets the one sample that ends after it.
    """

    def __init__(self, mask):
        counters = [rf"\Processor Information(0,{cpu})\% Processor Performance" for cpu in mask_cpus(mask)]
        counters.append(r"\Processor Information(_Total)\Processor Frequency")
        self.columns = len(counters)
        self.proc = subprocess.Popen(
            ["typeperf", *counters, "-si", "1"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            errors="replace",
        )
        self.lines = []
        self.reader = threading.Thread(target=lambda: self.lines.extend(self.proc.stdout), daemon=True)
        self.reader.start()

    def stop(self):
        deadline = time.monotonic() + 3
        while not typeperf_values("".join(self.lines), self.columns) and time.monotonic() < deadline:
            time.sleep(0.05)  # wait for the first sample of a short round
        self.proc.terminate()  # our own child; typeperf writes each sample as it takes it
        self.proc.wait()
        self.reader.join(timeout=5)
        rows = typeperf_values("".join(self.lines), self.columns)
        if not rows:
            raise RuntimeError("no clock samples")
        return statistics.mean(statistics.mean(row[:-1]) / 100 * row[-1] for row in rows)


def _reg(root, path, name):
    with winreg.OpenKey(root, path) as key:
        return winreg.QueryValueEx(key, name)[0]


def game_mode():
    # AutoGameModeEnabled is absent until Game Mode is toggled once; the probe then reports unavailable.
    return _reg(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\GameBar", "AutoGameModeEnabled")


_CPU_KEY = r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"


def cpu_name():
    return _reg(winreg.HKEY_LOCAL_MACHINE, _CPU_KEY, "ProcessorNameString").strip()


def microcode():
    raw = _reg(winreg.HKEY_LOCAL_MACHINE, _CPU_KEY, "Update Revision")
    value = int.from_bytes(raw, "little")
    return hex(value >> 32 if len(raw) == 8 else value)  # 8 bytes: the revision is the high DWORD


def bios():
    return _reg(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\BIOS", "BIOSVersion").strip()


def os_build():
    key = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"
    return f"{_reg(winreg.HKEY_LOCAL_MACHINE, key, 'CurrentBuild')}.{_reg(winreg.HKEY_LOCAL_MACHINE, key, 'UBR')}"


def _vswhere(prop):
    exe = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    command = [str(exe), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"]
    return subprocess.run(command + ["-property", prop], capture_output=True, text=True, check=True).stdout.strip()


def msvc_toolset():
    # The newest toolset of the newest Visual Studio: the one SCons picks without MSVC_VERSION.
    tools = Path(_vswhere("installationPath")) / "VC/Tools/MSVC"
    return max((d.name for d in tools.iterdir()), key=lambda v: [int(x) for x in v.split(".")])


def vs_version():
    return _vswhere("catalog_productDisplayVersion")


def acquire_lock(path):
    """One runner at a time. The OS drops the lock when the process dies, so a crash leaves no stale lock."""
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(path, "a+b")
    try:
        msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        handle.close()
        raise RuntimeError("Another bench.py is running (devtools/out/bench/.lock); wait for it to end.") from None
    return handle


def build_args(target, variant, defines=None, source_root=None, tests=False):
    args = ["-Bench", "-Target", target] + (["-Tests"] if tests else [])
    if variant:
        args += ["-Variant", variant]
    if defines:
        args += ["-Defines", defines]
    if source_root:
        args += ["-SourceRoot", str(source_root)]
    return args


def scons_args(target, variant, defines=None, tests=False):
    """The SCons arguments of build.ps1 -Bench (kept in step with Invoke-Scons there), for meta.json."""
    suffix = ".".join(part for part in ("bench", variant, "tests" if tests else None) if part)
    args = [f"target={target}", "production=yes"] + (["tests=yes", "object_prefix=t_"] if tests else [])
    args += ["debug_symbols=yes", "disable_path_overrides=no"]
    args = ["platform=windows"] + args + [f"extra_suffix={suffix}"]
    return args + [f"cppdefines={bench_defines(defines)}"]


def bench_defines(defines=None):
    """The cppdefines of build.ps1 -Bench: the allocation counter, then the candidate's defines."""
    return " ".join(part for part in ("FORK_ALLOC_COUNTERS=1", defines) if part)


def build(target, variant, defines=None, source_root=None, tests=False):
    command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "devtools/build/build.ps1"]
    result = subprocess.run(command + build_args(target, variant, defines, source_root, tests), cwd=ROOT)
    if result.returncode:
        shown = build_args(target, variant, defines, "<worktree>" if source_root else None, tests)
        raise RuntimeError(f"Build failed: build.ps1 {' '.join(shown)}")


# The container micro-benchmark (bench.py micro): the same flags on every side. /EHsc because the
# third-party controls (Boost) need exceptions; every side gets it, controls or not.
MICRO_FLAGS = [
    "/O2", "/MT", "/Gy", "/Gw", "/EHsc", "/std:c++20", "/Zc:__cplusplus", "/GR-", "/DDEBUG_ENABLED",
    "/DTOOLS_ENABLED", "/DWINDOWS_ENABLED", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS", "/DFORK_ALLOC_COUNTERS=1",
]
MICRO_LINK = ["/link", "/OPT:REF", "/OPT:ICF", "/INCREMENTAL:NO"]


def msvc_env():
    """vcvars64.bat of the newest Visual Studio with the x64 C++ tools."""
    vcvars = Path(_vswhere("installationPath")) / "VC/Auxiliary/Build/vcvars64.bat"
    if not vcvars.is_file():
        raise RuntimeError("MSVC vcvars64.bat not found")
    return vcvars


def compile_micro(out_dir, inputs, include_dirs, defines):
    """cl inputs into out_dir/bench.exe through a build.cmd; returns the cl version from its banner."""
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = out_dir / "bench.exe"
    exe.unlink(missing_ok=True)
    args = ["cl", *MICRO_FLAGS, *(f"/D{d}" for d in defines), *(f"/I{p}" for p in include_dirs),
            f"/Fo{out_dir}\\", f"/Fe{exe}", *map(str, inputs), *MICRO_LINK]
    command = out_dir / "build.cmd"
    command.write_text(f'@call "{msvc_env()}" >nul\n@if errorlevel 1 exit /b %errorlevel%\n'
                       + subprocess.list2cmdline(args) + "\n@exit /b %errorlevel%\n", encoding="utf-8")
    result = subprocess.run(["cmd", "/d", "/c", str(command)], cwd=ROOT, capture_output=True, text=True,
                            errors="replace")
    log = result.stdout + result.stderr
    (out_dir / "build.log").write_text(log, encoding="utf-8")
    if result.returncode or not exe.is_file():
        raise RuntimeError(f"Micro build failed; see {out_dir / 'build.log'}\n{log[-4000:]}")
    match = re.search(r"Compiler Version (\S+)", log)
    return match.group(1) if match else "unavailable"


def engine_exe(target, suffix, root=ROOT):
    # The real exe: the .console.exe wrapper is another process with its own PID and memory.
    return Path(root) / "bin" / f"godot.windows.{target}.x86_64.{suffix}.exe"
