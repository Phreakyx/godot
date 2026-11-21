"""Linux parts of devtools/bench/bench.py: the same functions as bench_windows.py, plus perf counters and profiles.

Everything here only reads system settings; the runner changes nothing but its own affinity. A wrong setting
is refused with the command that fixes it, printed for the user to run. Linux-only calls (sched_setaffinity,
wait4, fcntl, sysfs) sit inside functions, so this module imports on Windows and its parsers are tested there.
CPU sets are passed as bit masks (bit n = CPU n), as on Windows, so bench.py handles both the same way.
"""

import fnmatch
import os
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMPILER = "gcc"  # gcc or clang; bench.py sets it from --compiler before any build or lookup
CPU_CORE_PMU = "/sys/bus/event_source/devices/cpu_core/cpus"  # the P-cores of an Intel hybrid CPU
CPU_SYS = "/sys/devices/system/cpu"
NO_TURBO = "/sys/devices/system/cpu/intel_pstate/no_turbo"
SCHED_EXT = "/sys/kernel/sched_ext"
PACMAN_CONF = "/etc/pacman.conf"
PARANOID = "/proc/sys/kernel/perf_event_paranoid"
HELD_PACKAGES = ("gcc", "gcc-libs", "clang", "llvm", "llvm-libs", "lld")  # D-17
UNAVAILABLE = "unavailable"
# BENCH-08: user-space counts on the P-core PMU only (Pitfall 10: plain names open a cpu_atom twin too).
COUNTER_EVENTS = ("cpu_core/instructions/u,cpu_core/cycles/u,cpu_core/cache-misses/u,"
                  "cpu_core/cache-references/u")
PERF_TO_TSV = ROOT / "devtools/profile/perf_to_tsv.py"
IDLE_COMM = re.compile(r"^(cc1|cc1plus|lto1|lto-wrapper|collect2|ld|ld\.(bfd|gold|lld)|lld|clang(\+\+)?(-\d+)?|scons)$")


# --- parsers (pure, tested on fixtures in test_bench.py) ---


def parse_cpu_list(text):
    """sysfs CPU list ("0-3,8,10-11") as a sorted list."""
    cpus = set()
    for part in text.strip().split(","):
        if part:
            first, _, last = part.partition("-")
            cpus.update(range(int(first), int(last or first) + 1))
    return sorted(cpus)


def cpu_list_text(cpus):
    """The reverse: [2, 3, 4, 8] -> "2-4,8" (for cpupower and taskset)."""
    runs, cpus = [], sorted(cpus)
    for cpu in cpus:
        if runs and cpu == runs[-1][1] + 1:
            runs[-1][1] = cpu
        else:
            runs.append([cpu, cpu])
    return ",".join(str(a) if a == b else f"{a}-{b}" for a, b in runs)


def cpus_mask(cpus):
    return sum(1 << cpu for cpu in cpus)


def mask_cpus(mask):
    return [cpu for cpu in range(mask.bit_length()) if mask >> cpu & 1]


def bench_cpus(cpu_core_text, siblings):
    """D-12: the cpu_core PMU's CPUs grouped into cores by thread_siblings_list (cpu -> text), minus the core
    holding the lowest CPU (it stays free for the OS and interrupts)."""
    p_cpus = set(parse_cpu_list(cpu_core_text))
    cores = sorted({tuple(sorted(set(parse_cpu_list(siblings[cpu])) & p_cpus)) for cpu in p_cpus})
    if len(cores) < 2:
        raise RuntimeError(f"Need at least two P-cores, found {len(cores)}")
    return sorted(cpu for core in cores[1:] for cpu in core)


def ignorepkg_missing(pacman_conf):
    """The held packages no IgnorePkg pattern of pacman.conf covers (globs as pacman reads them)."""
    patterns = []
    for line in pacman_conf.splitlines():
        match = re.match(r"\s*IgnorePkg\s*=\s*(.*)", line.split("#", 1)[0])
        if match:
            patterns += match.group(1).split()
    return [p for p in HELD_PACKAGES if not any(fnmatch.fnmatchcase(p, pattern) for pattern in patterns)]


