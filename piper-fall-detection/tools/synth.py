#!/usr/bin/env python3
"""
Generate SYNTHETIC thermal sessions (32x24) for testing the fall-detection logic.

These scenes are drawn, not recorded: a warm ellipse for the body on a noisy
room background, seen from a wall-mounted sensor. They test that the
algorithm's *logic* behaves as designed (falls in several directions, slow
slides, sitting, bending, bed, heaters, frame-rate independence).
They say NOTHING about real-world accuracy — only recorded sessions can.

Usage:
    python tools/synth.py --out data/synthetic          # all scenarios
    python tools/synth.py --out data/synthetic --list   # list scenario names
"""
import argparse
import math
import os
import sys

import numpy as np

W, H = 32, 24

# Room seen from the wall: floor visible from row 12 down, a bed on the right,
# a radiator on the left wall. '.' normal, 'F' floor, 'B' bed/sofa, 'X' ignore.
def room_mask(ignore_radiator=False):
    rows = []
    for y in range(H):
        r = []
        for x in range(W):
            ch = "F" if y >= 12 else "."
            if 22 <= x <= 31 and 11 <= y <= 16:
                ch = "B"
            if ignore_radiator and 0 <= x <= 2 and 5 <= y <= 11:
                ch = "X"
            r.append(ch)
        rows.append("".join(r))
    return rows


# ---- body poses: (cx, cy, half_length, half_width, angle_deg) ----------------
# angle 90 = vertical (standing), 0 = horizontal (lying across the view)
def standing(cx):           return (cx, 9.0, 6.0, 2.0, 90.0)
def lying_side(cx):         return (cx, 17.5, 6.0, 1.5, 0.0)
def lying_toward(cx):       return (cx, 18.0, 3.5, 2.5, 90.0)   # feet near camera: short, low, wide
def lying_away(cx):         return (cx, 15.5, 2.5, 2.0, 90.0)   # head far away: small, higher up
def sitting_chair(cx):      return (cx, 11.0, 4.5, 2.5, 90.0)
def crouching(cx):          return (cx, 12.0, 3.5, 2.5, 90.0)
def sitting_floor(cx):      return (cx, 16.0, 3.5, 2.5, 90.0)
def in_bed():               return (26.5, 13.5, 4.5, 1.4, 0.0)


