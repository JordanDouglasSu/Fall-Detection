# PIPER — Thermal Fall Detection (v2)

Fall detection for PIPER using an ESP32 and a GY-MCU90640 thermal camera (32 × 24 pixels).
It replaces the first sketch's single-frame thresholds with a detector that follows a fall
through its phases — **fast descent → on the floor → still** — plus a **long-lie** path for slow
slides, and it reports to the PIPER backend over MQTT.

The detection code (`fall_algo.h`) is plain C++. The **same file** runs on the ESP32 and in the
PC replay tool, so every number in the report comes from the exact code on the board.

```
firmware/piper_fall_detect/
    piper_fall_detect.ino   ESP32 sketch: sensor, dashboard, Wi-Fi, MQTT
    config.h                ← your settings: Wi-Fi, MQTT, room zones, tuning
    fall_algo.h             the detector (shared with the PC tools)
    dashboard.h             web page: live view, events, zone painter
replay/
    replay.cpp              runs fall_algo.h on recorded sessions (built automatically)
    legacy_algo.h           the ORIGINAL sketch's logic, for before/after comparison
tools/
    record.py               record test sessions from the board over Wi-Fi
    evaluate.py             sensitivity / specificity / latency, compare, parameter sweeps
    plot_session.py         plot what the detector saw (tuning + report figures)
    synth.py                synthetic test scenes (logic tests only)
    fall_monitor.py         watch/command the node over MQTT + PIPER's response flow
    virtual_fall_node.py    software stand-in for the board on MQTT (backend testing)
tests/run_tests.py          automated tests
docs/
    ALGORITHM.md            how it works — for the report and the viva
    TEST_PROTOCOL.md        how to collect real fall data safely and report it
    MQTT.md                 topics and payloads (add to MQTT_CONTRACT.md) + backend integration
    synthetic_results.md    current results on the synthetic scenes
```

---

## 1. Flash the board

1. Arduino IDE → Boards Manager → **esp32 by Espressif** (core 3.x). Board: *ESP32 Dev Module*.
2. Library Manager → install **PubSubClient** (by Nick O'Leary).
3. Open `firmware/piper_fall_detect/piper_fall_detect.ino` and edit **`config.h`**:
   - `WIFI_SSID` / `WIFI_PASS`: the same 2.4 GHz hotspot as the PC and the other ESP32s.
     Leave empty to have the board make its own network (`PIPER-Fall-fall01`).
   - `MQTT_HOST`: the PC running Mosquitto.
   - `AP_PASS`: **change it** before the demo.
   - `SENSOR_MOUNT`: `fall::MOUNT_WALL` (default) or `fall::MOUNT_CEILING`.
4. Upload. The Serial Monitor (115200) prints the dashboard address, e.g. `http://192.168.43.57/`.

Wiring is unchanged from the first sketch: sensor TX → GPIO16, sensor RX → GPIO17, 5 V, GND.
The onboard LED blinks while checking a possible fall and stays on during an alert.

## 2. Mark the room zones (important)

The detector decides "on the floor" by **where** the body is in the image, so it needs to know
where the floor is. The bed/sofa zone is what stops lying in bed from looking like a fall.

1. Open the dashboard → **Room zones → Edit zones**.
2. Paint **Floor** over every pixel where the floor is visible (up to where it meets the far wall).
3. Paint **Bed / sofa** over anything people lie on. Paint **Ignore** over heaters, TVs, sunny windows.
4. **Save to sensor**. Zones are stored in flash and survive restarts.
   **Copy for config.h** gives the same zones as code, to keep in git.

Tip: stand where the floor meets the far wall and watch the *row* read-out under the image.
Your feet are at the top edge of the floor zone.

## 3. Record and evaluate (the report numbers)

```bash
pip install -r tools/requirements.txt
python tools/record.py --host 192.168.43.57 --label fall_sideways --expect alert --person A
#   press Enter at the moment of the fall, q + Enter to stop
python tools/evaluate.py data/real --compare --out results.md
python tools/plot_session.py data/real/<file>.csv
```

`evaluate.py` builds the replay tool on first use. It needs a C++ compiler:
Windows → MSYS2 (`pacman -S mingw-w64-ucrt-x86_64-gcc`, add `C:\msys64\ucrt64\bin` to PATH);
macOS → `xcode-select --install`; Linux → `sudo apt install g++`.

Tune on recordings, not by re-falling: `--set name=value` tries a value, and `--sweep name=a,b,c`
compares several. When a value is better, put it in `applyTuning()` in `config.h`.
`python tools/evaluate.py --params` lists every parameter.

See **docs/TEST_PROTOCOL.md** for the scenario list, safety rules and how to split the data.

## 4. Connect to PIPER (MQTT)

| Topic | Direction | Retained | Content |
|---|---|---|---|
| `piper/sensor/fall01` | node → backend | yes | status: state, alert, reason, posture, present... |
| `piper/event/fall01` | node → backend | no | `descent`, `on_floor`, `stumble`, `fall`, `recovered`, `cleared` |
| `piper/availability/fall01` | Last Will | yes | `online` / `offline` |
| `piper/cmd/fall01` | backend → node | no | `{"action":"clear"\|"user_ok"\|"status","req_id":"..."}` |

On a `fall` event the backend should ask the person if they are OK, and **treat silence as
"needs help"**. `tools/fall_monitor.py` contains a ready `FallResponder` class for this; plug in
PIPER's speak/listen and the Telegram alert. Details are in **docs/MQTT.md**.

Without the board, run `python tools/virtual_fall_node.py` and type `fall` / `recover`.

## 5. Tests

```bash
python tests/run_tests.py          # 19 tests, ~4 s (the MQTT test is skipped)
python tests/run_tests.py --mqtt   # all 20, incl. MQTT round trip (Mosquitto on localhost:1883)
```

## What has been verified, and what has not

| Verified | How |
|---|---|
| Detection logic on 29 synthetic scenes (falls in 3 directions, struggling, slow slide, out of bed, ceiling mount; sitting, bending, bed, heaters, a cat, residual heat, walking away) | `tests/run_tests.py` |
| Same decisions at 4 fps and 8 fps | tests |
| Firmware code: compiles with no warnings and parses real GY-MCU90640 frame bytes; web endpoints return valid JSON; decisions match the replay tool on all 25 wall-mount scenes | host build of the sketch with Arduino stand-ins + the real PubSubClient library |
| MQTT: status, events, Last Will, closed-loop `clear` / `user_ok`, PIPER response flow | against a real Mosquitto broker |

**Not yet verified:** an ESP32 compile with the real toolchain, and **anything on real sensor
data**. Synthetic scenes prove the logic does what it was designed to do; they say nothing
about real-world accuracy. The thresholds are starting points. They must be checked and tuned
on recordings from your room (docs/TEST_PROTOCOL.md) before quoting accuracy in the report.
