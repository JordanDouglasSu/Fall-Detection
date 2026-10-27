// =====================================================================
//  PIPER thermal fall detection — core algorithm
//
//  Pure C++ (no Arduino calls), so the SAME file runs on:
//    * the ESP32 (included by piper_fall_detect.ino), and
//    * the PC (compiled into replay/replay.cpp to re-run recorded sessions).
//  What the board decides and what the report's evaluation measures are
//  therefore guaranteed to be the same code.
//
//  Pipeline for each 32x24 frame:
//    1. Per-pixel background  ->  foreground = pixels warmer than their own background
//    2. Blob labelling        ->  keep the person, ignore small/static heat sources
//    3. Body features         ->  image moments: centroid, top, bottom, height, width,
//                                 elongation, fraction on the floor, motion
//    4. Posture               ->  UPRIGHT / LOW / FLOOR / REST (bed or sofa)
//    5. State machine         ->  MONITORING -> DESCENT -> ON_FLOOR -> FALL_ALERT
//                                 plus a separate "long lie" path
//  All timings are in milliseconds, so behaviour does not depend on frame rate.
// =====================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

namespace fall {

static const int W = 32;
static const int H = 24;
static const int N = W * H;
static const double PI_D = 3.14159265358979323846;

// ---- Room zones (one per pixel, set once per room) ------------------
//  '.' normal (wall, furniture tops, air)
//  'F' floor      — a body lying here counts as "on the floor"
//  'B' bed / sofa — lying here is resting, never a fall
//  'X' ignore     — radiator, TV, window in sun, kettle corner ...
enum Zone : uint8_t { Z_NORMAL = 0, Z_FLOOR = 1, Z_REST = 2, Z_IGNORE = 3 };

enum Mount : uint8_t { MOUNT_WALL = 0, MOUNT_CEILING = 1 };

enum Posture : uint8_t { P_NONE = 0, P_UPRIGHT, P_LOW, P_FLOOR, P_REST };

enum State : uint8_t {
  S_MONITORING = 0,  // normal
  S_DESCENT,         // fast downward movement seen, waiting to see where they end up
  S_ON_FLOOR,        // ended up on the floor after a descent, checking for recovery
  S_FALL_ALERT       // confirmed — latched until person is upright again or someone clears it
};

enum Reason : uint8_t {
  R_NONE = 0,
  R_DESCENT_STILL,    // fast descent -> on floor -> not moving
  R_DESCENT_FLOOR,    // fast descent -> still on floor after floorConfirmMs (even if moving)
  R_LONG_LIE,         // on the floor and still for longLieMs, no descent seen (slow slide)
  R_FLOOR_TOO_LONG,   // on the floor for floorMaxMs, moving or not
  R_LOST_ON_FLOOR,    // was on the floor after a descent, then disappeared from view
  R_CLEAR_RECOVERED,  // (clear reasons) person seen upright again
  R_CLEAR_MANUAL,     // caregiver / dashboard button
  R_CLEAR_USER_OK     // PIPER asked and the person said they are fine
};

// Event bits returned by update() for the frame on which they happen
enum Event : uint16_t {
  EV_DESCENT   = 1 << 0,  // fast descent detected
  EV_ON_FLOOR  = 1 << 1,  // landed on the floor after a descent
  EV_STUMBLE   = 1 << 2,  // descent + floor, but got up before confirmation (near-fall)
  EV_FALL      = 1 << 3,  // fall confirmed -> alert raised
  EV_RECOVERED = 1 << 4,  // alert ended because the person is upright again
  EV_CLEARED   = 1 << 5,  // alert ended by command (manual / user_ok)
  EV_PERSON_IN = 1 << 6,
  EV_PERSON_OUT= 1 << 7
};

// ---- Tuning ---------------------------------------------------------
// Defaults are starting points. Tune them on YOUR recordings with
// tools/evaluate.py (it accepts --set name=value and --sweep).
struct Config {
  uint8_t  mount           = MOUNT_WALL;

  // Foreground / background
  float    fgDelta         = 1.3f;    // °C above a pixel's own background = body
  int      minBlobPixels   = 6;       // smaller warm blobs are ignored
  int      mergeGap        = 2;       // join body fragments within this many pixels
  float    bgAlpha         = 0.05f;   // learning rate for empty pixels (per frame)
  float    staticAlpha     = 0.01f;   // absorb heat sources that never move
  float    moveToTrack     = 2.0f;    // px of net movement before a blob counts as a person that moved