class Scene:
    def __init__(self, fps, seed, radiator=None, floor_heater=False):
        self.fps = fps
        self.rng = np.random.default_rng(seed)
        yy, xx = np.mgrid[0:H, 0:W]
        # room: slightly warmer near the ceiling + fixed per-pixel offsets (sensor pattern)
        self.base = 24.0 + 0.6 * (1 - yy / (H - 1)) + self.rng.normal(0, 0.25, (H, W))
        if radiator:
            x1, x2, y1, y2, temp = radiator
            self.base[y1:y2 + 1, x1:x2 + 1] = temp
        if floor_heater:
            self.base[19:22, 8:12] = 33.0
        # supersampling grid for smooth body edges
        ss = 4
        o = (np.arange(ss) + 0.5) / ss
        self.sx = (xx[..., None, None] + o[None, None, None, :]).astype(np.float64)
        self.sy = (yy[..., None, None] + o[None, None, :, None]).astype(np.float64)
        self.keys = []      # (t_seconds, pose, jitter)
        self.patches = []   # warm patches left behind: (t_on, cx, cy, a, b, ang, peak_temp, tau_s)
        self.body_temp = 30.5

    def patch(self, t_on, cx, cy, a, b, ang, peak, tau):
        """A warm patch that appears at t_on and cools with time constant tau (residual body heat)."""
        self.patches.append((t_on, cx, cy, a, b, ang, peak, tau))
        return self

    def _ellipse_cover(self, cx, cy, a, b, ang):
        th = math.radians(ang)
        dx, dy = self.sx - cx, self.sy - cy
        u = dx * math.cos(th) + dy * math.sin(th)
        v = -dx * math.sin(th) + dy * math.cos(th)
        return ((u / a) ** 2 + (v / b) ** 2 <= 1.0).mean(axis=(2, 3))

    def key(self, t, pose, jitter=0.0):
        self.keys.append((t, pose, jitter))
        return self

    def pose_at(self, t):
        if not self.keys:
            return None, 0.0
        if t <= self.keys[0][0]:
            return self.keys[0][1], self.keys[0][2]
        for (t0, p0, j0), (t1, p1, j1) in zip(self.keys, self.keys[1:]):
            if t0 <= t <= t1:
                if p0 is None or p1 is None:
                    return (p0 if t - t0 < t1 - t else p1), (j0 if p0 is not None else j1)
                a = (t - t0) / (t1 - t0) if t1 > t0 else 1.0
                return tuple(v0 + (v1 - v0) * a for v0, v1 in zip(p0, p1)), j0
        return self.keys[-1][1], self.keys[-1][2]

    def render(self, pose, jitter, t=0.0):
        frame = self.base.copy()
        for (t_on, pcx, pcy, pa, pb, pang, peak, tau) in self.patches:
            if t >= t_on:
                cov = self._ellipse_cover(pcx, pcy, pa, pb, pang)
                temp = frame + (peak - frame) * math.exp(-(t - t_on) / tau)
                frame = frame * (1 - cov) + temp * cov
        if pose is not None:
            cx, cy, a, b, ang = pose
            if jitter:
                cx += self.rng.normal(0, jitter)
                a = max(1.0, a + self.rng.normal(0, jitter * 0.6))
                b = max(0.8, b + self.rng.normal(0, jitter * 0.4))
                ang += self.rng.normal(0, jitter * 8)
            cover = self._ellipse_cover(cx, cy, a, b, ang)
            frame = frame * (1 - cover) + self.body_temp * cover
        frame = frame + self.rng.normal(0, 0.12, frame.shape)   # sensor noise
        return frame

    def write(self, path, label, expect, duration, mask, marks=(), mount="wall"):
        dt = 1.0 / self.fps
        n = int(duration * self.fps)
        with open(path, "w", newline="") as f:
            f.write("# piper-fall-session v1\n")
            f.write("# source=synthetic\n")
            f.write(f"# label={label}\n# expect={expect}\n# person=synthetic\n")
            f.write(f"# mount={mount}\n# fps={self.fps}\n")
            f.write("# mask=" + "".join(mask) + "\n")
            for m in marks:
                f.write(f"# mark={int(m * 1000)}\n")
            f.write("t_ms,fid," + ",".join(f"p{i}" for i in range(W * H)) + "\n")
            for k in range(n):
                t = k * dt
                pose, jit = self.pose_at(t)
                fr = self.render(pose, jit, t)
                f.write(f"{int(round(t * 1000))},{k}," + ",".join(f"{v:.2f}" for v in fr.ravel()) + "\n")


# ---- scenarios ------------------------------------------------------------------
# Each returns (scene, expect, duration_s, mask, marks). expect: "alert" or "none".
# Timeline convention: empty room first 3 s, person walks in, event at ~10 s.
def walk_in(sc, x_from=-3.0, x_to=10.0, t0=3.0, t1=8.0):
    sc.key(0, None).key(t0 - 0.01, None)
    sc.key(t0, standing(x_from), 0.25).key(t1, standing(x_to), 0.25)
    return sc


def sc_walk_across(fps):
    sc = Scene(fps, 1)
    walk_in(sc, -3, 14, 3, 9).key(15, standing(34), 0.25)
    return sc, "none", 25, room_mask(), ()


def sc_fall_sideways(fps):
    sc = walk_in(Scene(fps, 2))
    sc.key(10, standing(10), 0.25).key(10.8, lying_side(14), 0.0).key(40, lying_side(14), 0.0)
    return sc, "alert", 30, room_mask(), (10,)


def sc_fall_toward(fps):
    sc = walk_in(Scene(fps, 3))
    sc.key(10, standing(10), 0.25).key(10.7, lying_toward(10), 0.0).key(40, lying_toward(10), 0.0)
    return sc, "alert", 30, room_mask(), (10,)


