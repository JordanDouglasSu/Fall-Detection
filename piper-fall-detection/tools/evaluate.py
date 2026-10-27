#!/usr/bin/env python3
"""
Evaluate the fall detector on recorded (or synthetic) sessions.

Runs the SAME C++ algorithm as the ESP32 (via replay/replay.cpp) on every
session file and reports sensitivity, specificity, false alarms per hour and
detection latency — the numbers for the report.

Examples
    python tools/evaluate.py data/real
    python tools/evaluate.py data/real --compare                 # new vs original algorithm
    python tools/evaluate.py data/real --set stillConfirmMs=5000
    python tools/evaluate.py data/real --sweep fgDelta=1.0,1.3,1.6,2.0
    python tools/evaluate.py data/real --persons B,C             # test only on people B and C
    python tools/evaluate.py data/real --out results.md

Session files are written by tools/record.py (or tools/synth.py). Each file
says what SHOULD happen in its header: "# expect=alert" or "# expect=none".
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALGO_DIR = os.path.join(ROOT, "firmware", "piper_fall_detect")
REPLAY_SRC = os.path.join(ROOT, "replay", "replay.cpp")
SOURCES = [REPLAY_SRC, os.path.join(ROOT, "replay", "legacy_algo.h"), os.path.join(ALGO_DIR, "fall_algo.h")]
EXE = os.path.join(ROOT, "replay", "build", "replay" + (".exe" if os.name == "nt" else ""))


# ---------------------------------------------------------------- build
def find_compiler():
    for c in [os.environ.get("CXX"), "g++", "clang++", "c++"]:
        if c and shutil.which(c):
            return c
    return None


def ensure_replay():
    """Build replay if it is missing or older than the algorithm sources."""
    if os.path.exists(EXE) and all(os.path.getmtime(EXE) >= os.path.getmtime(s) for s in SOURCES):
        return EXE
    cxx = find_compiler()
    if not cxx:
        sys.exit(
            "No C++ compiler found to build the replay tool.\n"
            "  Windows: install MSYS2 (https://www.msys2.org) then run  pacman -S mingw-w64-ucrt-x86_64-gcc\n"
            "           and add C:\\msys64\\ucrt64\\bin to PATH.\n"
            "  macOS:   xcode-select --install\n"
            "  Linux:   sudo apt install g++\n"
            "Or set CXX to your compiler."
        )
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [cxx, "-std=c++17", "-O2", "-I", ALGO_DIR, REPLAY_SRC, "-o", EXE]
    if os.name == "nt":
        cmd.append("-static")
    print("building replay:", " ".join(cmd), file=sys.stderr)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("replay build failed:\n" + r.stderr)
    return EXE


# ---------------------------------------------------------------- run
def collect_files(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(glob.glob(os.path.join(p, "**", "*.csv"), recursive=True))
        else:
            files += sorted(glob.glob(p))
    return files


def session_duration_ms(path):
    """First and last timestamp in a session file (read cheaply)."""
    first = last = None
    with open(path, "r", newline="") as f:
        for line in f:
            if not line or line[0] in "#t\n\r":
                continue
            t = int(line.split(",", 1)[0])
            if first is None:
                first = t
            last = t
    return (last - first) if first is not None else 0


def run_replay(files, sets=(), algo="new", mount=None, mask=None):
    exe = ensure_replay()
    base = [exe, "--algo", algo]
    for s in sets:
        base += ["--set", s]
    if mount:
        base += ["--mount", mount]
    if mask:
        base += ["--mask", mask]
    results = []
    for i in range(0, len(files), 40):          # batches keep Windows command lines short
        r = subprocess.run(base + files[i:i + 40], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("replay failed:\n" + r.stderr)
        if r.stderr.strip():
            print(r.stderr.strip(), file=sys.stderr)
        for line in r.stdout.splitlines():
            if line.strip():
                results.append(json.loads(line))
    for res in results:
        res["duration_ms"] = session_duration_ms(res["file"])
    return results


def metrics(results):
    tp = fn = fp = tn = 0
    none_ms = 0
    false_alerts = 0
    latencies = []
    unknown = 0
    for r in results:
        exp = r.get("expect", "")
        if exp not in ("alert", "none"):
            unknown += 1
            continue
        if exp == "alert":
            if r["alert"]:
                tp += 1
                if r.get("marks"):
                    latencies.append((r["first_alert_ms"] - r["marks"][0]) / 1000.0)
            else:
                fn += 1
        else:
            none_ms += r["duration_ms"]
            n_alerts = sum(1 for e in r["events"] if e["e"] == "fall")
            false_alerts += n_alerts
            if r["alert"]:
                fp += 1
            else:
                tn += 1
    def ratio(a, b):
        return a / b if b else float("nan")
    hours = none_ms / 3.6e6
    return {
        "tp": tp, "fn": fn, "fp": fp, "tn": tn, "unknown": unknown,
        "sensitivity": ratio(tp, tp + fn),
        "specificity": ratio(tn, tn + fp),
        "precision": ratio(tp, tp + fp),
        "accuracy": ratio(tp + tn, tp + tn + fp + fn),
        "false_alarms": false_alerts,
        "normal_hours": hours,
        "false_alarms_per_hour": ratio(false_alerts, hours),
        "latency_median_s": sorted(latencies)[len(latencies) // 2] if latencies else float("nan"),
        "latency_max_s": max(latencies) if latencies else float("nan"),
    }


# ---------------------------------------------------------------- report
def pct(x):
    return "–" if x != x else f"{x * 100:.0f}%"


def num(x, fmt="{:.1f}"):
    return "–" if x != x else fmt.format(x)


def outcome(r):
    exp = r.get("expect", "?")
    if exp == "alert":
        return "TP" if r["alert"] else "**FN (missed)**"
    if exp == "none":
        return "**FP (false alarm)**" if r["alert"] else "TN"
    return "?"


def summary_table(rows):
    out = ["| | Sessions | Sensitivity | Specificity | False alarms / h | Median latency | Max latency |",
           "|---|---|---|---|---|---|---|"]
    for name, m in rows:
        n = m["tp"] + m["fn"] + m["fp"] + m["tn"]
        out.append(f"| {name} | {n} ({m['tp'] + m['fn']} falls) | {pct(m['sensitivity'])} "
                   f"({m['tp']}/{m['tp'] + m['fn']}) | {pct(m['specificity'])} ({m['tn']}/{m['tn'] + m['fp']}) | "
                   f"{num(m['false_alarms_per_hour'])} | {num(m['latency_median_s'])} s | {num(m['latency_max_s'])} s |")
    return out


def session_table(results, other=None):
    head = "| Session | Label | Person | Expected | Result | Reason | Alert at |"
    sep = "|---|---|---|---|---|---|---|"
    if other:
        head += " Original algorithm |"
        sep += "---|"
    out = [head, sep]
    other_by_file = {r["file"]: r for r in (other or [])}
    for r in results:
        when = f"{r['first_alert_ms'] / 1000:.1f} s" if r["alert"] else "–"
        line = (f"| {os.path.basename(r['file'])} | {r.get('label', '')} | {r.get('person', '')} | "
                f"{r.get('expect', '')} | {outcome(r)} | {r.get('reason', '–') if r['alert'] else '–'} | {when} |")
        if other:
            o = other_by_file.get(r["file"])
            line += f" {outcome(o) if o else '?'} |"
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="session files or folders")
    ap.add_argument("--set", action="append", default=[], metavar="NAME=VALUE", help="override a parameter")
    ap.add_argument("--algo", choices=["new", "legacy"], default="new")
    ap.add_argument("--compare", action="store_true", help="also run the original algorithm")
    ap.add_argument("--sweep", metavar="NAME=V1,V2,...", help="try several values of one parameter")
    ap.add_argument("--persons", help="only sessions from these people (comma separated)")
    ap.add_argument("--labels", help="only sessions with these labels (comma separated)")
    ap.add_argument("--mount", choices=["wall", "ceiling"])
    ap.add_argument("--mask", help="room mask file (24 lines of 32 chars) to use instead of the session's")
    ap.add_argument("--out", help="also write the report to this Markdown file")
    ap.add_argument("--json", action="store_true", help="print raw per-session results as JSON lines")
    ap.add_argument("--params", action="store_true", help="list tuning parameters and defaults")
    args = ap.parse_args()

    if args.params:
        subprocess.run([ensure_replay(), "--params"])
        return 0

    files = collect_files(args.paths)
    if not files:
        sys.exit("no .csv session files found")
    results = run_replay(files, args.set, args.algo, args.mount, args.mask)
    keep = lambda r: ((not args.persons or r.get("person") in args.persons.split(",")) and
                      (not args.labels or r.get("label") in args.labels.split(",")))
    results = [r for r in results if keep(r)]
    if not results:
        sys.exit("no sessions left after filtering")

    if args.json:
        for r in results:
            print(json.dumps(r))
        return 0

    lines = ["# Fall detection evaluation", ""]
    lines.append(f"Sessions: {len(results)} from {', '.join(args.paths)}")
    if args.set:
        lines.append(f"Overrides: {', '.join(args.set)}")
    if any("synthetic" in r["file"] for r in results):
        lines.append("")
        lines.append("> Includes SYNTHETIC sessions: these test the logic only, not real-world accuracy.")
    lines.append("")

    if args.sweep:
        name, vals = args.sweep.split("=", 1)
        rows = []
        for v in vals.split(","):
            res = [r for r in run_replay(files, args.set + [f"{name}={v}"], args.algo, args.mount, args.mask) if keep(r)]
            rows.append((f"{name} = {v}", metrics(res)))
        lines += ["## Parameter sweep", ""] + summary_table(rows) + [""]
    else:
        m = metrics(results)
        rows = [("This algorithm" if args.algo == "new" else "Original algorithm", m)]
        legacy = None
        if args.compare and args.algo == "new":
            legacy = [r for r in run_replay(files, (), "legacy", args.mount, args.mask) if keep(r)]
            rows.append(("Original algorithm", metrics(legacy)))
        lines += ["## Summary", ""] + summary_table(rows) + [""]
        lines.append("Sensitivity = falls detected / falls. Specificity = normal sessions with no alert / normal sessions.")
        lines.append("Latency = time from the marked fall (Enter pressed while recording) to the alert.")
        lines += ["", "## Sessions", ""] + session_table(results, legacy) + [""]

    report = "\n".join(lines)
    print(report)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(report + "\n")
        print(f"\nwritten to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
