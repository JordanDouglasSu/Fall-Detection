#!/usr/bin/env python3
"""
Watch the fall sensor over MQTT, send it commands, and show how PIPER's
backend should respond to a fall.

Examples
    python tools/fall_monitor.py                       # watch status + events
    python tools/fall_monitor.py --respond             # also run the response flow (asks on the console)
    python tools/fall_monitor.py --clear               # caregiver clears the alert
    python tools/fall_monitor.py --ok                  # "the person says they are fine"
    python tools/fall_monitor.py --wait-for fall --timeout 60   # exit 0 when a fall event arrives

Topics (node id defaults to fall01)
    piper/sensor/fall01         status, retained       {"state","alert","reason","posture","present",...}
    piper/event/fall01          events, not retained   {"event": descent|on_floor|stumble|fall|recovered|cleared, ...}
    piper/availability/fall01   "online" / "offline"   (Last Will)
    piper/cmd/fall01            commands to the node   {"action": clear|user_ok|status, "req_id": "..."}

Needs: pip install paho-mqtt
"""
import argparse
import json
import queue
import sys
import threading
import time
import uuid

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is missing:  pip install paho-mqtt")


# =====================================================================
#  The response flow — copy this class into the PIPER backend.
#  Plug in the backend's own speak / listen / caregiver-alert functions.
# =====================================================================
class FallResponder:
    """
    On a fall event:
      1. PIPER asks the person if they are OK.
      2. "I'm fine"            -> send user_ok to the sensor (alert ends, logged).
      3. "help", or no answer  -> alert the caregiver (Telegram) straight away.
      4. Person seen getting up -> tell the caregiver they fell but got up.
    The sensor never decides alone: the person gets a chance to answer first,
    and silence is treated as "needs help", never as "fine".
    """
    # Keyword fallback only. In the backend, prefer asking Gemini to classify the
    # answer as ok / help / unclear, and keep this as the offline fallback.
    OK_PHRASES = ("not hurt", "don't need help", "do not need help", "no help", "no thanks")
    NOT_OK_PHRASES = ("not ok", "not okay", "not fine", "not alright", "not all right", "not good")
    HELP_WORDS = ("help", "hurt", "pain", "can't get up", "cannot get up", "fallen", "ambulance")
    OK_WORDS = ("fine", "okay", "ok", "alright", "all right", "i'm good", "yes")

    def __init__(self, speak, listen, alert_caregiver, send_command, answer_timeout=30):
        self.speak = speak                      # speak(text)
        self.listen = listen                    # listen(timeout_s) -> text or None
        self.alert_caregiver = alert_caregiver  # alert_caregiver(text)
        self.send_command = send_command        # send_command(action)
        self.answer_timeout = answer_timeout
        self.busy = False

    def classify(self, text):
        if not text:
            return "no_answer"
        t = " " + text.lower().replace("’", "'") + " "
        if any(p in t for p in self.OK_PHRASES):
            return "ok"
        if any(p in t for p in self.NOT_OK_PHRASES):
            return "help"
        if any(w in t for w in self.HELP_WORDS):
            return "help"
        if any(f" {w} " in t or f" {w}," in t or f" {w}." in t for w in self.OK_WORDS):
            return "ok"
        return "unclear"

    def on_event(self, ev):
        name = ev.get("event")
        if name == "fall" and not self.busy:
            threading.Thread(target=self._handle_fall, args=(ev,), daemon=True).start()
        elif name == "recovered":
            self.alert_caregiver("Update: they are back on their feet.")

    def _handle_fall(self, ev):
        self.busy = True
        try:
            self.speak("I think you may have fallen. Are you okay? Do you need help?")
            answer = self.classify(self.listen(self.answer_timeout))
            if answer == "unclear":
                self.speak("Sorry, I didn't catch that. Are you okay, or do you need help?")
                answer = self.classify(self.listen(self.answer_timeout))
            if answer == "ok":
                self.speak("Okay. I'll let your carer know just in case, but take your time getting up.")
                self.send_command("user_ok")
                self.alert_caregiver(f"Possible fall detected ({ev.get('reason')}). They said they are okay.")
            else:
                why = "asked for help" if answer == "help" else "did not answer"
                self.speak("I'm getting help now. Try to stay still and keep warm.")
                self.alert_caregiver(f"FALL DETECTED ({ev.get('reason')}). They {why}. Please check on them now.")
        finally:
            self.busy = False