def sc_fall_away(fps):
    sc = walk_in(Scene(fps, 4))
    sc.key(10, standing(10), 0.25).key(10.7, lying_away(10), 0.0).key(40, lying_away(10), 0.0)
    return sc, "alert", 30, room_mask(), (10,)


def sc_fall_struggling(fps):
    # falls, then keeps moving (trying to get up) but stays on the floor
    sc = walk_in(Scene(fps, 5))
    sc.key(10, standing(10), 0.25).key(10.8, lying_side(14), 0.6).key(40, lying_side(14), 0.6)
    return sc, "alert", 35, room_mask(), (10,)


def sc_stumble_get_up(fps):
    # falls, gets straight back up within ~3 s: near-fall, no alert (logged as stumble)
    sc = walk_in(Scene(fps, 6))
    sc.key(10, standing(10), 0.25).key(10.8, lying_side(13), 0.5).key(12.5, lying_side(13), 0.5)
    sc.key(14.0, standing(12), 0.25).key(25, standing(12), 0.25)
    return sc, "none", 25, room_mask(), ()


def sc_fall_then_recover(fps):
    # falls, alert raised, gets up after 20 s -> alert then "recovered"
    sc = walk_in(Scene(fps, 7))
    sc.key(10, standing(10), 0.25).key(10.8, lying_side(14), 0.0).key(30, lying_side(14), 0.0)
    sc.key(32, standing(12), 0.25).key(40, standing(12), 0.25)
    return sc, "alert", 40, room_mask(), (10,)


def sc_sit_chair(fps):
    sc = walk_in(Scene(fps, 8))
    sc.key(10, standing(10), 0.25).key(10.6, sitting_chair(10), 0.05).key(40, sitting_chair(10), 0.05)
    return sc, "none", 40, room_mask(), ()


def sc_bend_pick_up(fps):
    sc = walk_in(Scene(fps, 9))
    sc.key(10, standing(10), 0.25).key(10.6, crouching(10), 0.3).key(12.5, crouching(10), 0.3)
    sc.key(13.2, standing(10), 0.25).key(25, standing(16), 0.25)
    return sc, "none", 25, room_mask(), ()


def sc_crouch_still(fps):
    # crouches and stays still for 15 s (tying shoes, looking under furniture)
    sc = walk_in(Scene(fps, 10))
    sc.key(10, standing(10), 0.25).key(10.6, crouching(10), 0.05).key(25, crouching(10), 0.05)
    sc.key(26, standing(10), 0.25).key(32, standing(10), 0.25)
    return sc, "none", 32, room_mask(), ()


def sc_lie_on_bed(fps):
    sc = walk_in(Scene(fps, 11), x_to=24)
    sc.key(10, standing(25), 0.25).key(11, in_bed(), 0.0).key(70, in_bed(), 0.0)
    return sc, "none", 60, room_mask(), ()


def sc_slow_slide(fps):
    # slides down a wall over 6 s (no fast descent), then lies still: long-lie path
    sc = walk_in(Scene(fps, 12))
    sc.key(10, standing(10), 0.25).key(13, sitting_chair(10), 0.15).key(16, lying_toward(10), 0.0)
    sc.key(80, lying_toward(10), 0.0)
    return sc, "alert", 60, room_mask(), (10,)


def sc_floor_exercise(fps):
    # lies down slowly and moves about on the floor (stretching) for 45 s
    sc = walk_in(Scene(fps, 13))
    sc.key(10, standing(10), 0.25).key(12, sitting_floor(10), 0.4).key(14, lying_side(12), 0.7)
    sc.key(55, lying_side(12), 0.7).key(57, standing(12), 0.25).key(62, standing(14), 0.25)
    return sc, "none", 62, room_mask(), ()


def sc_radiator_walk(fps):
    # hot radiator on the wall (not masked) + person walking: radiator must not be "the person"
    sc = Scene(fps, 14, radiator=(0, 2, 5, 11, 45.0))
    walk_in(sc, -3, 14, 5, 12).key(25, standing(20), 0.25)
    return sc, "none", 30, room_mask(), ()


