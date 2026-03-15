#!/usr/bin/env python3
"""
CRIU Memory Pages Compression Benchmark

Measures the performance and storage impact of CRIU's compression
options across different memory workload patterns.

Compression modes:
  none      - No compression
  per-page  - Each 4 KiB page compressed as its own LZ4 block
  region    - N consecutive pages compressed as a single LZ4 block
              (region size set via --region-sizes)

Workload patterns (defined in workload.py):
  zero    - All pages zero-filled (best case: highly compressible)
  mixed   - 50% zeros, 25% repeated data, 25% random (typical)
  random  - All pages random bytes (worst case: incompressible)
  text    - JSON-like ASCII text (real-world heap shape)
  elf     - Concatenated system binaries (binary blob shape)

Methodology:
  - Each configuration runs N iterations (default 3) + 1 warmup
  - Page cache is dropped between runs for consistent IO
  - Results show median with interquartile range (P25..P75)
  - Memory integrity validated via SHA-256 after each restore
  - For region mode the benchmark sweeps multiple region sizes by
    default to expose the size/ratio/throughput tradeoff.

Usage:
  sudo python3 contrib/compression-benchmark/main.py
  sudo python3 contrib/compression-benchmark/main.py -n 5 -s 512
  sudo python3 contrib/compression-benchmark/main.py -p zero mixed random
  sudo python3 contrib/compression-benchmark/main.py \\
       --modes none per-page region \\
       --region-sizes 65536 262144 1048576
"""

import argparse
import json
import math
import os
import platform
import signal
import subprocess
import sys
import tempfile
import time

# The workload module lives next to this script. fill_pattern() is the
# single source of truth for what bytes the workload writes to memory,
# so we import it both for spawning the workload (as a separate process)
# and for computing the expected SHA-256 the restore side must match.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import workload  # noqa: E402

PAGE_SIZE = 4096
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WORKLOAD_PATH = os.path.join(SCRIPT_DIR, "workload.py")

# The benchmark lives at contrib/compression-benchmark/, so the repo root
# is two directories up. Anchor the default criu binary and the pycriu
# library to it so the script works regardless of the current directory.
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))

config = {"criu_bin": os.path.join(REPO_ROOT, "criu", "criu")}

# Track active child PIDs for cleanup on interrupt
_active_pids = set()
_tempdirs = set()


def _cleanup():
    """Kill all tracked child processes and remove temp directories."""
    for pid in list(_active_pids):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass  # Process already exited
        try:
            os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            pass  # Already reaped
    _active_pids.clear()

    import shutil
    for d in list(_tempdirs):
        shutil.rmtree(d, ignore_errors=True)
    _tempdirs.clear()


def _sighandler(signum, frame):
    _cleanup()
    sys.exit(1)


# --- Statistics ---

def median(v):
    s = sorted(v)
    n = len(s)
    if n == 0:
        return 0
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def pct(v, p):
    s = sorted(v)
    n = len(s)
    if n == 0:
        return 0
    k = (n - 1) * p / 100
    f, c = math.floor(k), math.ceil(k)
    return s[f] if f == c else s[f] * (c - k) + s[c] * (k - f)


# --- System info ---

def collect_system_info():
    info = {"kernel": platform.release(), "arch": platform.machine(),
            "cpus": os.cpu_count()}
    try:
        with open("/proc/cpuinfo") as f:
            for ln in f:
                if ln.startswith("model name"):
                    info["cpu"] = ln.split(":", 1)[1].strip()
                    break
    except OSError:
        pass  # /proc may not be available
    try:
        with open("/proc/meminfo") as f:
            for ln in f:
                if ln.startswith("MemTotal"):
                    info["memory_mb"] = int(ln.split()[1]) // 1024
                    break
    except OSError:
        pass  # /proc may not be available
    try:
        r = subprocess.run([config["criu_bin"], "--version"],
                           capture_output=True, text=True)
        for ln in r.stdout.strip().split("\n"):
            if ln.startswith("Version"):
                info["criu"] = ln.strip()
                break
    except OSError:
        pass  # criu binary not found
    return info