  // Posture (wall mount)
  float    floorFracMin    = 0.60f;   // share of body pixels on floor zone to count as on floor
  float    floorFracSure   = 0.85f;   // ... this share counts even if the blob looks tall
  float    restFracMin     = 0.50f;   // share on bed/sofa zone to count as resting
  float    uprightRatio    = 1.20f;   // body height / width at or above this = upright

  // Descent (wall mount): top of body drops fast while feet stay put
  uint32_t descentWindowMs = 1000;    // drop must happen within this time (falls take < ~1 s)
  float    descentMinRows  = 3.0f;    // top of body must drop at least this many rows
  float    descentMinFrac  = 0.40f;   // ... and at least this fraction of body height
  float    descentShrink   = 0.75f;   // body height must shrink to <= this ratio
  float    bottomRiseMax   = 2.0f;    // bottom edge may rise at most this (walking away raises it)

  // Ceiling mount (top-down view)
  float    ceilAreaGrowth  = 1.6f;    // area grows by this factor within the window = descent
  float    ceilLyingElong  = 1.8f;    // elongation at or above this = lying
  float    ceilLyingArea   = 1.8f;    // area >= this x upright area = lying
  float    ceilUprightArea = 12.0f;   // starting guess of upright area (pixels), learned online

  // Stillness
  float    stillMotion     = 0.15f;   // °C mean change per pixel (above sensor noise) = still
  uint32_t motionTauMs     = 700;     // smoothing time constant for motion

  // Timers (ms)
  uint32_t postureHoldMs   = 400;     // a new posture must persist this long
  uint32_t impactWindowMs  = 3000;    // after a descent, must reach the floor within this
  uint32_t stillConfirmMs  = 6000;    // on floor after descent + still this long -> FALL
  uint32_t floorConfirmMs  = 15000;   // on floor after descent this long (moving or not) -> FALL
  uint32_t longLieMs       = 30000;   // on floor + still this long, no descent needed -> FALL
  uint32_t floorMaxMs      = 120000;  // on floor this long, moving or not -> FALL (0 = off)
  uint32_t recoverMs       = 3000;    // upright this long -> recovered
  uint32_t missingMs       = 2000;    // blob gone this long -> person gone
  uint32_t lostConfirmMs   = 10000;   // gone this long after landing on floor -> FALL