def sc_heater_on_floor(fps):
    # a warm object on the floor, nobody in the room: must never become a "long lie"
    sc = Scene(fps, 15, floor_heater=True)
    sc.key(0, None).key(200, None)
    return sc, "none", 150, room_mask(), ()


def sc_walk_away(fps):
    # walks away from the camera: body gets smaller and higher in the image
    sc = Scene(fps, 16)
    sc.key(0, None).key(2.99, None)
    sc.key(3, (16.0, 11.0, 8.5, 3.0, 90.0), 0.2).key(6, (16.0, 11.0, 8.5, 3.0, 90.0), 0.2)
    sc.key(8, (16.0, 8.5, 4.5, 1.6, 90.0), 0.2).key(20, (16.0, 8.5, 4.5, 1.6, 90.0), 0.2)
    return sc, "none", 20, room_mask(), ()


def sc_radiator_fall(fps):
    # radiator present AND a fall: heat source must not hide the person
    sc = Scene(fps, 17, radiator=(0, 2, 5, 11, 45.0))
    walk_in(sc)
    sc.key(10, standing(10), 0.25).key(10.8, lying_side(14), 0.0).key(40, lying_side(14), 0.0)
    return sc, "alert", 30, room_mask(), (10,)


def sc_cat_on_floor(fps):
    # a cat walks across the floor and lies still in the floor zone for a minute
    sc = Scene(fps, 18)
    sc.body_temp = 31.0
    cat = lambda x: (x, 18.5, 1.8, 1.1, 0.0)
    sc.key(0, None).key(2.99, None).key(3, cat(-1), 0.15).key(9, cat(12), 0.15).key(10, cat(12), 0.0)
    sc.key(80, cat(12), 0.0)
    return sc, "none", 75, room_mask(), ()


def sc_residual_heat(fps):
    # sits on the floor (moving a little), gets up and walks out of view;
    # the warm patch they leave on the floor must not become a "long lie"
    sc = walk_in(Scene(fps, 19))
    sc.key(10, standing(10), 0.25).key(12.5, sitting_floor(10), 0.3).key(30, sitting_floor(10), 0.3)
    sc.key(32, standing(11), 0.25).key(38, standing(34), 0.25).key(39, None).key(130, None)
    sc.patch(31.0, 10.0, 18.0, 2.6, 1.8, 0.0, 28.5, 90.0)
    return sc, "none", 120, room_mask(), ()


def sc_fall_out_of_bed(fps):
    # walks to bed, lies still, then rolls off onto the floor beside the bed and stays there
    sc = walk_in(Scene(fps, 20), x_to=24)
    sc.key(10, standing(25), 0.25).key(11, in_bed(), 0.0).key(20, in_bed(), 0.0)
    sc.key(21, (26.0, 19.0, 4.5, 1.5, 0.0), 0.0).key(90, (26.0, 19.0, 4.5, 1.5, 0.0), 0.0)
    return sc, "alert", 75, room_mask(), (20,)


# ---- ceiling mount (looking straight down) -------------------------------------
# Standing = small round blob (head + shoulders). Lying = long blob. Sitting = a bit bigger.
def ceiling_mask():
    rows = []
    for y in range(H):
        rows.append("".join("B" if (21 <= x <= 30 and 2 <= y <= 9) else "F" for x in range(W)))
    return rows


def c_stand(x, y):  return (x, y, 1.7, 1.5, 90.0)
def c_lying(x, y):  return (x, y, 5.0, 1.6, 20.0)
def c_sit(x, y):    return (x, y, 2.4, 1.7, 0.0)
def c_bed():        return (25.5, 5.5, 4.5, 1.5, 0.0)