# =====================================================================
#  Command-line tool
# =====================================================================
def ts():
    return time.strftime("%H:%M:%S")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--node", default="fall01")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--clear", action="store_true", help="send clear and wait for confirmation")
    g.add_argument("--ok", action="store_true", help="send user_ok and wait for confirmation")
    g.add_argument("--status", action="store_true", help="ask for status and print it")
    g.add_argument("--respond", action="store_true", help="run the response flow, answering on the console")
    ap.add_argument("--wait-for", metavar="EVENT", help="exit 0 when this event arrives")
    ap.add_argument("--timeout", type=float, default=0, help="give up after this many seconds")
    args = ap.parse_args()

    t_status = f"piper/sensor/{args.node}"
    t_event = f"piper/event/{args.node}"
    t_avail = f"piper/availability/{args.node}"
    t_cmd = f"piper/cmd/{args.node}"

    q = queue.Queue()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"piper-monitor-{uuid.uuid4().hex[:6]}")
    if args.user:
        client.username_pw_set(args.user, args.password)

    def on_connect(c, u, flags, rc, props=None):
        if rc != 0:
            print(f"{ts()}  MQTT connect failed: {rc}")
            return
        c.subscribe([(t_status, 1), (t_event, 1), (t_avail, 1)])

    def on_message(c, u, msg):
        q.put((msg.topic, msg.payload.decode("utf-8", "replace"), msg.retain))

    client.on_connect = on_connect
    client.on_message = on_message
    try:
        client.connect(args.host, args.port, keepalive=20)
    except OSError as e:
        sys.exit(f"Cannot connect to the MQTT broker at {args.host}:{args.port} ({e})")
    client.loop_start()

    def send(action):
        req = uuid.uuid4().hex[:8]
        client.publish(t_cmd, json.dumps({"action": action, "req_id": req}), qos=1)
        return req

    # ---- one-shot commands: closed loop, wait for the node's status with our req_id
    if args.clear or args.ok or args.status:
        action = "clear" if args.clear else "user_ok" if args.ok else "status"
        time.sleep(0.3)
        req = send(action)
        deadline = time.time() + (args.timeout or 3)
        while time.time() < deadline:
            try:
                topic, body, _ = q.get(timeout=0.2)
            except queue.Empty:
                continue
            if topic == t_status:
                st = json.loads(body)
                if st.get("req_id") == req:
                    print(json.dumps(st, indent=2))
                    client.loop_stop()
                    return 0
        print(f"No confirmation from {args.node} within {args.timeout or 3:.0f} s — is it online?")
        client.loop_stop()
        return 1

    # ---- response flow with console stand-ins for PIPER's voice and Telegram
    responder = None
    if args.respond:
        answers = queue.Queue()
        threading.Thread(target=lambda: [answers.put(l.strip()) for l in sys.stdin], daemon=True).start()

        def listen(timeout):
            print(f"{ts()}  (type the person's answer, {timeout:.0f} s)")
            try:
                return answers.get(timeout=timeout)
            except queue.Empty:
                return None

        responder = FallResponder(
            speak=lambda t: print(f"{ts()}  PIPER says: \"{t}\""),
            listen=listen,
            alert_caregiver=lambda t: print(f"{ts()}  >>> TELEGRAM to caregiver: {t}"),
            send_command=lambda a: (send(a), print(f"{ts()}  sent {a} to {args.node}")),
        )

    print(f"{ts()}  watching {args.node} on {args.host}:{args.port}  (Ctrl+C to stop)")
    deadline = time.time() + args.timeout if args.timeout else None
    try:
        while True:
            if deadline and time.time() > deadline:
                print(f"{ts()}  timeout")
                return 1 if args.wait_for else 0
            try:
                topic, body, retained = q.get(timeout=0.2)
            except queue.Empty:
                continue
            if topic == t_avail:
                print(f"{ts()}  node is {body.upper()}" + ("  (retained)" if retained else ""))
                if body == "offline":
                    print(f"{ts()}  !!! fall sensor unreachable — PIPER should say it can't check for falls")
            elif topic == t_status:
                st = json.loads(body)
                flag = "ALERT" if st.get("alert") else "ok"
                print(f"{ts()}  status  {st.get('state'):<11} {flag:<5} posture={st.get('posture')} "
                      f"present={st.get('present')} reason={st.get('reason')}" + ("  (retained)" if retained else ""))
            elif topic == t_event:
                ev = json.loads(body)
                print(f"{ts()}  EVENT   {ev.get('event')}" + (f" ({ev['reason']})" if ev.get("reason") else ""))
                if responder:
                    responder.on_event(ev)
                if args.wait_for and ev.get("event") == args.wait_for:
                    return 0
    except KeyboardInterrupt:
        return 0
    finally:
        client.loop_stop()


if __name__ == "__main__":
    sys.exit(main())
