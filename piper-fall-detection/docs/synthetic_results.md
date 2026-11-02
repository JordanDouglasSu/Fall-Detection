# Fall detection evaluation

Sessions: 29 from data/synthetic

> Includes SYNTHETIC sessions: these test the logic only, not real-world accuracy.

## Summary

| | Sessions | Sensitivity | Specificity | False alarms / h | Median latency | Max latency |
|---|---|---|---|---|---|---|
| This algorithm | 29 (11 falls) | 100% (11/11) | 100% (18/18) | 0.0 | 9.2 s | 37.8 s |
| Original algorithm | 29 (11 falls) | 27% (3/11) | 100% (18/18) | 0.0 | 4.2 s | 4.5 s |

Sensitivity = falls detected / falls. Specificity = normal sessions with no alert / normal sessions.
Latency = time from the marked fall (Enter pressed while recording) to the alert.

## Sessions

| Session | Label | Person | Expected | Result | Reason | Alert at | Original algorithm |
|---|---|---|---|---|---|---|---|
| bend_pick_up_4fps.csv | bend_pick_up | synthetic | none | TN | – | – | TN |
| bend_pick_up_8fps.csv | bend_pick_up | synthetic | none | TN | – | – | TN |
| cat_on_floor_4fps.csv | cat_on_floor | synthetic | none | TN | – | – | TN |
| ceiling_bed_4fps.csv | ceiling_bed | synthetic | none | TN | – | – | TN |
| ceiling_fall_4fps.csv | ceiling_fall | synthetic | alert | TP | descent_then_still | 18.5 s | **FN (missed)** |
| ceiling_sit_chair_4fps.csv | ceiling_sit_chair | synthetic | none | TN | – | – | TN |
| ceiling_walk_4fps.csv | ceiling_walk | synthetic | none | TN | – | – | TN |
| crouch_still_4fps.csv | crouch_still | synthetic | none | TN | – | – | TN |
| fall_away_from_camera_4fps.csv | fall_away_from_camera | synthetic | alert | TP | descent_then_still | 18.8 s | **FN (missed)** |
| fall_out_of_bed_4fps.csv | fall_out_of_bed | synthetic | alert | TP | long_lie | 53.2 s | **FN (missed)** |
| fall_sideways_4fps.csv | fall_sideways | synthetic | alert | TP | descent_then_still | 19.0 s | TP |
| fall_sideways_8fps.csv | fall_sideways | synthetic | alert | TP | descent_then_still | 18.8 s | TP |
| fall_struggling_4fps.csv | fall_struggling | synthetic | alert | TP | descent_then_on_floor | 26.0 s | **FN (missed)** |
| fall_then_recover_4fps.csv | fall_then_recover | synthetic | alert | TP | descent_then_still | 19.2 s | TP |
| fall_toward_camera_4fps.csv | fall_toward_camera | synthetic | alert | TP | descent_then_still | 18.8 s | **FN (missed)** |
| floor_exercise_4fps.csv | floor_exercise | synthetic | none | TN | – | – | TN |
| heater_on_floor_4fps.csv | heater_on_floor | synthetic | none | TN | – | – | TN |
| lie_on_bed_4fps.csv | lie_on_bed | synthetic | none | TN | – | – | TN |
| radiator_fall_4fps.csv | radiator_fall | synthetic | alert | TP | descent_then_still | 19.2 s | **FN (missed)** |
| radiator_walk_4fps.csv | radiator_walk | synthetic | none | TN | – | – | TN |
| residual_heat_4fps.csv | residual_heat | synthetic | none | TN | – | – | TN |
| sit_chair_4fps.csv | sit_chair | synthetic | none | TN | – | – | TN |
| slow_slide_4fps.csv | slow_slide | synthetic | alert | TP | long_lie | 47.8 s | **FN (missed)** |
| slow_slide_8fps.csv | slow_slide | synthetic | alert | TP | long_lie | 47.0 s | **FN (missed)** |
| stumble_get_up_4fps.csv | stumble_get_up | synthetic | none | TN | – | – | TN |
| stumble_get_up_8fps.csv | stumble_get_up | synthetic | none | TN | – | – | TN |
| walk_across_4fps.csv | walk_across | synthetic | none | TN | – | – | TN |
| walk_across_8fps.csv | walk_across | synthetic | none | TN | – | – | TN |
| walk_away_4fps.csv | walk_away | synthetic | none | TN | – | – | TN |