  uint32_t featureTauMs    = 250;     // smoothing time constant for body features (0 = none)
};

// Named access to tuning values (used by the PC tools and the /config page)
struct ParamDef { const char* name; char type; size_t offset; };  // type: 'f' float, 'u' uint32, 'i' int, 'b' uint8
#define FALL_P(t, n) { #n, t, offsetof(Config, n) }
static const ParamDef PARAMS[] = {
  FALL_P('b', mount),
  FALL_P('f', fgDelta), FALL_P('i', minBlobPixels), FALL_P('i', mergeGap),
  FALL_P('f', bgAlpha), FALL_P('f', staticAlpha), FALL_P('f', moveToTrack),
  FALL_P('f', floorFracMin), FALL_P('f', floorFracSure), FALL_P('f', restFracMin), FALL_P('f', uprightRatio),
  FALL_P('u', descentWindowMs), FALL_P('f', descentMinRows), FALL_P('f', descentMinFrac),
  FALL_P('f', descentShrink), FALL_P('f', bottomRiseMax),
  FALL_P('f', ceilAreaGrowth), FALL_P('f', ceilLyingElong), FALL_P('f', ceilLyingArea), FALL_P('f', ceilUprightArea),
  FALL_P('f', stillMotion), FALL_P('u', motionTauMs),
  FALL_P('u', postureHoldMs), FALL_P('u', impactWindowMs), FALL_P('u', stillConfirmMs),
  FALL_P('u', floorConfirmMs), FALL_P('u', longLieMs), FALL_P('u', floorMaxMs),
  FALL_P('u', recoverMs), FALL_P('u', missingMs), FALL_P('u', lostConfirmMs),
  FALL_P('u', featureTauMs),
};
#undef FALL_P
static const int NUM_PARAMS = sizeof(PARAMS) / sizeof(PARAMS[0]);

inline bool setParam(Config& c, const char* name, double v) {
  for (int i = 0; i < NUM_PARAMS; i++) {
    if (strcmp(PARAMS[i].name, name) != 0) continue;
    char* p = reinterpret_cast<char*>(&c) + PARAMS[i].offset;
    switch (PARAMS[i].type) {
      case 'f': *reinterpret_cast<float*>(p)    = (float)v; break;
      case 'u': *reinterpret_cast<uint32_t*>(p) = (uint32_t)(v < 0 ? 0 : v); break;
      case 'i': *reinterpret_cast<int*>(p)      = (int)v; break;
      case 'b': *reinterpret_cast<uint8_t*>(p)  = (uint8_t)v; break;
    }
    return true;
  }
  return false;
}

inline double getParam(const Config& c, int i) {
  const char* p = reinterpret_cast<const char*>(&c) + PARAMS[i].offset;
  switch (PARAMS[i].type) {
    case 'f': return *reinterpret_cast<const float*>(p);
    case 'u': return *reinterpret_cast<const uint32_t*>(p);
    case 'i': return *reinterpret_cast<const int*>(p);
    case 'b': return *reinterpret_cast<const uint8_t*>(p);
  }
  return 0;
}

// Parse a room mask: 24 strings of 32 chars ('.', 'F', 'B', 'X').
// Missing/short rows are treated as '.'.
inline void parseMask(const char* const rows[H], uint8_t zone[N]) {
  for (int y = 0; y < H; y++) {
    const char* r = rows ? rows[y] : nullptr;
    size_t len = r ? strlen(r) : 0;
    for (int x = 0; x < W; x++) {
      char ch = (x < (int)len) ? r[x] : '.';
      zone[y * W + x] = (ch == 'F' || ch == 'f') ? Z_FLOOR
                      : (ch == 'B' || ch == 'b') ? Z_REST
                      : (ch == 'X' || ch == 'x') ? Z_IGNORE : Z_NORMAL;
    }
  }
}

// Same, from one 768-char string (row by row). Used by the PC tools.
inline void parseMaskFlat(const char* flat, uint8_t zone[N]) {
  size_t len = flat ? strlen(flat) : 0;
  for (int i = 0; i < N; i++) {
    char ch = (i < (int)len) ? flat[i] : '.';
    zone[i] = (ch == 'F' || ch == 'f') ? Z_FLOOR
            : (ch == 'B' || ch == 'b') ? Z_REST
            : (ch == 'X' || ch == 'x') ? Z_IGNORE : Z_NORMAL;
  }
}

inline char zoneChar(uint8_t z) { return z == Z_FLOOR ? 'F' : z == Z_REST ? 'B' : z == Z_IGNORE ? 'X' : '.'; }

// ---- Per-frame body features ------------------------------------------
struct Features {
  bool  present = false;   // a person blob was found this frame
  int   pixels = 0;        // blob size
  float cx = 0, cy = 0;    // centroid (column, row)
  float top = 0, bottom = 0, height = 0, width = 0;  // moment-based extents (rows/cols)
  float ratio = 0;         // height / width
  float elong = 1;         // major / minor axis (1 = round)
  float angleDeg = 0;      // major axis angle: 0 = horizontal, 90 = vertical
  float floorFrac = 0;     // share of blob pixels on floor zone
  float restFrac = 0;      // share on bed/sofa zone
  float motion = 0;        // smoothed mean temp change on the body, minus sensor noise
  float noise = 0;         // mean temp change on background pixels (sensor noise level)
  int   bx1 = -1, by1 = -1, bx2 = -1, by2 = -1;  // bounding box (for drawing only)
  float background = 0;    // median background temperature (for display)
};

// ---- Detector ---------------------------------------------------------------
class Detector {
 public:
  Config cfg;
  uint8_t zone[N];

  // Read-only outputs for display / logging
  Features f;
  Posture posture = P_NONE;
  State   state = S_MONITORING;
  Reason  alertReason = R_NONE;   // why the current/last alert was raised
  Reason  clearReason = R_NONE;   // why the last alert ended
  bool    alert = false;
  uint32_t alertSince = 0;
  uint32_t eventCount = 0;        // increments on every fall alert
  bool    still = false;
  uint8_t personMask[N];          // 1 = pixel belongs to the tracked person

  Detector() { memset(zone, Z_NORMAL, sizeof(zone)); reset(); }

