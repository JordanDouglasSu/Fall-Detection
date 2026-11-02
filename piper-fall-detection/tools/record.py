#!/usr/bin/env python3
"""
Record a test session from the fall sensor over Wi-Fi.

Saves every thermal frame (plus the room zones and mount) to a CSV file that
tools/evaluate.py can replay through the detector later — so each fall only
has to be performed once, however many times you re-tune.

While recording:
    Enter        mark "the fall happens now" (used to measure latency)
    q + Enter    stop and save          (Ctrl+C also stops and saves)

Examples
    python tools/record.py --host 192.168.43.57 --label fall_sideways --expect alert --person A
    python tools/record.py --host fall01.local --label sit_chair --expect none --person B --seconds 40

Only uses the Python standard library.
"""
import argparse
import datetime as dt
import json
import os
import queue
import re
import sys
import threading
import time
import urllib.error
import urllib.request

LABELS_HINT = ("fall_forward fall_backward fall_sideways fall_toward_camera fall_away_from_camera "
               "slow_slide walk sit_chair sit_floor bend_pick_up crouch lie_bed lie_sofa "
               "two_people hot_drink empty_room")


def get(url, timeout=3.0):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8", "replace")


def stdin_reader(q):
    for line in sys.stdin:
        q.put(line.strip())
    q.put(None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="sensor IP or name, e.g. 192.168.4.1 or fall01.local")
    ap.add_argument("--label", required=True, help="what happens, e.g. " + LABELS_HINT.split()[0])
    ap.add_argument("--expect", required=True, choices=["alert", "none"],
                    help="alert = should raise a fall alert, none = should not")
    ap.add_argument("--person", required=True, help="who performs it (A, B, C... — no real names needed)")
    ap.add_argument("--out", default=os.path.join("data", "real"), help="folder (default data/real)")
    ap.add_argument("--seconds", type=float, default=0, help="stop automatically after this long")
    ap.add_argument("--notes", default="", help="free text saved in the file header")
    args = ap.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9_\-]+", args.label):
        sys.exit("label: use letters, numbers, _ or - only")
    base = f"http://{args.host}"

    try:
        _, cfg_txt = get(base + "/config")
        cfg = json.loads(cfg_txt)
    except (urllib.error.URLError, OSError, ValueError) as e:
        sys.exit(f"Cannot reach the sensor at {base}/config ({e}).\n"
                 "Check the PC is on the same Wi-Fi as the board and the IP is right.")

    os.makedirs(args.out, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    path = os.path.join(args.out, f"{stamp}_{args.person}_{args.label}.csv")

    marks = []
    frames = dropped = 0
    last_fid = None
    first_ms = None
    last_ms = 0
    q = queue.Queue()
    threading.Thread(target=stdin_reader, args=(q,), daemon=True).start()

    print(f"Recording to {path}")
    print("Press Enter at the moment of the fall to mark it.  q + Enter to stop.\n")
    t_start = time.time()
    with open(path, "w", newline="") as f:
        f.write("# piper-fall-session v1\n# source=recorded\n")
        f.write(f"# label={args.label}\n# expect={args.expect}\n# person={args.person}\n")
        f.write(f"# mount={cfg.get('mount', 'wall')}\n# node={cfg.get('node', '')}\n# fw={cfg.get('fw', '')}\n")
        f.write(f"# recorded_at={dt.datetime.now().isoformat(timespec='seconds')}\n")
        if args.notes:
            f.write(f"# notes={args.notes}\n")
        for k, v in cfg.get("params", {}).items():
            f.write(f"# param.{k}={v}\n")
        f.write(f"# mask={cfg.get('mask', '')}\n")
        f.write("t_ms,fid," + ",".join(f"p{i}" for i in range(768)) + "\n")

        stop = False
        last_print = 0
        try:
            while not stop:
                # keyboard
                while not q.empty():
                    cmd = q.get()
                    if cmd is None or cmd.lower() in ("q", "quit", "stop"):
                        stop = True
                    elif first_ms is not None:
                        t = last_ms - first_ms
                        marks.append(t)
                        f.write(f"# mark={t}\n")
                        print(f"\n  marked fall at {t / 1000:.1f} s")
                if args.seconds and time.time() - t_start >= args.seconds:
                    stop = True

                # next frame
                url = base + "/frame" + (f"?since={last_fid}" if last_fid is not None else "")
                try:
                    status, body = get(url, timeout=2.0)
                except (urllib.error.URLError, OSError) as e:
                    print(f"\n  connection problem: {e} — retrying")
                    time.sleep(0.5)
                    continue
                if status == 204 or not body:
                    time.sleep(0.03)
                    continue
                parts = body.split(",")
                if len(parts) != 770:
                    continue
                fid, ms = int(parts[0]), int(parts[1])
                if last_fid is not None:
                    if fid <= last_fid:
                        continue
                    dropped += fid - last_fid - 1
                if first_ms is None:
                    first_ms = ms
                last_fid, last_ms = fid, ms
                f.write(f"{ms - first_ms},{fid}," + ",".join(parts[2:]) + "\n")
                frames += 1

                if time.time() - last_print > 0.5:
                    last_print = time.time()
                    secs = (ms - first_ms) / 1000
                    rate = frames / secs if secs > 0 else 0
                    print(f"\r  {secs:6.1f} s   frames {frames}   {rate:4.1f} fps   missed {dropped}   ",
                          end="", flush=True)
        except KeyboardInterrupt:
            pass

    print(f"\n\nSaved {frames} frames ({(last_ms - (first_ms or 0)) / 1000:.1f} s), "
          f"{dropped} missed, {len(marks)} mark(s) -> {path}")
    if dropped > frames * 0.1:
        print("Note: more than 10% of frames were missed. Move closer to the board or close the dashboard tab.")
    if args.expect == "alert" and not marks:
        print("Note: no fall was marked, so latency can't be measured for this session.")
    print("Check it with:  python tools/evaluate.py " + path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