def drop_caches():
    try:
        subprocess.run(["sync"], check=True)
        with open("/proc/sys/vm/drop_caches", "w") as f:
            f.write("3\n")
    except OSError:
        pass  # Not fatal; may lack permissions or /proc access


# --- Workload ---

def expected_checksum(pattern, size):
    """SHA-256 of the bytes the workload will write. Computed locally
    by reusing workload.pattern_checksum() so the driver and the workload
    process can never disagree about what 'pattern X' looks like."""
    return workload.pattern_checksum(pattern, size)


def start_workload(pattern, size, workdir):
    pp = os.path.join(workdir, "pid")
    hp = os.path.join(workdir, "hash")
    proc = subprocess.Popen(
        [sys.executable, WORKLOAD_PATH, str(size), pattern, pp, hp],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _active_pids.add(proc.pid)
    deadline = time.monotonic() + 30
    for path in (pp, hp):
        while not os.path.exists(path):
            if time.monotonic() > deadline:
                proc.kill()
                _active_pids.discard(proc.pid)
                raise RuntimeError("Workload timeout")
            time.sleep(0.05)
    with open(pp) as f:
        pid = int(f.read().strip())
    _active_pids.add(pid)
    return proc, pid, hp


# --- Trial ---

def cfg_label(cfg):
    """Short, stable label for a (mode, region_size) pair."""
    if cfg["mode"] == "none":
        return "none"
    if cfg["mode"] == "per-page":
        return "per-page"
    return f"region-{cfg['region_size']//1024}K"


def run_trial(pattern, size, cfg, workdir, expect):
    imgdir = os.path.join(workdir, "img")
    os.makedirs(imgdir)
    drop_caches()
    proc, pid, hp = start_workload(pattern, size, workdir)

    try:
        # Dump
        cmd = [config["criu_bin"], "dump", "-t", str(pid),
               "-D", imgdir, "--shell-job", "-v0", "-o", "dump.log"]
        mode = cfg["mode"]
        if mode == "per-page":
            cmd += ["-c"]
        elif mode == "region":
            cmd += ["--compress-region", str(cfg["region_size"])]
        if mode != "none" and config.get("compress_acceleration", 1) != 1:
            cmd += ["--compress-acceleration",
                    str(config["compress_acceleration"])]
        t0 = time.monotonic()
        r = subprocess.run(cmd, capture_output=True)
        dump_wall = int((time.monotonic() - t0) * 1e6)
        if r.returncode:
            raise RuntimeError(f"dump failed (rc={r.returncode}): {r.stderr.decode(errors='replace')[-400:]}")
        proc.wait()

        # Measure image
        pages_sz = sum(os.path.getsize(os.path.join(imgdir, f))
                       for f in os.listdir(imgdir)
                       if f.startswith("pages-") and os.path.isfile(os.path.join(imgdir, f)))
        total_sz = sum(os.path.getsize(os.path.join(imgdir, f))
                       for f in os.listdir(imgdir)
                       if os.path.isfile(os.path.join(imgdir, f)))

        # Restore
        t0 = time.monotonic()
        r = subprocess.run([config["criu_bin"], "restore", "-D", imgdir,
                            "--shell-job", "-v0", "-o", "restore.log", "-d"],
                           capture_output=True)
        restore_wall = int((time.monotonic() - t0) * 1e6)
        if r.returncode:
            raise RuntimeError(f"restore failed (rc={r.returncode})")

        with open(os.path.join(workdir, "pid")) as f:
            rpid = int(f.read().strip())
        _active_pids.add(rpid)
        with open(hp) as f:
            valid = f.read().strip() == expect
        try:
            os.kill(rpid, signal.SIGTERM)
            os.waitpid(rpid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass  # Restored process already exited
        _active_pids.discard(rpid)

        # CRIU internal stats
        sys.path.insert(0, os.path.join(REPO_ROOT, "lib"))
        from pycriu import images as pimg
        ds = rs = {}
        for name in ("stats-dump", "stats-restore"):
            p = os.path.join(imgdir, name)
            if os.path.exists(p):
                with open(p, "rb") as f:
                    e = pimg.load(f)["entries"][0]
                    if "dump" in e:
                        ds = e["dump"]
                    if "restore" in e:
                        rs = e["restore"]

        return {
            "pages_size": pages_sz, "total_size": total_sz,
            "pages_written": ds.get("pages_written", 0),
            "frozen_us": ds.get("frozen_time", 0),
            "memdump_us": ds.get("memdump_time", 0),
            "memwrite_us": ds.get("memwrite_time", 0),
            "dump_wall_us": dump_wall,
            "restore_us": rs.get("restore_time", 0),
            "restore_wall_us": restore_wall,
            "valid": valid,
        }
    finally:
        try:
            proc.kill()
            proc.wait()
        except OSError:
            pass  # Process was already killed by criu dump
        _active_pids.discard(proc.pid)
        subprocess.run(["rm", "-rf", imgdir], capture_output=True)


# --- Formatting ---

def fsz(b):
    return f"{b/1048576:.1f} MB" if b >= 1048576 else f"{b/1024:.1f} KB"


def fus(us):
    return f"{us/1e6:.3f} s" if us >= 1e6 else f"{us/1000:.1f} ms"


def ftp(byt, us):
    return f"{byt/1048576/(us/1e6):.0f} MB/s" if us > 0 else "N/A"


# --- Report ---

def report(name, size_mb, results_by_cfg, cfg_order):
    """results_by_cfg: dict label -> list of trial dicts."""
    W = 88
    print()
    print("=" * W)
    print(f"  {name.upper()} ({size_mb} MB, n={len(next(iter(results_by_cfg.values())))})")
    print("=" * W)

    ok = all(r["valid"] for trials in results_by_cfg.values() for r in trials)
    print(f"  Memory integrity: {'PASS' if ok else 'FAIL'}")
    if not ok:
        print("  WARNING: checksum mismatch detected")
    print()

    # Pick a baseline for ratio: prefer "none", else first config.
    baseline_label = "none" if "none" in results_by_cfg else cfg_order[0]
    base_pages = median([r["pages_size"] for r in results_by_cfg[baseline_label]])

    # Storage
    print(f"  STORAGE          {'pages':>12s}  {'total':>12s}  {'ratio':>10s}  {'saved':>8s}")
    print("  " + "-" * (W - 2))
    for label in cfg_order:
        trials = results_by_cfg[label]
        ps = median([r["pages_size"] for r in trials])
        ts = median([r["total_size"] for r in trials])
        ratio = ps / base_pages if base_pages else 0
        saved = (1 - ratio) if base_pages else 0
        print(f"  {label:<14s}  {fsz(ps):>12s}  {fsz(ts):>12s}  "
              f"{ratio:>8.3f}x  {saved:>7.0%}")
    print()

    # Latency
    rows = [
        ("Frozen time",     "frozen_us"),
        ("Memory dump",     "memdump_us"),
        ("Page write",      "memwrite_us"),
        ("Dump total",      "dump_wall_us"),
        ("Restore total",   "restore_wall_us"),
    ]
    print("  LATENCY (median)")
    header = f"  {'metric':<14s} | " + " | ".join(f"{lab:>12s}" for lab in cfg_order)
    print(header)
    print("  " + "-" * (W - 2))
    for label, key in rows:
        cells = [fus(median([r[key] for r in results_by_cfg[lab]])) for lab in cfg_order]
        print(f"  {label:<14s} | " + " | ".join(f"{c:>12s}" for c in cells))
    print()

    # Throughput
    data = median([r["pages_written"] for r in results_by_cfg[baseline_label]]) * PAGE_SIZE
    print(f"  THROUGHPUT (per {fsz(data)} of pages dumped)")
    print(header)
    print("  " + "-" * (W - 2))
    for label, key in [("Dump (write)", "memwrite_us"), ("Restore", "restore_us")]:
        cells = [ftp(data, median([r[key] for r in results_by_cfg[lab]])) for lab in cfg_order]
        print(f"  {label:<14s} | " + " | ".join(f"{c:>12s}" for c in cells))


# --- Main ---

def main():
    ap = argparse.ArgumentParser(description=__doc__,
         formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--iterations", type=int, default=3,
                    help="Measured iterations (default: 3)")
    ap.add_argument("-s", "--size", type=int, default=256,
                    help="Workload memory in MB (default: 256)")
    ap.add_argument("-p", "--data-pattern", nargs="+", default=["mixed"],
                    choices=workload.supported_patterns(),
                    help="Data patterns to test (default: mixed)")
    ap.add_argument("--modes", nargs="+",
                    default=["none", "per-page", "region"],
                    choices=["none", "per-page", "region"],
                    help="Compression modes to compare (default: all three)")
    ap.add_argument("--region-sizes", nargs="+", type=int,
                    default=[65536, 262144, 1048576],
                    help="Region sizes (bytes) to sweep when --modes contains "
                         "region (default: 64K 256K 1M)")
    ap.add_argument("--compress-acceleration", type=int, default=1,
                    help="LZ4 acceleration level (default: 1, higher=faster)")
    ap.add_argument("--criu", default=config["criu_bin"],
                    help="Path to criu (default: %(default)s)")
    ap.add_argument("--json", metavar="FILE",
                    help="Write raw results to JSON")
    ap.add_argument("--no-progress-bar", action="store_true",
                    help="Disable progress bar")
    args = ap.parse_args()
    config["criu_bin"] = args.criu
    config["compress_acceleration"] = args.compress_acceleration

    if os.getuid() != 0:
        sys.exit("Error: must run as root")
    if not os.access(config["criu_bin"], os.X_OK):
        sys.exit(f"Error: {config['criu_bin']} not found")

    import atexit
    signal.signal(signal.SIGINT, _sighandler)
    signal.signal(signal.SIGTERM, _sighandler)
    atexit.register(_cleanup)

    # Build the configuration matrix: each entry is a dict {mode, region_size}.
    cfgs = []
    for m in args.modes:
        if m == "region":
            for rs in args.region_sizes:
                cfgs.append({"mode": "region", "region_size": rs})
        else:
            cfgs.append({"mode": m, "region_size": 0})
    cfg_labels = [cfg_label(c) for c in cfgs]

    info = collect_system_info()
    size = args.size * 1024 * 1024
    results = {}

    print()
    print("CRIU Compression Benchmark")
    print(f"  Kernel : {info.get('kernel', '?')}")
    print(f"  CPU    : {info.get('cpu', '?')}")
    print(f"  Memory : {info.get('memory_mb', '?')} MB")
    print(f"  CRIU   : {info.get('criu', '?')}")
    print(f"  Config : {args.size} MB, {args.iterations}+1 iterations, "
          f"modes={','.join(cfg_labels)}")

    show_progress = sys.stdout.isatty() and not args.no_progress_bar

    for pat in args.data_pattern:
        exp = expected_checksum(pat, size)
        results_by_cfg = {lab: [] for lab in cfg_labels}
        total = args.iterations + 1

        for i in range(total):
            warmup = (i == 0)
            for cfg, lab in zip(cfgs, cfg_labels):
                wd = tempfile.mkdtemp(prefix="criu-bench-")
                _tempdirs.add(wd)
                try:
                    r = run_trial(pat, size, cfg, wd, exp)
                    if not warmup:
                        results_by_cfg[lab].append(r)
                finally:
                    import shutil
                    shutil.rmtree(wd, ignore_errors=True)
                    _tempdirs.discard(wd)
            if show_progress:
                done = i + 1
                bar_w = 20
                filled = bar_w * done // total
                bar = "#" * filled + "." * (bar_w - filled)
                label = "warmup" if warmup else f"{i}/{args.iterations}"
                print(f"\r  {pat}: [{bar}] {label}  ", end="", flush=True)

        if show_progress:
            print(f"\r{' ' * 40}\r", end="")

        report(pat, args.size, results_by_cfg, cfg_labels)
        results[pat] = {lab: results_by_cfg[lab] for lab in cfg_labels}

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"system": info,
                       "config": {"iterations": args.iterations,
                                  "size_mb": args.size,
                                  "modes": cfg_labels,
                                  "region_sizes": args.region_sizes,
                                  "compress_acceleration":
                                      args.compress_acceleration},
                       "results": results}, f, indent=2)
        print(f"\nResults written to {args.json}")

    print()


if __name__ == "__main__":
    main()