  void reset() {
    haveBg = false; haveFrame = false;
    memset(personMask, 0, sizeof(personMask));
    memset(prevPersonMask, 0, sizeof(prevPersonMask));
    f = Features();
    posture = P_NONE; candPosture = P_NONE; candSet = false;
    state = S_MONITORING; alert = false; alertReason = R_NONE; clearReason = R_NONE;
    histLen = 0; histHead = 0;
    trackAlive = false; trackMoved = false; trackPersonLike = false; originX = originY = 0; lastT = 0;
    memset(lastSeenMask, 0, sizeof(lastSeenMask)); lastSeenPixels = 0; chosenOverlap = 0;
    lastSeenSet = false; motionEma = 0; still = false;
    stillSet = uprightSet = floorSet = false;
    upArea = cfg.ceilUprightArea;
  }

  // Caregiver / dashboard / PIPER "I'm fine" -> end the alert
  uint16_t clear(uint32_t now, Reason why) {
    if (!alert && state == S_MONITORING) return 0;
    alert = false; state = S_MONITORING; clearReason = why;
    // Restart the floor/still timers so the same lie does not re-alert at once.
    // If the person stays down, long-lie / floor-too-long will alert again later.
    if (floorSet) floorSince = now;
    if (stillSet) stillSince = now;
    return EV_CLEARED;
  }

