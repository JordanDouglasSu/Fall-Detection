# Test protocol: collecting real fall data

Goal: honest sensitivity, specificity and false-alarm figures for the report, from recordings
the whole team can replay and re-tune on without anyone falling again.

## Safety first

- **Only team members fall.** Never ask older adults or anyone with dementia to simulate falls.
- Fall **only onto a thick mat or mattress** (≥ 10 cm). Clear the area of furniture edges.
- Always have a **spotter** standing by.
- Simulate falls in a **controlled** way: go down to a knee, then onto the side or front. No
  falling backwards, no falling from height, no head contact. A thermal camera only needs the
  *movement pattern*, not a realistic impact.
- Stop if anything hurts. Fewer, safer repetitions are fine.
- Ask your supervisor whether the module needs an ethics form for recording team members.
  The thermal data is 32×24 and anonymous, but ask anyway.

## Set-up

1. Mount the sensor **where it will be in the demo**, at the same height and angle. Moving it
   later means repainting zones and re-recording.
2. Paint the zones on the dashboard (README §2). Put the mattress in the floor zone.
3. Note the room temperature and the time of day in `--notes`.
4. Check the dashboard shows about 4 fps or more and a clean outline when someone stands in view.

## What to record

People A, B, C (team members). Each row is one session (`python tools/record.py ...`).
Press **Enter** at the moment of each fall.

| Label | Expect | Reps per person | What to do |
|---|---|---|---|
| `fall_sideways` | alert | 3 | fall across the camera's view, lie still 20 s |
| `fall_toward_camera` | alert | 3 | fall with feet or head toward the sensor, lie still 20 s |
| `fall_away_from_camera` | alert | 3 | fall away from the sensor, lie still 20 s |
| `fall_struggling` | alert | 2 | fall, then try to get up but stay on the floor for 25 s |
| `slow_slide` | alert | 2 | slide down slowly over about 5 s, lie still 40 s |
| `fall_out_of_bed` | alert | 2 | lie on bed, roll off onto the mat beside it, still 40 s |
| `stumble_get_up` | none | 2 | go down, get straight back up within about 3 s |
| `walk` | none | 2 | walk around for 1 min, toward and away from the sensor |
| `sit_chair` | none | 3 | sit down quickly, stay 1 min |
| `bend_pick_up` | none | 3 | pick something up from the floor |
| `crouch` | none | 2 | crouch and tie shoes / look under furniture, 15 s |
| `sit_floor` | none | 2 | sit on the floor on purpose, move around, 1 min |
| `lie_bed` | none | 2 | lie down on the bed, still 1 min |
| `two_people` | none | 2 | two people walking and sitting near each other |
| `hot_drink` | none | 1 | sit at a table with a hot drink for 2 min |
| `empty_room` | none | 1 | nobody in the room, heater on if you have one, 5 min |
| `daily_life` | none | 2 | 10–15 min of normal activity: tidy up, sit, walk, TV |

About 100 sessions with 3 people (`empty_room` and `two_people` only need doing once in total). That's roughly 3–4 hours of recording, most of it the `daily_life` sessions, spread over a few days.

`daily_life` and `empty_room` matter: they give **false alarms per hour**, which is what decides
whether a carer would keep the system switched on.

## Tune on some people, test on another

If you tune and test on the same recordings, the result is optimistic. Split by person:

```bash
# 1. tune on A and B only
python tools/evaluate.py data/real --persons A,B --sweep stillConfirmMs=4000,6000,8000
python tools/evaluate.py data/real --persons A,B --sweep fgDelta=1.0,1.3,1.6
#    ... put the chosen values in config.h applyTuning() and in your --set list
# 2. report on C only, untouched during tuning
python tools/evaluate.py data/real --persons C --compare --out results_test_C.md
```

With time, repeat with each person held out in turn (leave-one-person-out) and report the average.

## What to report

| | Falls | Sensitivity | Specificity | False alarms / h | Median latency |
|---|---|---|---|---|---|
| Original algorithm | | | | | |
| Improved algorithm | | | | | |

Plus:
- the per-scenario table from `evaluate.py` (which cases still fail, and why),
- one or two `plot_session.py` figures (a detected fall, and a near-miss case),
- the synthetic results only as a *logic check*, clearly labelled as such,
- limitations: number of people, one room, simulated falls by young adults, one sensor position.

## When a case fails

1. `python tools/plot_session.py data/real/<file>.csv` shows which stage didn't happen
   (no descent? not on floor? never still?).
2. Fix the cause before touching thresholds: zones wrong? Sensor angle? A heat source that needs
   an Ignore zone?
3. Only then sweep the one parameter that stage uses, and **check it doesn't break other sessions**.
