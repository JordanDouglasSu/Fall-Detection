// =====================================================================
//  replay — run the ESP32 fall-detection algorithm on recorded sessions
//
//  Uses firmware/piper_fall_detect/fall_algo.h unchanged, so results here
//  are what the board would have decided on the same frames.
//
//  Build:   g++ -std=c++17 -O2 -I../firmware/piper_fall_detect replay.cpp -o replay
//           (tools/evaluate.py builds it automatically)
//
//  Usage:   replay [options] session.csv [session2.csv ...]
//    --set name=value     override a tuning parameter (repeatable)
//    --mount wall|ceiling override the mount from the session file
//    --mask file.txt      override the room mask (24 lines of 32 chars)
//    --trace out.csv      write per-frame features/state (first session only)
//    --algo new|legacy    legacy = the original sketch's logic, for comparison
//    --params             list tuning parameters and defaults, then exit
//
//  Output: one JSON line per session on stdout.
// =====================================================================
#include "fall_algo.h"
#include "legacy_algo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace fall;

struct Override { std::string name; double value; };

static std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

static bool loadMaskFile(const std::string& path, std::string& flat) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line; flat.clear();
  int rows = 0;
  while (std::getline(in, line) && rows < H) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    line.resize(W, '.');
    flat += line; rows++;
  }
  flat.resize(N, '.');
  return true;
}

struct EventRec { uint32_t t; std::string name; std::string reason; };

static bool g_legacy = false;

static int runSession(const std::string& path, const std::vector<Override>& ov,
                      const std::string& mountOv, const std::string& maskOv,
                      const std::string& tracePath) {
  std::ifstream in(path);
  if (!in) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }

  std::string label, expect, person, mount = "wall", mask;
  std::vector<uint32_t> marks;
  Detector* d = new Detector();   // large object: keep it off the stack
  legacy::Detector* old = new legacy::Detector();

  std::ofstream trace;
  if (!tracePath.empty()) {
    trace.open(tracePath);
    trace << "t_ms,present,pixels,cx,cy,top,bottom,height,width,ratio,elong,floor_frac,rest_frac,"
             "motion,noise,still,posture,state,alert,events\n";
  }

  std::string line;
  std::vector<float> frame(N);
  std::vector<EventRec> events;
  uint32_t firstAlert = 0; bool alerted = false; std::string firstReason;
  size_t frames = 0;
  bool configured = false;

  auto configure = [&]() {
    if (!mountOv.empty()) mount = mountOv;
    d->cfg = Config();
    d->cfg.mount = (mount == "ceiling") ? MOUNT_CEILING : MOUNT_WALL;
    for (const auto& o : ov) {
      if (!setParam(d->cfg, o.name.c_str(), o.value)) {
        fprintf(stderr, "unknown parameter: %s (use --params)\n", o.name.c_str());
        exit(2);
      }
    }
    std::string m = !maskOv.empty() ? maskOv : mask;
    parseMaskFlat(m.c_str(), d->zone);
    d->reset();
    configured = true;
  };

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (line[0] == '#') {
      std::string kv = line.substr(1);
      while (!kv.empty() && kv[0] == ' ') kv.erase(0, 1);
      size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      if (k == "label") label = v;
      else if (k == "expect") expect = v;
      else if (k == "person") person = v;
      else if (k == "mount") mount = v;
      else if (k == "mask") mask = v;
      else if (k == "mark") marks.push_back((uint32_t)strtoul(v.c_str(), nullptr, 10));
      continue;
    }
    if (line[0] == 't') continue;  // header row
    if (!configured) configure();

    // t_ms,fid,p0..p767
    const char* p = line.c_str();
    char* end;
    uint32_t t = (uint32_t)strtoul(p, &end, 10); p = end; if (*p == ',') p++;
    strtoul(p, &end, 10); p = end; if (*p == ',') p++;   // fid (unused)
    int i = 0;
    for (; i < N && *p; i++) {
      frame[i] = strtof(p, &end);
      if (end == p) break;
      p = end; if (*p == ',') p++;
    }
    if (i < N) { fprintf(stderr, "%s: short frame at t=%u (%d values)\n", path.c_str(), t, i); continue; }

    frames++;
    if (g_legacy) {
      int r = old->update(frame.data(), t);
      if (r > 0) {
        events.push_back({t, "fall", "legacy"});
        if (!alerted) { alerted = true; firstAlert = t; firstReason = "legacy"; }
      } else if (r < 0) {
        events.push_back({t, "alert_ended", ""});
      }
      continue;
    }
    uint16_t ev = d->update(frame.data(), t);

    auto add = [&](uint16_t bit, const char* name, const char* reason) {
      if (ev & bit) events.push_back({t, name, reason ? reason : ""});
    };
    add(EV_DESCENT, "descent", nullptr);
    add(EV_ON_FLOOR, "on_floor", nullptr);
    add(EV_STUMBLE, "stumble", nullptr);
    add(EV_FALL, "fall", reasonName(d->alertReason));
    add(EV_RECOVERED, "recovered", nullptr);
    if ((ev & EV_FALL) && !alerted) { alerted = true; firstAlert = t; firstReason = reasonName(d->alertReason); }

    if (trace.is_open()) {
      const Features& f = d->f;
      trace << t << ',' << f.present << ',' << f.pixels << ',' << f.cx << ',' << f.cy << ','
            << f.top << ',' << f.bottom << ',' << f.height << ',' << f.width << ',' << f.ratio << ','
            << f.elong << ',' << f.floorFrac << ',' << f.restFrac << ',' << f.motion << ','
            << f.noise << ',' << d->still << ',' << postureName(d->posture) << ','
            << stateName(d->state) << ',' << d->alert << ',' << ev << '\n';
    }
  }

  // JSON summary
  std::ostringstream js;
  js << "{\"file\":\"" << jsonEscape(path) << "\",\"label\":\"" << jsonEscape(label)
     << "\",\"expect\":\"" << jsonEscape(expect) << "\",\"person\":\"" << jsonEscape(person)
     << "\",\"mount\":\"" << jsonEscape(mount) << "\",\"frames\":" << frames
     << ",\"alert\":" << (alerted ? "true" : "false");
  if (alerted) js << ",\"first_alert_ms\":" << firstAlert << ",\"reason\":\"" << firstReason << "\"";
  js << ",\"marks\":[";
  for (size_t k = 0; k < marks.size(); k++) js << (k ? "," : "") << marks[k];
  js << "],\"events\":[";
  for (size_t k = 0; k < events.size(); k++) {
    js << (k ? "," : "") << "{\"t\":" << events[k].t << ",\"e\":\"" << events[k].name << "\"";
    if (!events[k].reason.empty()) js << ",\"reason\":\"" << events[k].reason << "\"";
    js << "}";
  }
  js << "]}";
  std::cout << js.str() << std::endl;
  delete d;
  delete old;
  return 0;
}

