// =====================================================================
//  The ORIGINAL fall-detection logic (before the redesign), ported
//  line-for-line from the first sketch so both versions can be replayed
//  on the same recordings and compared in the report.
//  Not used on the ESP32.
// =====================================================================
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

namespace legacy {

static const int W = 32, H = 24, N = W * H;

struct Detector {
  // thresholds exactly as in the original sketch
  float BODY_DELTA_C = 1.5f; int MIN_BODY_PIXELS = 6;
  float DROP_TRIGGER = 3.0f, AR_TRIGGER = 2.60f, ROW_TRIGGER = 12.0f;
  float AR_FALL = 2.80f, ROW_FLOOR = 12.0f; int BOTTOM_ROW_FLOOR = 15;
  float AR_UPRIGHT = 1.35f, ROW_UPRIGHT = 11.0f;
  uint32_t CONFIRM_MS = 3500, SUSPECT_TIMEOUT_MS = 10000, ALERT_HOLD_MS = 60000;

  enum State { STANDING, POTENTIAL_FALL, FALL_ALERT };
  State state = STANDING;
  bool fallDetected = false;
  float centroidY = 0, prevCentroidY = -1, aspectRatio = 0;
  uint32_t stateStart = 0, horizontalStart = 0;

  static int cmp(const void* a, const void* b) {
    float fa = *(const float*)a, fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
  }

  // returns 1 when an alert is raised on this frame, -1 when it clears, else 0
  int update(const float* frame, uint32_t now) {
    float sorted[N];
    memcpy(sorted, frame, sizeof(sorted));
    qsort(sorted, N, sizeof(float), cmp);
    float threshold = sorted[N / 2] + BODY_DELTA_C;
    int minX = 32, maxX = -1, minY = 24, maxY = -1, count = 0; float sumY = 0;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (frame[y * W + x] > threshold) {
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
          if (y < minY) minY = y;
          if (y > maxY) maxY = y;
          sumY += y; count++;
        }
    bool was = fallDetected;
    if (count < MIN_BODY_PIXELS) {
      aspectRatio = 0; centroidY = 0; prevCentroidY = -1; horizontalStart = 0;
      if (state == POTENTIAL_FALL) state = STANDING;
      if (state == FALL_ALERT && (now - stateStart > ALERT_HOLD_MS)) { state = STANDING; fallDetected = false; }
      return was && !fallDetected ? -1 : 0;
    }
    aspectRatio = (float)(maxX - minX + 1) / (float)(maxY - minY + 1);
    centroidY = sumY / count;
    float drop = (prevCentroidY < 0) ? 0 : (centroidY - prevCentroidY);
    prevCentroidY = centroidY;
    bool horizontalOnFloor = aspectRatio >= AR_FALL && centroidY >= ROW_FLOOR && maxY >= BOTTOM_ROW_FLOOR;
    switch (state) {
      case STANDING:
        if (drop >= DROP_TRIGGER || (aspectRatio >= AR_TRIGGER && centroidY >= ROW_TRIGGER)) {
          state = POTENTIAL_FALL; stateStart = now; horizontalStart = horizontalOnFloor ? now : 0;
        }
        break;
      case POTENTIAL_FALL:
        if (horizontalOnFloor) {
          if (horizontalStart == 0) horizontalStart = now;
          if (now - horizontalStart >= CONFIRM_MS) { state = FALL_ALERT; stateStart = now; fallDetected = true; }
        } else {
          horizontalStart = 0;
          if (aspectRatio < AR_UPRIGHT && centroidY < ROW_UPRIGHT) state = STANDING;
          else if (now - stateStart >= SUSPECT_TIMEOUT_MS) state = STANDING;
        }
        break;
      case FALL_ALERT:
        if (aspectRatio < AR_UPRIGHT && centroidY < ROW_UPRIGHT) { state = STANDING; horizontalStart = 0; fallDetected = false; }
        else if (now - stateStart >= ALERT_HOLD_MS) { state = STANDING; horizontalStart = 0; fallDetected = false; }
        break;
    }
    if (!was && fallDetected) return 1;
    if (was && !fallDetected) return -1;
    return 0;
  }
};

}  // namespace legacy
