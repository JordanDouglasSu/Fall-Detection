#!/usr/bin/env python3
"""
Plot what the detector saw during one session — useful for tuning and for
report figures.

    python tools/plot_session.py data/real/xxx.csv               # saves xxx.png next to it
    python tools/plot_session.py data/real/xxx.csv --set fgDelta=1.6 --out fig.png

Panels: body top/bottom (rows), share of body on the floor, motion vs the
"still" threshold, and the detector state. Dashed lines = events, red = alert,
black = the fall marked during recording.

Needs: pip install matplotlib
"""
import argparse
import csv
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import evaluate  # noqa: E402

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is missing:  pip install matplotlib")

STATE_Y = {"MONITORING": 0, "DESCENT": 1, "ON_FLOOR": 2, "FALL_ALERT": 3}
POSTURE_Y = {"none": 0, "upright": 1, "low": 2, "floor": 3, "rest": 4}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--set", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--out")
    args = ap.parse_args()

    exe = evaluate.ensure_replay()
    fd, trace = tempfile.mkstemp(suffix=".csv")
    os.close(fd)
    cmd = [exe, "--trace", trace]
    for s in args.set:
        cmd += ["--set", s]
    r = subprocess.run(cmd + [args.session], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr)
    import json
    summary = json.loads(r.stdout.strip().splitlines()[-1])
    params = dict(l.split("=", 1) for l in subprocess.run([exe, "--params"], capture_output=True, text=True).stdout.split())
    for s in args.set:
        k, v = s.split("=", 1)
        params[k] = v
    still_thr = float(params.get("stillMotion", 0.15))

    rows = list(csv.DictReader(open(trace)))
    os.remove(trace)
    t = [int(x["t_ms"]) / 1000 for x in rows]
    present = [x["present"] == "1" for x in rows]
    def col(name):
        return [float(x[name]) if p else float("nan") for x, p in zip(rows, present)]

    fig, ax = plt.subplots(4, 1, figsize=(11, 8.5), sharex=True,
                           gridspec_kw={"height_ratios": [2.2, 1.2, 1.4, 1.2]})
    ax[0].plot(t, col("top"), label="top of body")
    ax[0].plot(t, col("bottom"), label="bottom of body")
    ax[0].plot(t, col("cy"), label="centre", alpha=0.6)
    ax[0].invert_yaxis()
    ax[0].set_ylabel("image row\n(0 = top)")
    ax[0].legend(loc="upper right", fontsize=8)

    ax[1].plot(t, col("floor_frac"), color="tab:green")
    ax[1].axhline(float(params.get("floorFracMin", 0.6)), color="tab:green", ls=":", lw=1)
    ax[1].set_ylim(-0.05, 1.05)
    ax[1].set_ylabel("share on\nfloor zone")

    ax[2].plot(t, col("motion"), color="tab:purple", label="motion")
    ax[2].axhline(still_thr, color="tab:purple", ls=":", lw=1, label="still below this")
    ax[2].set_ylabel("motion (°C)")
    ax[2].legend(loc="upper right", fontsize=8)

    ax[3].step(t, [STATE_Y.get(x["state"], 0) for x in rows], where="post", color="black", label="state (left)")
    ax[3].set_yticks(list(STATE_Y.values()))
    ax[3].set_yticklabels(list(STATE_Y.keys()), fontsize=8)
    ax[3].set_ylim(-0.5, 3.5)
    ax[3].set_xlabel("time (s)")
    pa = ax[3].twinx()
    pa.step(t, [POSTURE_Y.get(x["posture"], 0) for x in rows], where="post",
            color="tab:orange", alpha=0.8, label="posture (right)")
    pa.set_yticks(list(POSTURE_Y.values()))
    pa.set_yticklabels(list(POSTURE_Y.keys()), fontsize=8, color="tab:orange")
    pa.set_ylim(-0.5, 4.5)
    h1, l1 = ax[3].get_legend_handles_labels()
    h2, l2 = pa.get_legend_handles_labels()
    ax[3].legend(h1 + h2, l1 + l2, loc="upper left", fontsize=8)

    for e in summary["events"]:
        x = e["t"] / 1000
        color = "red" if e["e"] == "fall" else "grey"
        for a in ax:
            a.axvline(x, color=color, ls="--", lw=1 if e["e"] != "fall" else 1.8, alpha=0.7)
        ax[0].annotate(e["e"], (x, 0), xycoords=("data", "axes fraction"), rotation=90,
                       fontsize=7, va="bottom", ha="right", color=color)
    for m in summary.get("marks", []):
        for a in ax:
            a.axvline(m / 1000, color="black", lw=1.5, alpha=0.8)

    verdict = "ALERT" if summary["alert"] else "no alert"
    fig.suptitle(f"{os.path.basename(args.session)} — expected {summary.get('expect') or '?'}, got {verdict}"
                 + (f" ({summary.get('reason')})" if summary["alert"] else ""), fontsize=11)
    fig.tight_layout()
    out = args.out or os.path.splitext(args.session)[0] + ".png"
    fig.savefig(out, dpi=130)
    print(f"saved {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