  // Process one frame (°C, row-major 32x24). Returns event bits.
  uint16_t update(const float* frame, uint32_t now) {
    uint16_t ev = 0;
    if (!haveBg) initBackground(frame);

    // 1) foreground + noise estimate ---------------------------------------
    float noiseSum = 0; int noiseN = 0;
    for (int i = 0; i < N; i++) {
      fg[i] = (zone[i] != Z_IGNORE) && (frame[i] - bg[i] > cfg.fgDelta);
    }

    // 2) blobs ---------------------------------------------------------------
    int nBlobs = labelBlobs();
    int chosen = choosePerson(nBlobs);

    memset(personMask, 0, sizeof(personMask));
    if (chosen >= 0) {
      int bx1 = blobX1[chosen] - cfg.mergeGap, bx2 = blobX2[chosen] + cfg.mergeGap;
      int by1 = blobY1[chosen] - cfg.mergeGap, by2 = blobY2[chosen] + cfg.mergeGap;
      for (int b = 0; b < nBlobs; b++) {
        bool join = (b == chosen) ||
                    (blobX2[b] >= bx1 && blobX1[b] <= bx2 && blobY2[b] >= by1 && blobY1[b] <= by2);
        if (!join) continue;
        for (int i = 0; i < N; i++) if (label[i] == b + 1) personMask[i] = 1;
      }
    }

    // motion: mean |frame change| on body (now or last frame) minus mean change on background
    float bodySum = 0; int bodyN = 0;
    if (haveFrame) {
      for (int i = 0; i < N; i++) {
        if (zone[i] == Z_IGNORE) continue;
        float d = fabsf(frame[i] - prev[i]);
        if (personMask[i] || prevPersonMask[i]) { bodySum += d; bodyN++; }
        else if (!fg[i]) { noiseSum += d; noiseN++; }
      }
    }
    float noise = noiseN ? noiseSum / noiseN : 0;
    float rawMotion = bodyN ? (bodySum / bodyN - noise) : 0;
    if (rawMotion < 0) rawMotion = 0;

    // 3) background update ---------------------------------------------------
    updateBackground(frame);

    // 4) features ------------------------------------------------------------
    Features nf;
    nf.noise = noise;
    nf.background = bgMedian;
    bool present = computeFeatures(nf);

    // Is this the same object we were following? A blob that doesn't overlap
    // where the person was last seen, or is suddenly a third of the size
    // (person walked off, leaving a warm seat or a pet behind), is a new track.
    bool switched = present && trackAlive &&
                    (chosenOverlap == 0 || nf.pixels < 0.35f * (float)lastSeenPixels);

    if (present) {
      if (!trackAlive || switched) {   // new track
        if (!trackAlive) ev |= EV_PERSON_IN;
        trackAlive = true; trackMoved = false; trackPersonLike = false;
        f = nf; f.motion = 0; motionEma = 0;
        originX = nf.cx; originY = nf.cy;
        histLen = 0;
      } else {
        float a = smoothK(now, cfg.featureTauMs);
        f.present = true; f.pixels = nf.pixels;
        f.cx = lerp(f.cx, nf.cx, a);         f.cy = lerp(f.cy, nf.cy, a);
        f.top = lerp(f.top, nf.top, a);      f.bottom = lerp(f.bottom, nf.bottom, a);
        f.height = lerp(f.height, nf.height, a); f.width = lerp(f.width, nf.width, a);
        f.elong = lerp(f.elong, nf.elong, a);
        f.angleDeg = nf.angleDeg;
        f.ratio = f.width > 0.01f ? f.height / f.width : 0;
        f.floorFrac = lerp(f.floorFrac, nf.floorFrac, a);
        f.restFrac = lerp(f.restFrac, nf.restFrac, a);
        f.bx1 = nf.bx1; f.by1 = nf.by1; f.bx2 = nf.bx2; f.by2 = nf.by2;
        f.noise = nf.noise; f.background = nf.background;
      }
      // net movement since the track appeared (noise jitter does not add up)
      if (!trackMoved) {
        float mx = f.cx - originX, my = f.cy - originY;
        if (sqrtf(mx * mx + my * my) >= cfg.moveToTrack) trackMoved = true;
      }
      motionEma = lerp(motionEma, rawMotion, smoothK(now, cfg.motionTauMs));
      f.motion = motionEma;
      lastSeen = now; lastSeenSet = true;
      memcpy(lastSeenMask, personMask, sizeof(lastSeenMask));
      lastSeenPixels = nf.pixels;
      pushHistory(now);
    } else {
      f.present = false;
      f.noise = nf.noise; f.background = nf.background;
      if (trackAlive && lastSeenSet && (now - lastSeen) >= cfg.missingMs) {
        trackAlive = false;
        ev |= EV_PERSON_OUT;
        histLen = 0;
        f = Features(); f.noise = nf.noise; f.background = nf.background;
      }
    }

    // stillness
    bool nowStill = trackAlive && (motionEma < cfg.stillMotion);
    if (nowStill && !stillSet) { stillSet = true; stillSince = now; }
    if (!nowStill) stillSet = false;
    still = nowStill;

    // 5) posture with hold time ----------------------------------------------
    Posture raw = trackAlive ? (present ? classify() : posture) : P_NONE;
    // Only something that has looked like a person (standing, or in bed) can
    // raise a long-lie alert — a cat or a warm bag on the floor never has.
    if (present && (raw == P_UPRIGHT || raw == P_REST)) trackPersonLike = true;
    if (raw == P_NONE || posture == P_NONE) {
      posture = raw; candSet = false;
    } else if (raw != posture) {
      if (!candSet || candPosture != raw) { candPosture = raw; candSince = now; candSet = true; }
      else if (now - candSince >= cfg.postureHoldMs) { posture = raw; candSet = false; }
    } else {
      candSet = false;
    }

    // ceiling: learn upright area while walking about
    if (cfg.mount == MOUNT_CEILING && present && posture == P_UPRIGHT && !nowStill) {
      upArea = lerp(upArea, (float)f.pixels, 0.05f);
    }

    // timers for upright / floor
    if (posture == P_UPRIGHT) { if (!uprightSet) { uprightSet = true; uprightSince = now; } }
    else uprightSet = false;
    if (posture == P_FLOOR) { if (!floorSet) { floorSet = true; floorSince = now; } }
    else floorSet = false;

    // 6) state machine ---------------------------------------------------------
    bool descent = present && detectDescent(now);
    switch (state) {
      case S_MONITORING:
        if (descent) {
          state = S_DESCENT; stateSince = now; ev |= EV_DESCENT;
        } else if (floorSet && trackMoved && trackPersonLike) {
          if (stillSet && (now - stillSince) >= cfg.longLieMs && (now - floorSince) >= cfg.longLieMs)
            ev |= raise(now, R_LONG_LIE);
          else if (cfg.floorMaxMs && (now - floorSince) >= cfg.floorMaxMs)
            ev |= raise(now, R_FLOOR_TOO_LONG);
        }
        break;

      case S_DESCENT:
        if (posture == P_FLOOR) {
          state = S_ON_FLOOR; stateSince = now; ev |= EV_ON_FLOOR;
        } else if (posture == P_REST || (now - stateSince) >= cfg.impactWindowMs) {
          state = S_MONITORING;          // sat down / lay on bed / bent over — not a fall
        }
        break;

      case S_ON_FLOOR:
        if (!trackAlive) {
          if (lastSeenSet && (now - lastSeen) >= cfg.lostConfirmMs) ev |= raise(now, R_LOST_ON_FLOOR);
        } else if (uprightSet && (now - uprightSince) >= cfg.recoverMs) {
          state = S_MONITORING; ev |= EV_STUMBLE;   // got up by themselves: near-fall
        } else if (posture == P_REST) {
          state = S_MONITORING;
        } else if (stillSet && (now - stillSince) >= cfg.stillConfirmMs &&
                   (now - stateSince) >= cfg.stillConfirmMs / 2) {
          ev |= raise(now, R_DESCENT_STILL);
        } else if ((now - stateSince) >= cfg.floorConfirmMs) {
          ev |= raise(now, R_DESCENT_FLOOR);
        }
        break;

      case S_FALL_ALERT:
        // Latched. Only ends when the person is clearly upright again, or by clear().
        if (trackAlive && uprightSet && (now - uprightSince) >= cfg.recoverMs) {
          alert = false; state = S_MONITORING; clearReason = R_CLEAR_RECOVERED;
          ev |= EV_RECOVERED;
        }
        break;
    }

    // keep frame for next motion estimate
    lastT = now;
    memcpy(prev, frame, sizeof(prev));
    memcpy(prevPersonMask, personMask, sizeof(prevPersonMask));
    haveFrame = true;
    return ev;
  }