def ignorepkg_held(pacman_conf):
    return not ignorepkg_missing(pacman_conf)


def _proc_stat(text):
    """cpu index -> (busy, total) jiffies; busy is everything but idle and iowait."""
    out = {}
    for line in text.splitlines():
        match = re.match(r"cpu(\d+)\s+(.*)", line)
        if match:
            fields = [int(v) for v in match.group(2).split()[:8]]  # user..steal; guest is inside user
            out[int(match.group(1))] = (sum(fields) - fields[3] - fields[4], sum(fields))
    return out


def cpu_load(before, after, cpus):
    """Busy fraction of cpus between two /proc/stat snapshots."""
    a, b = _proc_stat(before), _proc_stat(after)
    busy = sum(b[c][0] - a[c][0] for c in cpus)
    total = sum(b[c][1] - a[c][1] for c in cpus)
    return busy / total if total else 0.0


def os_release(text):
    match = re.search(r'^PRETTY_NAME="?([^"\n]*)"?', text, re.M)
    return match.group(1) if match else None


def cpuinfo_field(text, name):
    match = re.search(rf"^{re.escape(name)}\s*:\s*(.*)$", text, re.M)
    return match.group(1).strip() if match else None


def busy_compilers(procs):
    """procs: (comm, cmdline) pairs; the compiler, linker or SCons processes among them (never killed)."""
    return sorted({comm for comm, cmdline in procs
                   if IDLE_COMM.match(comm) or (comm.startswith("python") and "scons" in cmdline.lower())})


def state_problems(state, cpus, perf):
    """D-13: each wrong setting with the command that fixes it, for the user to run. An unavailable value
    (the sysfs file does not exist here, A6) is recorded, not refused."""
    listed = cpu_list_text(cpus)
    problems = []
    if state["governor"] not in ("performance", UNAVAILABLE):
        problems.append(f"scaling_governor of CPUs {listed} is {state['governor']}, not performance. Fix:"
                        f" sudo cpupower -c {listed} frequency-set -g performance")
    if state["epp"] not in ("performance", UNAVAILABLE):
        files = f"{CPU_SYS}/cpu{_brace(cpus)}/cpufreq/energy_performance_preference"
        problems.append(f"energy_performance_preference of CPUs {listed} is {state['epp']}, not performance"
                        f" (the performance governor sets it). Fix: echo performance | sudo tee {files}")
    if state["no_turbo"] not in ("0", UNAVAILABLE):
        problems.append(f"boost is off (intel_pstate no_turbo = {state['no_turbo']}). Fix: echo 0 | sudo tee {NO_TURBO}")
    if state["sched_ext"] not in ("disabled", UNAVAILABLE):
        problems.append(f"a sched_ext scheduler is {state['sched_ext']} ({state['sched_ext_ops']}): it changes"
                        " placement. Fix: stop it for the series (sudo systemctl stop scx_loader.service, or"
                        f" scx.service on older setups) and check {SCHED_EXT}/state reads disabled")
    if state["compiler_hold"] not in ("held", UNAVAILABLE):
        problems.append(f"compilers not held in {PACMAN_CONF} (missing: {state['compiler_hold']}). Fix: add the"
                        f" line `IgnorePkg = {' '.join(HELD_PACKAGES)}` under [options] (sudoedit {PACMAN_CONF})")
    if perf and state["perf_event_paranoid"] != UNAVAILABLE and int(state["perf_event_paranoid"]) > 2:
        problems.append(f"perf_event_paranoid is {state['perf_event_paranoid']}: counters and profiles need at"
                        " most 2. Fix: sudo sysctl kernel.perf_event_paranoid=2")
    return problems


def _brace(cpus):
    return str(cpus[0]) if len(cpus) == 1 else "{" + ",".join(map(str, cpus)) + "}"


