# Fall node on MQTT

Follows the same pattern as the house node in `docs/MQTT_CONTRACT.md`: commands are never
retained, state is always retained, a Last Will reports the node offline, and commands are
confirmed by a `req_id` echoed back in the status. **Add this section to MQTT_CONTRACT.md.**

## Topics

```
piper/sensor/<node_id>         node -> backend   retained       current status
piper/event/<node_id>          node -> backend   not retained   something happened
piper/availability/<node_id>   Last Will         retained       "online" / "offline"
piper/cmd/<node_id>            backend -> node   not retained   commands
```

The default `node_id` is `fall01` (set `NODE_ID` in `config.h`).

## Status — `piper/sensor/fall01` (retained)

Published on any change of state, posture, presence or alert (at most once a second); every 30 s
as a heartbeat; every 10 s while an alert is active; and after every command.

```json
{"node":"fall01","state":"FALL_ALERT","alert":true,"reason":"descent_then_still",
 "posture":"floor","present":true,"still":true,"sensor_ok":true,"event_id":3,
 "uptime_ms":812345,"req_id":"a1b2c3d4","fw":"2.0.0"}
```

| Field | Values |
|---|---|
| `state` | `MONITORING`, `DESCENT`, `ON_FLOOR`, `FALL_ALERT` |
| `alert` | `true` while a fall alert is active |
| `reason` | why the alert was raised (`descent_then_still`, `descent_then_on_floor`, `long_lie`, `floor_too_long`, `lost_from_view_on_floor`) or, after it ended, why it ended (`recovered`, `manual`, `user_ok`) |
| `posture` | `none`, `upright`, `low`, `floor`, `rest` |
| `present` | someone is in view (can also feed the rules engine, e.g. night lighting) |
| `sensor_ok` | `false` if the thermal camera stopped sending frames |
| `event_id` | increments on every fall alert |
| `req_id` | the `req_id` of the last command received (closed loop) |

## Events — `piper/event/fall01` (not retained)

```json
{"node":"fall01","event":"fall","reason":"descent_then_still","event_id":3,"uptime_ms":812345}
```

| `event` | Meaning | Suggested backend action |
|---|---|---|
| `descent` | sudden drop seen | none (or log) |
| `on_floor` | landed on the floor after the drop | optional early "Are you alright?" |
| `stumble` | went down and got up again by themselves | log; tell the caregiver in the daily summary |
| `fall` | **fall confirmed** | run the response flow below |
| `recovered` | the person is upright again, alert ended | tell the caregiver they got up |
| `cleared` | alert ended by a command (`manual` or `user_ok`) | log |

## Commands — `piper/cmd/fall01`

```json
{"action":"user_ok","req_id":"a1b2c3d4"}
```

| `action` | Effect |
|---|---|
| `clear` | caregiver ends the alert |
| `user_ok` | PIPER asked and the person said they're fine — ends the alert, logged differently |
| `status` | just republish status |

Success = a status message with the same `req_id` within 3 s, the same rule as the house node.

## Response flow in the backend

`tools/fall_monitor.py` has a `FallResponder` class with the flow below. Copy it into the
backend and pass in PIPER's own functions:

```python
responder = FallResponder(
    speak=piper_tts_say,                         # Piper TTS
    listen=lambda t: whisper_listen(timeout=t),  # Whisper transcript, or None on silence
    alert_caregiver=send_telegram,               # the same function the report uses for alerts
    send_command=lambda a: mqtt_publish("piper/cmd/fall01", {"action": a, "req_id": new_req_id()}),
)
# in the MQTT on_message for piper/event/fall01:
responder.on_event(json.loads(payload))
```

1. On `fall`: PIPER says *"I think you may have fallen. Are you okay? Do you need help?"*
2. Answer means "fine" → send `user_ok`, and tell the caregiver gently ("possible fall, they say they're okay").
3. "help", or **no answer within 30 s** → Telegram the caregiver straight away. Silence is never treated as fine.
4. Unclear answer → ask once more, then treat as above.
5. On `recovered` → tell the caregiver the person is back on their feet.

`FallResponder.classify()` is a keyword fallback for the answer. In the backend, prefer asking
Gemini to classify the answer as ok / help / unclear, consistent with the rest of the system, and
use the keywords if the LLM is unavailable. **Never let the LLM decide that no alert is needed
when the person did not answer.**

If `piper/availability/fall01` is `offline`, or `sensor_ok` is `false`, PIPER should say it
can't check for falls right now, the same honesty rule as the house node.

## Testing without the board

```bash
python tools/virtual_fall_node.py          # then type: fall / recover / stumble / quit
python tools/fall_monitor.py --respond     # watch, and play the person on the console
python tools/fall_monitor.py --ok          # send user_ok, wait for confirmation
```