  const float* backgroundPtr() const { return bg; }

 private:
  // background
  float bg[N];
  float prev[N];
  float bgMedian = 25.0f;
  bool  haveBg = false, haveFrame = false;
  uint8_t fg[N];
  uint8_t prevPersonMask[N];
  uint8_t protect[N];
  float   scratch[N];

  // blobs
  static const int MAXB = 32;
  uint8_t label[N];                // 0 = none, else blob index + 1
  int16_t stackBuf[N];
  int blobSize[MAXB], blobX1[MAXB], blobX2[MAXB], blobY1[MAXB], blobY2[MAXB];

  // track
  bool  trackAlive = false, trackMoved = false, trackPersonLike = false;
  uint8_t lastSeenMask[N];       // where the tracked person was last seen (identity)
  int   lastSeenPixels = 0;
  int   chosenOverlap = 0;
  float originX = 0, originY = 0;
  uint32_t lastT = 0;
  uint32_t lastSeen = 0; bool lastSeenSet = false;
  float motionEma = 0;
  float upArea = 12.0f;

  // posture/timers
  Posture candPosture = P_NONE; uint32_t candSince = 0; bool candSet = false;
  uint32_t stateSince = 0;
  uint32_t stillSince = 0;   bool stillSet = false;
  uint32_t uprightSince = 0; bool uprightSet = false;
  uint32_t floorSince = 0;   bool floorSet = false;

  // history for descent detection (time-based window)
  struct Hist { uint32_t t; float top, bottom, height; int pixels; };
  static const int HMAX = 64;
  Hist hist[HMAX];
  int histLen = 0, histHead = 0;

  static float lerp(float a, float b, float t) { return a + (b - a) * t; }

  // Exponential smoothing factor for the time since the last frame, so the
  // same time constant gives the same behaviour at 4 fps or 16 fps.
  float smoothK(uint32_t now, uint32_t tauMs) const {
    if (tauMs == 0 || !haveFrame) return 1.0f;
    float dt = (float)(now - lastT);
    if (dt <= 0) return 1.0f;
    return 1.0f - expf(-dt / (float)tauMs);
  }

  uint16_t raise(uint32_t now, Reason r) {
    state = S_FALL_ALERT; alert = true; alertReason = r; alertSince = now;
    clearReason = R_NONE; eventCount++;
    return EV_FALL;
  }

  void initBackground(const float* frame) {
    // start from the frame median everywhere; empty pixels converge to their
    // true value within a few seconds, the person is never learned in.
    memcpy(scratch, frame, sizeof(scratch));
    bgMedian = median(scratch, N);
    for (int i = 0; i < N; i++) bg[i] = bgMedian;
    haveBg = true;
  }

