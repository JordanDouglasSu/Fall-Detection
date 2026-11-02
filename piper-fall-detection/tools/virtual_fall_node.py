#!/usr/bin/env python3
"""
A software stand-in for the fall-sensor ESP32 on MQTT (like sim/virtual_house.py
for the house node). Sends the same topics and payloads as the real firmware, so
the PIPER backend can be built and tested without the sensor.

Interactive (type a command + Enter):
    fall [reason]   raise a fall alert         (default reason: descent_then_still)
    recover         person gets up             (alert ends: "recovered")
    stumble         descent + floor + got up   (no alert, logged as near-fall)
    descent         just the early warning
    empty / back    person leaves / comes back into view
    quit            stop (the Last Will then marks the node "offline" — try Ctrl+Break too)

Scripted:
    python tools/virtual_fall_node.py --script "3:descent,4:fall,20:recover"

Replaying a recorded session through the real algorithm, in real time:
    python tools/virtual_fall_node.py --session data/real/xxx.csv --speed 2

Needs: pip install paho-mqtt
"""
import argparse
import json
import os
import queue
import subprocess
import sys
import threading
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is missing:  pip install paho-mqtt")

HERE = os.path.dirname(os.path.abspath(__file__))


class VirtualNode:
    def __init__(self, client, node):
        self.c = client
        self.node = node
        self.t0 = time.time()
        self.state = "MONITORING"
        self.alert = False
        self.reason = "none"
        self.posture = "upright"
        self.present = True
        self.event_id = 0
        self.req_id = ""
        self.lock = threading.Lock()

    @property
    def topics(self):
        n = self.node
        return dict(status=f"piper/sensor/{n}", event=f"piper/event/{n}",
                    cmd=f"piper/cmd/{n}", avail=f"piper/availability/{n}")

    def uptime(self):
        return int((time.time() - self.t0) * 1000)

    def publish_status(self):
        st = {"node": self.node, "state": self.state, "alert": self.alert, "reason": self.reason,
              "posture": self.posture, "present": self.present, "still": self.alert,
              "sensor_ok": True, "event_id": self.event_id, "uptime_ms": self.uptime(),
              "req_id": self.req_id, "fw": "virtual"}
        self.c.publish(self.topics["status"], json.dumps(st), qos=1, retain=True)

    def publish_event(self, name, reason=""):
        ev = {"node": self.node, "event": name, "reason": reason,
              "event_id": self.event_id, "uptime_ms": self.uptime()}
        self.c.publish(self.topics["event"], json.dumps(ev), qos=1, retain=False)
        print(f"  -> event {name}" + (f" ({reason})" if reason else ""))

    # ---- simulated detector outcomes
    def do(self, cmd):
        parts = cmd.split()
        if not parts:
            return
        verb, arg = parts[0].lower(), (parts[1] if len(parts) > 1 else "")
        with self.lock:
            if verb == "descent":
                self.state = "DESCENT"; self.posture = "low"
                self.publish_event("descent")
            elif verb == "fall":
                self.event_id += 1
                self.state = "FALL_ALERT"; self.alert = True; self.posture = "floor"
                self.reason = arg or "descent_then_still"
                self.publish_event("fall", self.reason)
            elif verb in ("recover", "recovered"):
                if self.alert:
                    self.alert = False; self.reason = "recovered"
                    self.publish_event("recovered", "recovered")
                self.state = "MONITORING"; self.posture = "upright"
            elif verb == "stumble":
                self.publish_event("descent")
                self.publish_event("on_floor")
                self.publish_event("stumble")
                self.state = "MONITORING"; self.posture = "upright"
            elif verb == "empty":
                self.present = False; self.posture = "none"
            elif verb == "back":
                self.present = True; self.posture = "upright"
            else:
                print(f"  unknown: {cmd}")
                return
            self.publish_status()

    # ---- commands from the backend
    def on_command(self, payload):
        try:
            msg = json.loads(payload)
        except ValueError:
            msg = {}
        action = msg.get("action", "")
        with self.lock:
            self.req_id = str(msg.get("req_id", ""))
            if action in ("clear", "user_ok") and self.alert:
                self.alert = False; self.state = "MONITORING"
                self.reason = "manual" if action == "clear" else "user_ok"
                self.publish_event("cleared", self.reason)
            print(f"  <- command {action or payload!r}")
            self.publish_status()


def run_session(node, session, speed):
    """Replay a recorded session through the real C++ algorithm and emit its events in real time."""
    sys.path.insert(0, HERE)
    import evaluate  # noqa: E402  (builds replay if needed)
    res = evaluate.run_replay([session])[0]
    print(f"session {os.path.basename(session)}: {len(res['events'])} events, "
          f"{res['duration_ms'] / 1000:.0f} s (x{speed})")
    start = time.time()
    for e in res["events"]:
        wait = e["t"] / 1000 / speed - (time.time() - start)
        if wait > 0:
            time.sleep(wait)
        name = e["e"]
        if name == "fall":
            node.do("fall " + e.get("reason", ""))
        elif name == "recovered":
            node.do("recover")
        elif name in ("descent", "on_floor", "stumble"):
            with node.lock:
                node.publish_event(name)
    print("session finished")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--node", default="fall01")
    ap.add_argument("--script", help='timed commands, e.g. "3:descent,4:fall,20:recover"')
    ap.add_argument("--session", help="recorded session to replay through the real algorithm")
    ap.add_argument("--speed", type=float, default=1.0, help="replay speed for --session")
    ap.add_argument("--linger", type=float, default=2.0, help="seconds to stay connected after a script/session")
    args = ap.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"virtual-{args.node}")
    if args.user:
        client.username_pw_set(args.user, args.password)
    node = VirtualNode(client, args.node)
    T = node.topics
    client.will_set(T["avail"], "offline", qos=1, retain=True)

    connected = threading.Event()

    def on_connect(c, u, flags, rc, props=None):
        if rc == 0:
            c.subscribe(T["cmd"], qos=1)
            c.publish(T["avail"], "online", qos=1, retain=True)
            node.publish_status()
            connected.set()
            print(f"virtual {args.node} online at {args.host}:{args.port}")
        else:
            print(f"connect failed: {rc}")

    client.on_connect = on_connect
    client.on_message = lambda c, u, m: node.on_command(m.payload.decode("utf-8", "replace"))
    try:
        client.connect(args.host, args.port, keepalive=15)
    except OSError as e:
        sys.exit(f"Cannot connect to the MQTT broker at {args.host}:{args.port} ({e})")
    client.loop_start()
    if not connected.wait(5):
        sys.exit("no CONNACK from broker")

    try:
        if args.session:
            run_session(node, args.session, args.speed)
            time.sleep(args.linger)
        elif args.script:
            start = time.time()
            for item in args.script.split(","):
                t, cmd = item.split(":", 1)
                wait = float(t) - (time.time() - start)
                if wait > 0:
                    time.sleep(wait)
                node.do(cmd.strip())
            time.sleep(args.linger)
        else:
            print("commands: fall [reason] | recover | stumble | descent | empty | back | quit")
            for line in sys.stdin:
                line = line.strip()
                if line in ("quit", "exit", "q"):
                    break
                node.do(line)
    except KeyboardInterrupt:
        pass
    # leave cleanly: mark offline ourselves (a crash would trigger the Last Will instead)
    client.publish(T["avail"], "offline", qos=1, retain=True).wait_for_publish(2)
    client.loop_stop()
    client.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
