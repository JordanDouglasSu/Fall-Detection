// =====================================================================
//  PIPER fall node — settings you change for your setup
// =====================================================================
#pragma once
#include "fall_algo.h"

// ---- Identity ---------------------------------------------------------
// (Each #ifndef lets these be overridden from build flags, so the file can be
//  shared in git without anyone's Wi-Fi password in it.)
#ifndef NODE_ID
#define NODE_ID        "fall01"          // MQTT topics use this: piper/sensor/fall01 ...
#endif

// ---- Wi-Fi --------------------------------------------------------------
// Put the same 2.4 GHz hotspot the PC and the other ESP32s use.
// Leave WIFI_SSID empty to run stand-alone: the board makes its own network.
#ifndef WIFI_SSID
#define WIFI_SSID      ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS      ""
#endif

// Stand-alone network (used when WIFI_SSID is empty or cannot connect).
// CHANGE THE PASSWORD before the demo (8+ characters).
#define AP_SSID        "PIPER-Fall-" NODE_ID
#ifndef AP_PASS
#define AP_PASS        "change-this-pass"
#endif

// ---- MQTT (needs WIFI_SSID) -------------------------------------------------
#ifndef MQTT_ENABLED
#define MQTT_ENABLED   1
#endif
#ifndef MQTT_HOST
#define MQTT_HOST      "192.168.43.100"   // IP of the PC running Mosquitto
#endif
#ifndef MQTT_PORT
#define MQTT_PORT      1883
#endif
#ifndef MQTT_USER
#define MQTT_USER      ""                 // set once the broker password is turned on
#endif
#ifndef MQTT_PASS
#define MQTT_PASS      ""
#endif

// ---- Sensor wiring ------------------------------------------------------------
#define RXD2           16                 // ESP32 RX2 <- GY-MCU90640 TX
#define TXD2           17                 // ESP32 TX2 -> GY-MCU90640 RX
#define ALERT_LED_PIN  2                  // onboard LED
#define STRICT_CHECKSUM 0                 // 1 = drop frames with a bad checksum

// ---- Serial logging -------------------------------------------------------------
#define SERIAL_FEATURES 0                 // 1 = print one CSV line of features per frame

// ---- How the sensor is mounted ---------------------------------------------------
//   fall::MOUNT_WALL    — on a wall/shelf looking across the room (default)
//   fall::MOUNT_CEILING — on the ceiling looking straight down
#define SENSOR_MOUNT   fall::MOUNT_WALL

// ---- Room map: 24 rows x 32 columns, as the sensor sees the room ----------------
//   '.'  normal (wall, furniture, air)
//   'F'  floor     — someone lying here counts as "on the floor"
//   'B'  bed/sofa  — lying here is resting, never a fall
//   'X'  ignore    — radiator, TV, sunny window, kettle ...
// Row 0 is the TOP of the image. Use the dashboard's "Zones" overlay and the
// row/column read-out under the image to place them. See docs/SETUP.md.
// Default: floor visible in the lower half of the image (wall mount).
static const char* const ROOM_MASK[24] = {
  //0         1         2         3
  //01234567890123456789012345678901
  "................................",  // 0  (top of image)
  "................................",  // 1
  "................................",  // 2
  "................................",  // 3
  "................................",  // 4
  "................................",  // 5
  "................................",  // 6
  "................................",  // 7
  "................................",  // 8
  "................................",  // 9
  "................................",  // 10
  "................................",  // 11
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 12
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 13
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 14
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 15
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 16
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 17
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 18
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 19
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 20
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 21
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 22
  "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",  // 23 (bottom of image)
};

// ---- Tuning ------------------------------------------------------------------
// Change values here after testing them on recordings with tools/evaluate.py.
// The full list with explanations is in fall_algo.h (struct Config).
inline void applyTuning(fall::Config& c) {
  c.mount = SENSOR_MOUNT;
  // examples:
  // c.fgDelta        = 1.3f;
  // c.stillConfirmMs = 6000;
  // c.longLieMs      = 30000;
  (void)c;
}