def sc_ceiling_fall(fps):
    sc = Scene(fps, 30)
    sc.key(0, None).key(2.99, None).key(3, c_stand(2, 12), 0.15).key(9, c_stand(14, 14), 0.15)
    sc.key(10, c_stand(15, 14), 0.15).key(10.7, c_lying(15, 15), 0.0).key(40, c_lying(15, 15), 0.0)
    return sc, "alert", 30, ceiling_mask(), (10,), "ceiling"


def sc_ceiling_walk(fps):
    sc = Scene(fps, 31)
    sc.key(0, None).key(2.99, None).key(3, c_stand(2, 12), 0.15).key(12, c_stand(30, 18), 0.15)
    sc.key(16, c_stand(12, 20), 0.15).key(25, c_stand(5, 4), 0.15)
    return sc, "none", 25, ceiling_mask(), (), "ceiling"


def sc_ceiling_sit_chair(fps):
    sc = Scene(fps, 32)
    sc.key(0, None).key(2.99, None).key(3, c_stand(2, 12), 0.15).key(9, c_stand(14, 14), 0.15)
    sc.key(10, c_stand(14, 14), 0.15).key(11, c_sit(14, 14), 0.05).key(60, c_sit(14, 14), 0.05)
    return sc, "none", 50, ceiling_mask(), (), "ceiling"


def sc_ceiling_bed(fps):
    sc = Scene(fps, 33)
    sc.key(0, None).key(2.99, None).key(3, c_stand(2, 12), 0.15).key(9, c_stand(24, 7), 0.15)
    sc.key(10, c_stand(25, 6), 0.15).key(11, c_bed(), 0.0).key(70, c_bed(), 0.0)
    return sc, "none", 60, ceiling_mask(), (), "ceiling"


SCENARIOS = {
    "walk_across": sc_walk_across,
    "fall_sideways": sc_fall_sideways,
    "fall_toward_camera": sc_fall_toward,
    "fall_away_from_camera": sc_fall_away,
    "fall_struggling": sc_fall_struggling,
    "stumble_get_up": sc_stumble_get_up,
    "fall_then_recover": sc_fall_then_recover,
    "sit_chair": sc_sit_chair,
    "bend_pick_up": sc_bend_pick_up,
    "crouch_still": sc_crouch_still,
    "lie_on_bed": sc_lie_on_bed,
    "slow_slide": sc_slow_slide,
    "floor_exercise": sc_floor_exercise,
    "radiator_walk": sc_radiator_walk,
    "heater_on_floor": sc_heater_on_floor,
    "walk_away": sc_walk_away,
    "radiator_fall": sc_radiator_fall,
    "cat_on_floor": sc_cat_on_floor,
    "residual_heat": sc_residual_heat,
    "fall_out_of_bed": sc_fall_out_of_bed,
    "ceiling_fall": sc_ceiling_fall,
    "ceiling_walk": sc_ceiling_walk,
    "ceiling_sit_chair": sc_ceiling_sit_chair,
    "ceiling_bed": sc_ceiling_bed,
}
# Run these at a second frame rate too, to show timing is frame-rate independent
ALSO_8FPS = ["fall_sideways", "walk_across", "bend_pick_up", "stumble_get_up", "slow_slide"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="data/synthetic", help="output folder")
    ap.add_argument("--fps", type=float, default=4.0, help="main frame rate (default 4)")
    ap.add_argument("--only", nargs="*", help="only these scenarios")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    if args.list:
        print("\n".join(SCENARIOS))
        return 0
    os.makedirs(args.out, exist_ok=True)
    names = args.only or list(SCENARIOS)
    jobs = [(n, args.fps) for n in names] + [(n, 8.0) for n in names if n in ALSO_8FPS and not args.only]
    for name, fps in jobs:
        if name not in SCENARIOS:
            print(f"unknown scenario {name}", file=sys.stderr)
            return 2
        sc, expect, dur, mask, marks, *rest = SCENARIOS[name](fps)
        mount = rest[0] if rest else "wall"
        fn = f"{name}_{int(fps)}fps.csv"
        sc.write(os.path.join(args.out, fn), f"{name}", expect, dur, mask, marks, mount)
        print(f"wrote {fn}  (expect {expect})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