  static float median(float* a, int n) {
    // nth_element-style quickselect (no <algorithm> on purpose)
    int k = n / 2, lo = 0, hi = n - 1;
    while (lo < hi) {
      float pivot = a[(lo + hi) / 2];
      int i = lo, j = hi;
      while (i <= j) {
        while (a[i] < pivot) i++;
        while (a[j] > pivot) j--;
        if (i <= j) { float t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
      }
      if (k <= j) hi = j; else if (k >= i) lo = i; else break;
    }
    return a[k];
  }

  void updateBackground(const float* frame) {
    // protect the person (dilated by 1 px) from being learned into the background
    memset(protect, 0, sizeof(protect));
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        if (!personMask[y * W + x]) continue;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && xx < W && yy >= 0 && yy < H) protect[yy * W + xx] = 1;
          }
      }
    bool personStatic = trackAlive && !trackMoved;   // e.g. a radiator picked up as "person"
    for (int i = 0; i < N; i++) {
      if (protect[i]) {
        if (personStatic) bg[i] += cfg.staticAlpha * (frame[i] - bg[i]);
      } else if (fg[i]) {
        bg[i] += cfg.staticAlpha * (frame[i] - bg[i]);   // other warm blobs: absorb slowly
      } else {
        bg[i] += cfg.bgAlpha * (frame[i] - bg[i]);       // empty: follow the room
      }
    }
    // display value: median of background
    memcpy(scratch, bg, sizeof(scratch));
    bgMedian = median(scratch, N);
  }

  int labelBlobs() {
    memset(label, 0, sizeof(label));
    int nb = 0;
    for (int s = 0; s < N; s++) {
      if (!fg[s] || label[s]) continue;
      if (nb >= MAXB) break;
      int id = nb + 1, sp = 0, size = 0;
      int x1 = W, x2 = -1, y1 = H, y2 = -1;
      stackBuf[sp++] = (int16_t)s; label[s] = (uint8_t)id;
      while (sp) {
        int p = stackBuf[--sp];
        int px = p % W, py = p / W;
        size++;
        if (px < x1) x1 = px;
        if (px > x2) x2 = px;
        if (py < y1) y1 = py;
        if (py > y2) y2 = py;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int xx = px + dx, yy = py + dy;
            if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
            int q = yy * W + xx;
            if (fg[q] && !label[q]) { label[q] = (uint8_t)id; stackBuf[sp++] = (int16_t)q; }
          }
      }
      blobSize[nb] = size; blobX1[nb] = x1; blobX2[nb] = x2; blobY1[nb] = y1; blobY2[nb] = y2;
      nb++;
    }
    return nb;
  }

  int choosePerson(int nb) {
    // Following a person who has moved: keep the blob that overlaps where they
    // were last seen. Otherwise (nobody yet, or only a static warm thing so far):
    // take the largest blob, so a radiator can't hide someone walking in.
    bool sticky = trackAlive && trackMoved;
    int best = -1, bestOverlap = -1, bestSize = 0;
    for (int b = 0; b < nb; b++) {
      if (blobSize[b] < cfg.minBlobPixels) continue;
      int overlap = 0;
      for (int i = 0; i < N; i++) if (label[i] == b + 1 && lastSeenMask[i]) overlap++;
      bool better = sticky ? (overlap > bestOverlap || (overlap == bestOverlap && blobSize[b] > bestSize))
                           : (blobSize[b] > bestSize);
      if (better) { best = b; bestOverlap = overlap; bestSize = blobSize[b]; }
    }
    chosenOverlap = best >= 0 ? bestOverlap : 0;
    return best;
  }

  bool computeFeatures(Features& o) {
    int n = 0, nFloor = 0, nRest = 0;
    double sx = 0, sy = 0;
    int x1 = W, x2 = -1, y1 = H, y2 = -1;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        int i = y * W + x;
        if (!personMask[i]) continue;
        n++; sx += x; sy += y;
        if (zone[i] == Z_FLOOR) nFloor++;
        else if (zone[i] == Z_REST) nRest++;
        if (x < x1) x1 = x;
        if (x > x2) x2 = x;
        if (y < y1) y1 = y;
        if (y > y2) y2 = y;
      }
    if (n < cfg.minBlobPixels) { o.present = false; return false; }
    double mx = sx / n, my = sy / n;
    double cxx = 0, cyy = 0, cxy = 0;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        if (!personMask[y * W + x]) continue;
        double dx = x - mx, dy = y - my;
        cxx += dx * dx; cyy += dy * dy; cxy += dx * dy;
      }
    // +1/12 = variance of a 1-pixel-wide cell, keeps thin blobs from collapsing to 0
    cxx = cxx / n + 1.0 / 12; cyy = cyy / n + 1.0 / 12; cxy /= n;
    double tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
    double disc = sqrt(fmax(0.0, tr * tr / 4 - det));
    double l1 = tr / 2 + disc, l2 = fmax(1e-6, tr / 2 - disc);
    // For a uniform bar of length L, std = L/sqrt(12): extent = sqrt(12)*std ≈ 3.46*std
    const double K = 3.4641;
    double sdy = sqrt(cyy), sdx = sqrt(cxx);
    o.present = true;
    o.pixels = n;
    o.cx = (float)mx; o.cy = (float)my;
    o.height = (float)(K * sdy); o.width = (float)(K * sdx);
    o.top = (float)(my - K / 2 * sdy); o.bottom = (float)(my + K / 2 * sdy);
    o.ratio = o.width > 0.01f ? o.height / o.width : 0;
    o.elong = (float)sqrt(l1 / l2);
    o.angleDeg = (float)(fabs(0.5 * atan2(2 * cxy, cxx - cyy)) * 180.0 / PI_D);
    o.floorFrac = (float)nFloor / n;
    o.restFrac = (float)nRest / n;
    o.bx1 = x1; o.bx2 = x2; o.by1 = y1; o.by2 = y2;
    return true;
  }

  Posture classify() const {
    if (f.restFrac >= cfg.restFracMin) return P_REST;
    if (cfg.mount == MOUNT_CEILING) {
      bool lying = (f.elong >= cfg.ceilLyingElong) ||
                   (upArea > 0 && f.pixels >= cfg.ceilLyingArea * upArea);
      if (lying && f.floorFrac >= cfg.floorFracMin) return P_FLOOR;
      return lying ? P_LOW : P_UPRIGHT;
    }
    // Wall mount: a body lying on the floor only covers floor pixels;
    // a standing body also covers the wall behind it.
    bool onFloor = f.floorFrac >= cfg.floorFracMin &&
                   (f.ratio < cfg.uprightRatio || f.floorFrac >= cfg.floorFracSure);
    if (onFloor) return P_FLOOR;
    if (f.ratio >= cfg.uprightRatio) return P_UPRIGHT;
    return P_LOW;
  }

  void pushHistory(uint32_t now) {
    Hist& h = hist[histHead];
    h.t = now; h.top = f.top; h.bottom = f.bottom; h.height = f.height; h.pixels = f.pixels;
    histHead = (histHead + 1) % HMAX;
    if (histLen < HMAX) histLen++;
  }

  bool detectDescent(uint32_t now) const {
    if (histLen < 2) return false;
    const Hist& cur = hist[(histHead - 1 + HMAX) % HMAX];
    for (int k = 1; k < histLen; k++) {
      const Hist& h = hist[(histHead - 1 - k + 2 * HMAX) % HMAX];
      if (now - h.t > cfg.descentWindowMs) break;
      if (cfg.mount == MOUNT_CEILING) {
        if (h.pixels > 0 && cur.pixels >= cfg.ceilAreaGrowth * h.pixels) return true;
        continue;
      }
      float drop = cur.top - h.top;               // +ve = top moved down
      if (drop < cfg.descentMinRows) continue;
      if (h.height <= 0.5f) continue;
      if (drop < cfg.descentMinFrac * h.height) continue;
      if (cur.height > cfg.descentShrink * h.height) continue;
      if (h.bottom - cur.bottom > cfg.bottomRiseMax) continue;   // feet moved up: walking away
      return true;
    }
    return false;
  }
};

inline const char* postureName(Posture p) {
  switch (p) { case P_UPRIGHT: return "upright"; case P_LOW: return "low";
               case P_FLOOR: return "floor"; case P_REST: return "rest"; default: return "none"; }
}
inline const char* stateName(State s) {
  switch (s) { case S_DESCENT: return "DESCENT"; case S_ON_FLOOR: return "ON_FLOOR";
               case S_FALL_ALERT: return "FALL_ALERT"; default: return "MONITORING"; }
}
inline const char* reasonName(Reason r) {
  switch (r) {
    case R_DESCENT_STILL: return "descent_then_still";
    case R_DESCENT_FLOOR: return "descent_then_on_floor";
    case R_LONG_LIE: return "long_lie";
    case R_FLOOR_TOO_LONG: return "floor_too_long";
    case R_LOST_ON_FLOOR: return "lost_from_view_on_floor";
    case R_CLEAR_RECOVERED: return "recovered";
    case R_CLEAR_MANUAL: return "manual";
    case R_CLEAR_USER_OK: return "user_ok";
    default: return "none";
  }
}

}  // namespace fall