int main(int argc, char** argv) {
  std::vector<Override> ov;
  std::vector<std::string> files;
  std::string mountOv, maskOv, tracePath;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&](const char* opt) -> std::string {
      if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", opt); exit(2); }
      return argv[++i];
    };
    if (a == "--set") {
      std::string kv = need("--set");
      size_t eq = kv.find('=');
      if (eq == std::string::npos) { fprintf(stderr, "--set expects name=value\n"); return 2; }
      ov.push_back({kv.substr(0, eq), atof(kv.substr(eq + 1).c_str())});
    } else if (a == "--mount") {
      mountOv = need("--mount");
    } else if (a == "--mask") {
      std::string path = need("--mask");
      if (!loadMaskFile(path, maskOv)) { fprintf(stderr, "cannot read mask %s\n", path.c_str()); return 2; }
    } else if (a == "--algo") {
      std::string v = need("--algo");
      if (v == "legacy") g_legacy = true;
      else if (v != "new") { fprintf(stderr, "--algo must be new or legacy\n"); return 2; }
    } else if (a == "--trace") {
      tracePath = need("--trace");
    } else if (a == "--params") {
      Config c;
      for (int k = 0; k < NUM_PARAMS; k++) printf("%s=%g\n", PARAMS[k].name, getParam(c, k));
      return 0;
    } else if (a == "-h" || a == "--help") {
      printf("usage: replay [--set name=value]... [--mount wall|ceiling] [--mask file] "
             "[--trace out.csv] [--algo new|legacy] [--params] session.csv...\n");
      return 0;
    } else {
      files.push_back(a);
    }
  }
  if (files.empty()) { fprintf(stderr, "no session files given (see --help)\n"); return 2; }
  int rc = 0;
  for (size_t k = 0; k < files.size(); k++)
    rc |= runSession(files[k], ov, mountOv, maskOv, k == 0 ? tracePath : "");
  return rc;
}