def parse_perf_stat(text):
    """`perf stat -x,` output: counter name (instructions, cycles, cache_misses, cache_references) -> int, or
    None when perf printed <not counted> / <not supported> (missing, never 0)."""
    out = {"instructions": None, "cycles": None, "cache_misses": None, "cache_references": None}
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split(",")
        if len(fields) < 3:
            continue
        match = re.fullmatch(r"(?:cpu_core/)?([\w-]+)(?:/u|:u)?/?", fields[2].strip())
        name = match.group(1).replace("-", "_") if match else None
        if name in out:
            value = fields[0].strip()
            out[name] = int(float(value)) if re.fullmatch(r"\d+(\.\d+)?", value) else None
    return out


def counter_command(path, command):
    """BENCH-08: one counting pass, separate from the timed rounds (perf as a parent would own wait4's rusage)."""
    return ["perf", "stat", "-x,", "-o", str(path), "-e", COUNTER_EVENTS, "--", *map(str, command)]


def linux_binary(target, suffix, clang):
    """SCons puts .llvm before the extra suffix (platform/linuxbsd/detect.py)."""
    return Path("bin") / f"godot.linuxbsd.{target}.x86_64{'.llvm' if clang else ''}.{suffix}"


# --- system reads ---


def _read(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return UNAVAILABLE


def _same(paths):
    """One value when every file agrees, else cpu=value pairs."""
    values = [_read(p) for p in paths]
    return values[0] if len(set(values)) == 1 else " ".join(f"{Path(p).parts[-3]}={v}" for p, v in zip(paths, values))


def read_state(mask):
    """The D-13 settings, read only."""
    cpus = mask_cpus(mask)
    conf = _read(PACMAN_CONF)
    missing = UNAVAILABLE if conf == UNAVAILABLE else ignorepkg_missing(conf)
    return {
        "governor": _same([f"{CPU_SYS}/cpu{c}/cpufreq/scaling_governor" for c in cpus]),
        "epp": _same([f"{CPU_SYS}/cpu{c}/cpufreq/energy_performance_preference" for c in cpus]),
        "no_turbo": _read(NO_TURBO),
        "sched_ext": _read(f"{SCHED_EXT}/state"),
        "sched_ext_ops": _read(f"{SCHED_EXT}/root/ops"),
        "compiler_hold": missing if missing == UNAVAILABLE else ("held" if not missing else " ".join(missing)),
        "perf_event_paranoid": _read(PARANOID),
    }


def p_core_mask():
    """P-cores 1-7 with both hyperthreads, found by the cpu_core PMU (D-12), never by CPU numbers."""
    text = _read(CPU_CORE_PMU)
    if text == UNAVAILABLE:
        raise RuntimeError(f"No {CPU_CORE_PMU}: not an Intel hybrid CPU, or perf events are unavailable")
    siblings = {c: _read(f"{CPU_SYS}/cpu{c}/topology/thread_siblings_list") for c in parse_cpu_list(text)}
    return cpus_mask(bench_cpus(text, siblings))


def set_home_affinity(bench_mask):
    """Keep the runner and its helpers off the measured CPUs; threads and children inherit the home set."""
    home = os.sched_getaffinity(0) - set(mask_cpus(bench_mask))
    if not home:
        raise OSError("No CPU left outside the bench set")
    os.sched_setaffinity(0, home)
    return cpus_mask(home)


def start_pinned(args, cwd, mask, stdout=subprocess.PIPE):
    """Start args with the runner's affinity set to mask (inherited at fork); returns the Popen."""
    previous = os.sched_getaffinity(0)
    os.sched_setaffinity(0, mask_cpus(mask))
    try:
        return subprocess.Popen(
            args, cwd=cwd, stdout=stdout, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
        )
    finally:
        os.sched_setaffinity(0, previous)  # back to the home set


def run_pinned(args, cwd, mask, timeout):
    """Start args pinned, reap it with wait4 (its rusage: peak RSS, CPU time), as bench_windows.run_pinned."""
    import resource  # Unix only; this module imports on every OS

    runner_kib = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss  # what the child's ru_maxrss starts at
    start = time.perf_counter()
    proc = start_pinned(args, cwd, mask)
    child = cpus_mask(os.sched_getaffinity(proc.pid))  # a zombie still answers; only wait4 below reaps it
    output = []
    reader = threading.Thread(target=lambda: output.append(proc.stdout.read()), daemon=True)
    reader.start()
    timed_out = threading.Event()

    def kill():
        timed_out.set()
        os.kill(proc.pid, signal.SIGKILL)  # our own child only, not reaped yet

    timer = threading.Timer(timeout, kill)
    timer.start()
    try:
        _, status, usage = os.wait4(proc.pid, 0)
    finally:
        timer.cancel()
    wall = time.perf_counter() - start
    proc.returncode = os.waitstatus_to_exitcode(status)  # Popen must not wait for a reaped child
    reader.join(timeout=30)
    proc.stdout.close()
    return {
        "output": "".join(output),
        "returncode": proc.returncode,
        "timed_out": timed_out.is_set(),
        "child_mask": child,
        "peak_ws_bytes": child_peak_bytes(usage.ru_maxrss, runner_kib),
        "cpu_s": usage.ru_utime + usage.ru_stime,
        "wall_s": wall,
    }


def child_peak_bytes(child_kib, runner_kib):
    """A child's ru_maxrss (KiB) in bytes. Linux starts it at the spawning process's peak (exec keeps the old address
    space's high-water mark, and vfork's old space is the runner's), so a reading not above the runner's own peak
    may be the runner's: refuse it (02-12 read the runner's 603 MiB for every workload smaller than that)."""
    if child_kib <= runner_kib:
        raise RuntimeError(f"child peak RSS {child_kib} KiB is not above the runner's own {runner_kib} KiB, so it may be"
                           " the runner's; keep the runner small (hash big files with hashlib.file_digest)")
    return child_kib * 1024


def ensure_idle():
    # A build running next to a measurement shows up as a regression. Refuse; never kill.
    procs = []
    for pid in filter(str.isdigit, os.listdir("/proc")):
        comm = _read(f"/proc/{pid}/comm")
        cmdline = _read(f"/proc/{pid}/cmdline").replace("\0", " ") if comm.startswith("python") else ""
        procs.append((comm, cmdline))
    active = busy_compilers(procs)
    if active:
        raise RuntimeError(f"Compiler or SCons still running ({', '.join(active)}); stop it and run again.")


def bench_cpu_load(mask):
    """Mean busy % of the bench CPUs over 3 seconds (/proc/stat)."""
    before = Path("/proc/stat").read_text()
    time.sleep(3)
    return 100 * cpu_load(before, Path("/proc/stat").read_text(), mask_cpus(mask))


def package_temp():
    """hwmon coretemp "Package id 0" in degrees C, or None."""
    for hwmon in Path("/sys/class/hwmon").glob("hwmon*"):
        if _read(hwmon / "name") != "coretemp":
            continue
        for label in hwmon.glob("temp*_label"):
            if _read(label) == "Package id 0":
                return int(_read(hwmon / label.name.replace("_label", "_input"))) / 1000
    return None


class ClockSampler:
    """Mean scaling_cur_freq of the bench CPUs during a round (MHz), sampled every 0.25 s by a thread on the
    home CPUs; temperature_c is the mean package temperature (D-14)."""

    def __init__(self, mask):
        self.paths = [Path(f"{CPU_SYS}/cpu{c}/cpufreq/scaling_cur_freq") for c in mask_cpus(mask)]
        self.mhz, self.temps, self.done = [], [], threading.Event()
        self.temperature_c = None
        self.sample()  # an unreadable file fails here, and the round records the clock as unavailable
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def sample(self):
        self.mhz.append(statistics.mean(int(p.read_text()) for p in self.paths) / 1000)  # kHz
        temp = package_temp()
        if temp is not None:
            self.temps.append(temp)

    def run(self):
        while not self.done.wait(0.25):
            self.sample()

    def stop(self):
        self.done.set()
        self.thread.join(timeout=5)
        self.sample()
        self.temperature_c = statistics.mean(self.temps) if self.temps else None
        return statistics.mean(self.mhz)


def cpu_name():
    return cpuinfo_field(Path("/proc/cpuinfo").read_text(), "model name")


def microcode():
    return cpuinfo_field(Path("/proc/cpuinfo").read_text(), "microcode")


def bios():
    return _read("/sys/class/dmi/id/bios_version")


def os_build():
    return platform.release()  # the kernel release (uname -r)


def distribution():
    return os_release(Path("/etc/os-release").read_text())


def compiler_version():
    """First line of gcc --version or clang --version (D-17: recorded with every run)."""
    out = subprocess.run([COMPILER, "--version"], capture_output=True, text=True, check=True).stdout
    return out.splitlines()[0].strip()


def acquire_lock(path):
    """One runner at a time. The kernel drops the lock when the process dies, so a crash leaves no stale lock."""
    import fcntl  # Linux only

    path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(path, "a+b")
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        handle.close()
        raise RuntimeError("Another bench.py is running (devtools/out/bench/.lock); wait for it to end.") from None
    return handle


# --- builds ---


def bench_defines(defines=None):
    """The cppdefines of a bench build: the allocation counter, then the candidate's defines."""
    return " ".join(part for part in ("FORK_ALLOC_COUNTERS=1", defines) if part)


def scons_args(target, variant, defines=None, tests=False):
    """The setup script's invocation (CUSTOM_BUILD.md "Build (Linux)") with the bench options of build.ps1 -Bench."""
    suffix = ".".join(part for part in ("bench", variant, "tests" if tests else None) if part)
    args = ["platform=linuxbsd", f"target={target}", "production=yes"]
    args += ["tests=yes", "object_prefix=t_"] if tests else []
    args += ["debug_symbols=yes", "disable_path_overrides=no"]
    args += ["use_llvm=yes", "linker=lld"] if COMPILER == "clang" else []
    return args + [f"extra_suffix={suffix}", f"cppdefines={bench_defines(defines)}"]


def build_args(target, variant, defines=None, source_root=None, tests=False):
    """The build command (meta.json); source_root is the directory it runs in."""
    return ["scons", f"-j{os.cpu_count()}", *scons_args(target, variant, defines, tests)]


def build(target, variant, defines=None, source_root=None, tests=False):
    command = build_args(target, variant, defines, source_root, tests)
    if subprocess.run(command, cwd=source_root or ROOT).returncode:
        raise RuntimeError(f"Build failed: {' '.join(command)}" + (" (in the worktree)" if source_root else ""))


def engine_exe(target, suffix, root=ROOT):
    return Path(root) / linux_binary(target, suffix, COMPILER == "clang")


# The container micro-benchmark (bench.py micro): the same flags on every side and for both compilers.
MICRO_FLAGS = [
    "-O2", "-std=gnu++20", "-fno-rtti", "-DDEBUG_ENABLED", "-DTOOLS_ENABLED", "-DLINUXBSD_ENABLED",
    "-DUNIX_ENABLED", "-DFORK_ALLOC_COUNTERS=1",
]
MICRO_LINK = []
MICRO_EXE = "bench"
MICRO_CXX = {"gcc": "g++", "clang": "clang++"}
MICRO_CC = {"gcc": "gcc", "clang": "clang"}
MICRO_C_FLAGS = ["-O2", "-std=gnu17", *(f for f in MICRO_FLAGS if f.startswith("-D"))]


def micro_command(compiler, exe, inputs, include_dirs, defines):
    return [MICRO_CXX[compiler], *MICRO_FLAGS, *(f"-D{d}" for d in defines), *(f"-I{p}" for p in include_dirs),
            "-o", str(exe), *map(str, inputs), *MICRO_LINK]


def micro_c_command(compiler, obj, source, include_dirs, defines):
    # g++ and clang++ would compile a .c input as C++: C inputs (core/os/fork_mimalloc.c) get their own
    # C compile with the same -D defines, and the object goes into micro_command.
    return [MICRO_CC[compiler], *MICRO_C_FLAGS, *(f"-D{d}" for d in defines), *(f"-I{p}" for p in include_dirs),
            "-c", str(source), "-o", str(obj)]


def compile_micro(out_dir, inputs, include_dirs, defines):
    """g++ or clang++ inputs into out_dir/bench; returns the compiler's version line."""
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = out_dir / MICRO_EXE
    exe.unlink(missing_ok=True)
    log = ""
    objects = []
    for source in (Path(i) for i in inputs if Path(i).suffix == ".c"):
        obj = out_dir / (source.stem + ".o")
        c_command = micro_c_command(COMPILER, obj, source, include_dirs, defines)
        result = subprocess.run(c_command, cwd=ROOT, capture_output=True, text=True, errors="replace")
        log += " ".join(c_command) + "\n" + result.stdout + result.stderr
        if result.returncode:
            (out_dir / "build.log").write_text(log, encoding="utf-8")
            raise RuntimeError(f"Micro build failed; see {out_dir / 'build.log'}\n{log[-4000:]}")
        objects.append(obj)
    cxx_inputs = [i for i in inputs if Path(i).suffix != ".c"] + objects
    command = micro_command(COMPILER, exe, cxx_inputs, include_dirs, defines)
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors="replace")
    log += " ".join(command) + "\n" + result.stdout + result.stderr
    (out_dir / "build.log").write_text(log, encoding="utf-8")
    if result.returncode or not exe.is_file():
        raise RuntimeError(f"Micro build failed; see {out_dir / 'build.log'}\n{log[-4000:]}")
    version = subprocess.run([MICRO_CXX[COMPILER], "--version"], capture_output=True, text=True).stdout
    return version.splitlines()[0].strip() if version else UNAVAILABLE


# --- profiles (BENCH-07): perf record, then perf script into the sampler's TSV format ---


def sampler_exe():
    perf = shutil.which("perf")
    if not perf:
        raise RuntimeError("perf not found. Install it with: sudo pacman -S --needed perf")
    return perf


def perf_profile(perf, command, mask, form, warmup, seconds, stem, log, timeout):
    """perf record the command pinned with taskset (perf itself stays on the home CPUs); a frames workload is
    recorded after warmup for seconds, then ended (SIGINT to perf, which ends its own workload); a launch
    workload until it exits. Writes <stem>.perf.data and <stem>.tsv; returns (perf Popen, CompletedProcess)."""
    data, tsv = stem.with_name(stem.name + ".perf.data"), stem.with_name(stem.name + ".tsv")
    # --user-callchains: at perf_event_paranoid=2 a -g call graph that may include kernel frames is refused.
    record = [perf, "record", "-e", "cpu_core/cycles/u", "-F", "997", "-g", "--user-callchains", "-o", str(data)]
    if form == "frames":
        record += ["-D", str(int(warmup * 1000))]
    record += ["--", "taskset", "-c", cpu_list_text(mask_cpus(mask)), *map(str, command)]
    proc = subprocess.Popen(record, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    try:
        proc.wait(timeout=warmup + seconds if form == "frames" else timeout)
        if form == "frames":
            raise RuntimeError(f"perf or the workload ended before the sampling did (exit {proc.returncode})")
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=120)
        except subprocess.TimeoutExpired:
            proc.kill()  # the runner's own child only
            proc.wait()
    with open(tsv, "w", encoding="utf-8") as out:
        # Clang binaries: binutils addr2line restarts on timeouts (6-7 min per profile, 02-12) and added no source
        # line; with llvm-addr2line perf 7.3 skips the external tool (same TSV, 8 s), a perf that takes it is right too.
        addr2line = ["--addr2line", "llvm-addr2line"] if COMPILER == "clang" else []
        script = subprocess.Popen([perf, "script", "-i", str(data), "-F", "tid,comm,ip,sym,dso,srcline", "--inline",
                                   "--full-source-path", *addr2line], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        convert = subprocess.run([sys.executable, str(PERF_TO_TSV)], stdin=script.stdout, stdout=out)
        script.stdout.close()
        script.wait()
    rows = sum(1 for line in tsv.read_text(encoding="utf-8").splitlines() if line and not line.startswith("#"))
    code = script.returncode or convert.returncode
    return proc, subprocess.CompletedProcess(record, code, stdout=f"perf record {data.name}: {rows} stacks", stderr="")
