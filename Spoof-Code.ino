#include <GL868_ESP32.h>
#include <math.h>
#include <string.h>
#include <esp_system.h>
#include <Preferences.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>

// ============================================================================
// Configuration
// ============================================================================
#define DEVICE_ID            "Device_name"
#define API_KEY              "Your_API-Key"
#define ALERT_NUMBER         "Care_taker_number"   // primary caretaker
#define PET_NAME             "Toby"             // shown on display

#define GPS_TIMEOUT          30000UL
#define GPS_MAX_RETRIES      3
#define GPS_POLL_INTERVAL    5000UL
#define CLOUD_SEND_INTERVAL  30          // seconds
#define GPS_LOSS_ALERT_POLLS 3

#define CALL_RING_DURATION   15000UL
#define CALL_PENDING_DELAY   3000UL

#define TIME_OFFSET_HOURS    5
#define TIME_OFFSET_MINS     30

#define HYSTERESIS_METERS    20.0
#define CIPSHUT_SETTLE_MS    2000UL

#ifndef DEG_TO_RAD
  static constexpr double DEG_TO_RAD = M_PI / 180.0;
#endif

// ============================================================================
// E-Paper display — pins (TODO: confirm these don't collide with anything
// else you're wiring onto the same board for this build)
// ============================================================================
#define EPD_SCK   12
#define EPD_MOSI  11
#define EPD_CS    10
#define EPD_DC    13
#define EPD_RST   21
#define EPD_BUSY  48

// ASSUMPTION: Waveshare 1.54" 200x200 (GDEH0154D67 controller). Change this
// one line if your panel is different.
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// Small 16x16 monochrome paw-print icon (best-effort stand-in for the 🐾
// emoji — standard GFX fonts can't render emoji/Unicode glyphs, so this is
// a hand-drawn bitmap instead). Purely cosmetic — delete the drawBitmap()
// call in epdShowHealthy() if you'd rather skip it.
static const unsigned char PAW_BITMAP[32] PROGMEM = {
  0x00,0x00, 0x06,0x60, 0x0F,0xF0, 0x0F,0xF0,
  0x06,0x60, 0x00,0x00, 0x19,0x98, 0x3F,0xFC,
  0x3F,0xFC, 0x3F,0xFC, 0x1F,0xF8, 0x0F,0xF0,
  0x07,0xE0, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

// ============================================================================
// Geofence zones
// restricted = false → SMS only on enter/exit
// restricted = true  → SMS + voice call on enter/exit, AND flips the
//                       e-paper to the "lost pet" screen while inside.
// ============================================================================
struct GeofenceZone {
  const char *name;
  double      latitude;
  double      longitude;
  double      radiusMeters;
  bool        restricted;
  bool        insideNow;    // runtime — persisted to NVS on every change
};

GeofenceZone zones[] = {
  { "Home",             11.024934042701, 77.010585083681,  100.0,  false,  false },
  { "Park",             11.019119891223, 77.006775393646,  150.0,  false,  false },
  { "Garden",           11.010967,        77.012959,         80.0,  false,  false },
  { "Restricted Area",  11.011156458988, 77.017023144466,    50.0,  true,   false },
};
constexpr int ZONE_COUNT = sizeof(zones) / sizeof(zones[0]);
static_assert(ZONE_COUNT > 0, "zones[] must not be empty");

// ============================================================================
// Runtime state
// ============================================================================
static Preferences  prefs;
static uint32_t     lastPollTime     = 0;
static uint32_t     lastCloudPush    = 0;
static int          gpsFailStreak    = 0;
static bool         gpsLossAlertSent = false;
static bool         gprsAttached     = false;

static bool         callInProgress   = false;
static uint32_t     callStartTime    = 0;
static bool         pendingCall      = false;
static uint32_t     pendingCallTimer = 0;

// [F1] After cold boot, first fix silently syncs insideNow without alerting.
static bool         firstFixDone     = false;

// Last known fix — used to answer "LOCATION" SMS instantly without forcing
// a fresh GPS acquisition (which can take up to GPS_TIMEOUT*GPS_MAX_RETRIES).
static GPSData      lastGoodGPS      = {};
static bool         haveAnyFix       = false;

// Display state — only redraw the e-paper when this actually changes,
// since e-paper refreshes are slow (~1-2s) and have a finite refresh-cycle
// lifetime; no point re-flashing the same screen every poll.
static bool         petIsLost        = false;
static bool         displayShowsLost = false; // what's currently ON SCREEN

// TESTING ONLY: while true, real GPS-based geofence checks are skipped, so
// a TESTOUT/TESTIN simulated state doesn't get silently overridden by the
// next real GPS fix (which reflects wherever the device actually is, not
// wherever you're pretending it is). Always false on boot — real
// monitoring is never left paused by accident across a power cycle.
static bool         testModeActive   = false;

// ============================================================================
// [F1] Zone state persistence — RESTORE from flash, never clear on boot
// ============================================================================
static void saveZoneStates() {
  prefs.begin("gfence", false);
  for (int i = 0; i < ZONE_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "z%d", i);
    prefs.putBool(key, zones[i].insideNow);
  }
  prefs.end();
}

static void loadZoneStates() {
  prefs.begin("gfence", true);   // read-only open
  for (int i = 0; i < ZONE_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "z%d", i);
    zones[i].insideNow = prefs.getBool(key, false);
  }
  prefs.end();

  Serial.println("[INIT] Zone states loaded from NVS:");
  for (int i = 0; i < ZONE_COUNT; i++) {
    Serial.printf("       %s -> %s\n",
                  zones[i].name,
                  zones[i].insideNow ? "INSIDE (remembered)" : "outside");
  }
}

// ============================================================================
// GPRS helpers
// ============================================================================
static void gprsDetach() {
  if (!gprsAttached) return;
  Serial.println("[GPRS] Detaching...");
  GeoLinker.sendATCommand("+CIPSHUT", 5000);
  gprsAttached = false;
  delay(CIPSHUT_SETTLE_MS);
  Serial.println("[GPRS] Detached.");
}

static void gprsReattach() {
  if (gprsAttached) return;
  Serial.println("[GPRS] Reattaching...");
  if (GeoLinker.gsm.attachGPRS()) {
    gprsAttached = true;
    Serial.println("[GPRS] Attached.");
  } else {
    Serial.println("[GPRS] Failed — will retry next push.");
  }
}

// ============================================================================
// Raw SMS (direct AT commands, CRLF line endings)
// ============================================================================
static bool sendRawSMS(const char *number, const char *message) {
  HardwareSerial &mdm = GeoLinker.getModemSerial();

  GeoLinker.sendATCommand("+CMGF=1", 2000);
  GeoLinker.sendATCommand("+CSCS=\"GSM\"", 2000);
  delay(200);

  mdm.printf("AT+CMGS=\"%s\"\r", number);
  Serial.printf("[SMS] AT+CMGS -> %s\n", number);

  uint32_t t = millis();
  bool gotPrompt = false;
  while (millis() - t < 8000) {
    if (mdm.available()) {
      if (mdm.read() == '>') { gotPrompt = true; break; }
    }
    delay(10);
  }

  if (!gotPrompt) {
    Serial.println("[SMS] No '>' prompt — aborting.");
    mdm.write(0x1B);   // ESC
    return false;
  }

  for (const char *p = message; *p; p++) {
    if (*p == '\n' && (p == message || *(p - 1) != '\r')) mdm.write('\r');
    mdm.write(*p);
  }
  mdm.write(0x1A);  // Ctrl+Z to send

  t = millis();
  while (millis() - t < 30000) {
    if (mdm.available()) {
      String line = mdm.readStringUntil('\n');
      line.trim();
      if (line.startsWith("+CMGS:")) {
        Serial.println("[SMS] Delivered.");
        return true;
      }
      if (line.indexOf("ERROR") >= 0) {
        Serial.printf("[SMS] Error: %s\n", line.c_str());
        return false;
      }
    }
    delay(10);
  }
  Serial.println("[SMS] Timeout waiting for +CMGS.");
  return false;
}

static bool sendReliableSMS(const char *number, const char *message) {
  gprsDetach();
  bool ok = sendRawSMS(number, message);
  gprsReattach();
  return ok;
}

// ============================================================================
// Raw voice call
// ============================================================================
static bool makeRawCall(const char *number) {
  HardwareSerial &mdm = GeoLinker.getModemSerial();
  while (mdm.available()) mdm.read();   // flush

  mdm.printf("ATD%s;\r", number);
  Serial.printf("[CALL] ATD -> %s\n", number);

  uint32_t t = millis();
  while (millis() - t < 5000) {
    if (mdm.available()) {
      String line = mdm.readStringUntil('\n');
      line.trim();
      if (line == "OK") { Serial.println("[CALL] Ringing."); return true; }
      if (line.indexOf("ERROR") >= 0 || line.indexOf("NO CARRIER") >= 0) {
        Serial.printf("[CALL] Rejected: %s\n", line.c_str());
        return false;
      }
    }
    delay(20);
  }
  Serial.println("[CALL] No explicit OK — assuming ringing.");
  return true;
}

// ============================================================================
// Non-blocking call ticker
// ============================================================================
static void tickCall() {
  if (!callInProgress) return;
  GeoLinker.update();
  if (millis() - callStartTime >= CALL_RING_DURATION) {
    GeoLinker.sendATCommand("H", 2000);
    Serial.println("[CALL] Ring timeout — hung up.");
    callInProgress = false;
    delay(500);
    gprsReattach();
  }
}

// ============================================================================
// Haversine distance (metres)
// ============================================================================
static double haversineDistance(double lat1, double lon1,
                                double lat2, double lon2) {
  const double R = 6371000.0;
  double dLat = (lat2 - lat1) * DEG_TO_RAD;
  double dLon = (lon2 - lon1) * DEG_TO_RAD;
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
             sin(dLon / 2) * sin(dLon / 2);
  return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static void sanitiseForSMS(char *dst, int dstLen, const char *src) {
  int j = 0;
  for (int i = 0; src[i] && j < dstLen - 1; i++) {
    char c = src[i];
    if (c == '\r') continue;
    if (c == '"')  { dst[j++] = '\''; continue; }
    dst[j++] = c;
  }
  dst[j] = '\0';
}

// ============================================================================
// SMS body builders
// ============================================================================
static void buildAlertMessage(char *buf, int bufLen,
                              const char *verb, const char *zoneName,
                              GPSData *gps, bool hasLoc) {
  char safe[64];
  sanitiseForSMS(safe, sizeof(safe), zoneName);
  int bat = GeoLinker.getBatteryPercent();

  if (hasLoc) {
    snprintf(buf, bufLen,
             "%s %s %s\n%.6f,%.6f\nSpd:%.1f Bat:%d%%\nmaps.google.com/?q=%.6f,%.6f",
             PET_NAME, verb, safe,
             gps->latitude, gps->longitude,
             gps->speed, bat,
             gps->latitude, gps->longitude);
  } else {
    snprintf(buf, bufLen,
             "%s %s %s\nNo GPS fix\nBat:%d%%",
             PET_NAME, verb, safe, bat);
  }
}

static void buildLocationReplyMessage(char *buf, int bufLen) {
  int bat = GeoLinker.getBatteryPercent();
  if (haveAnyFix) {
    snprintf(buf, bufLen,
             "%s's location:\n%.6f,%.6f\nBat:%d%%\nmaps.google.com/?q=%.6f,%.6f",
             PET_NAME,
             lastGoodGPS.latitude, lastGoodGPS.longitude, bat,
             lastGoodGPS.latitude, lastGoodGPS.longitude);
  } else {
    snprintf(buf, bufLen,
             "%s: no GPS fix yet.\nBat:%d%%", PET_NAME, bat);
  }
}

// ============================================================================
// Geofence alert dispatcher
// ============================================================================
static void sendGeofenceAlert(const char *zoneName, bool entered,
                              bool restricted, GPSData *gps, bool hasLoc) {
  const char *verb = entered ? "ENTERED" : "LEFT";
  Serial.printf("[ALERT] %s %s  restricted:%s\n",
                verb, zoneName, restricted ? "YES" : "NO");

  char msg[256];
  buildAlertMessage(msg, sizeof(msg), verb, zoneName, gps, hasLoc);
  sendReliableSMS(ALERT_NUMBER, msg);

  if (restricted && !callInProgress && !pendingCall) {
    Serial.println("[ALERT] Scheduling voice call in 3 s...");
    pendingCall      = true;
    pendingCallTimer = millis();
  }
}

// ============================================================================
// GPS-loss alert
// ============================================================================
static void sendGPSLossAlert() {
  char msg[160];
  snprintf(msg, sizeof(msg),
           "%s: GPS lost!\nFailed:%d polls\nBat:%d%%",
           PET_NAME, gpsFailStreak, GeoLinker.getBatteryPercent());
  Serial.println("[GPS] Sending GPS-loss SMS...");
  sendReliableSMS(ALERT_NUMBER, msg);
}

// ============================================================================
// GPS acquisition
// ============================================================================
static bool getValidLocation(GPSData *gps) {
  Serial.println("[GPS] Acquiring fix...");
  for (int retry = 0; retry < GPS_MAX_RETRIES; retry++) {
    Serial.printf("[GPS] Attempt %d/%d\n", retry + 1, GPS_MAX_RETRIES);
    uint32_t start = millis();
    while (millis() - start < GPS_TIMEOUT) {
      GeoLinker.update();
      if (GeoLinker.getLocationNow(gps) && gps->valid) {
        Serial.printf("[GPS] Fix: %.6f, %.6f  Sats:%d\n",
                      gps->latitude, gps->longitude, gps->satellites);
        return true;
      }
      uint32_t sub = millis();
      while (millis() - sub < 1000) { GeoLinker.update(); delay(25); }
    }
    Serial.println("[GPS] Timed out, retrying...");
  }
  Serial.println("[GPS] All retries failed.");
  return false;
}

// ============================================================================
// E-Paper display
// ============================================================================
static void epdInit() {
  SPI.begin(EPD_SCK, -1 /* MISO unused */, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(1);
}

// Centers a line of text horizontally for the current font.
static void epdPrintCentered(const char *text, int16_t y) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  int16_t x = (display.width() - w) / 2;
  display.setCursor(x, y);
  display.print(text);
}

// Caretaker number without the "+91" country code, purely for the e-paper
// (shorter string = can be shown bigger without overflowing the 200px
// panel). SMS/calls always use the full ALERT_NUMBER — this is
// display-only.
static const char *epdShortNumber() {
  static char buf[20];
  const char *src = ALERT_NUMBER;
  if (strncmp(src, "+91", 3) == 0) src += 3;
  strncpy(buf, src, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

// Frame drawn just inside the panel edge on every screen. Kept as its own
// helper so both screens stay visually consistent if the margin changes.
static void epdDrawBorder() {
  display.drawRoundRect(6, 6, display.width() - 12, display.height() - 12,
                        10, GxEPD_BLACK);
  display.drawRoundRect(9, 9, display.width() - 18, display.height() - 18,
                        7, GxEPD_BLACK);   // second inner line = a bit bolder
}

static void epdShowHealthy() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    epdDrawBorder();

    // PET_NAME and "Hello" pushed up to 24pt — both are short (<=6 chars)
    // so they still fit comfortably inside the border at this size.
    display.setFont(&FreeMonoBold24pt7b);
    epdPrintCentered(PET_NAME, 80);
    epdPrintCentered("Hi", 135);

    // Small paw icon under the text — cosmetic, see PAW_BITMAP comment above.
    int16_t bx = (display.width() - 16) / 2;
    display.drawBitmap(bx, 155, PAW_BITMAP, 16, 16, GxEPD_BLACK);
  } while (display.nextPage());
}

static void epdShowLost() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    epdDrawBorder();

    display.setFont(&FreeMonoBold24pt7b);
    epdPrintCentered("Oops!", 50);

    // Every line here is kept to <=13 characters — at 9pt bold-mono that's
    // comfortably under the ~180px usable width inside the border, with
    // margin to spare. The two long single lines from before ("I followed
    // my nose...", "Please call my human:", both 22 chars) were wider than
    // the panel itself, which is why they were spilling past the border.
    display.setFont(&FreeMonoBold9pt7b);
    epdPrintCentered("I followed", 75);
    epdPrintCentered("my nose...", 91);
    epdPrintCentered("now I'm lost!", 107);

    epdPrintCentered("Please call", 130);
    epdPrintCentered("my human:", 146);

    // Shown without the +91 country code (see epdShortNumber()) so it can
    // stay at 12pt without overflowing.
    display.setFont(&FreeMonoBold12pt7b);
    epdPrintCentered(epdShortNumber(), 175);
  } while (display.nextPage());
}

// Only actually redraws if the state changed since last time on screen —
// e-paper refreshes are slow and have a finite cycle lifetime, no reason
// to repaint the same screen on every single geofence poll.
static void epdUpdateIfNeeded() {
  if (petIsLost == displayShowsLost) return;
  Serial.printf("[EPD] Updating display -> %s\n", petIsLost ? "LOST" : "Hi");
  if (petIsLost) epdShowLost();
  else           epdShowHealthy();
  displayShowsLost = petIsLost;
}

// ============================================================================
// [F1] Geofence checker
// ============================================================================
static void checkGeofences(GPSData *gps, bool hasLoc) {
  if (!hasLoc) { Serial.println("[FENCE] No fix — skipping."); return; }

  if (testModeActive) {
    Serial.println("[FENCE] Test mode active — skipping real GPS geofence check.");
    return;
  }

  bool stateChanged = false;
  bool zoneJustChanged[ZONE_COUNT] = { false };

  for (int i = 0; i < ZONE_COUNT; i++) {
    double dist        = haversineDistance(gps->latitude, gps->longitude,
                                           zones[i].latitude, zones[i].longitude);
    double enterThresh = zones[i].radiusMeters;
    double exitThresh  = zones[i].radiusMeters + HYSTERESIS_METERS;

    bool nowInside = zones[i].insideNow ? (dist <= exitThresh)
                                        : (dist <= enterThresh);

    Serial.printf("[FENCE] %-10s | %7.1f m | enter:%.0f exit:%.0f | %-7s | %s\n",
                  zones[i].name, dist, enterThresh, exitThresh,
                  nowInside ? "INSIDE" : "outside",
                  zones[i].restricted ? "RESTRICTED" : "normal");

    if (nowInside != zones[i].insideNow) {
      zones[i].insideNow  = nowInside;
      stateChanged        = true;
      zoneJustChanged[i]  = true;
    }
  }

  if (stateChanged) saveZoneStates();

  // Update the e-paper FIRST — it's fast and local. The SMS/voice-call
  // alerts below go out over the modem, which is much slower, so the
  // display should already reflect reality before those are even sent.
  //
  // "Lost" display state = currently inside ANY restricted zone (matches
  // the original design: only a restricted-zone breach shows the "Oops!
  // I'm lost" screen — being merely outside Home/Park/Garden does not).
  bool anyRestrictedIn = false;
  for (int i = 0; i < ZONE_COUNT; i++) {
    if (zones[i].restricted && zones[i].insideNow) anyRestrictedIn = true;
  }
  petIsLost = anyRestrictedIn;
  epdUpdateIfNeeded();

  for (int i = 0; i < ZONE_COUNT; i++) {
    if (!zoneJustChanged[i]) continue;

    if (!firstFixDone) {
      Serial.printf("[FENCE] (silent sync) %s -> %s\n",
                    zones[i].name, zones[i].insideNow ? "inside" : "outside");
    } else {
      sendGeofenceAlert(zones[i].name, zones[i].insideNow,
                        zones[i].restricted, gps, hasLoc);
    }
  }

  if (!firstFixDone) {
    firstFixDone = true;
    Serial.println("[FENCE] First-fix sync complete — alerts now armed.");
  }
}

// ============================================================================
// Cloud push
// ============================================================================
static void cloudPush(GPSData *gps, bool hasLoc) {
  if (!gprsAttached) { gprsReattach(); if (!gprsAttached) return; }

  // If this poll has no fix, fall back to the last known GOOD fix (which
  // has a valid timestamp) instead of GeoLinker.getLastGPS(), which before
  // the very first fix is all-zero with no timestamp set — that's what was
  // causing the cloud API's "Invalid timestamp at index 0" / HTTP 400.
  // The caller (loop()) already skips calling cloudPush() entirely when
  // there has never been any fix at all, so haveAnyFix is true here.
  GeoLinker.json.clear();
  const GPSData &src = hasLoc ? *gps : lastGoodGPS;
  GeoLinker.json.addDataPoint(src,
                              GeoLinker.getBatteryPercent(),
                              GeoLinker.getSignalStrength());
  char payload[1024];
  if (!GeoLinker.json.build(payload, sizeof(payload))) {
    Serial.println("[CLOUD] JSON build failed."); return;
  }

  Serial.printf("[CLOUD] Push: %.6f, %.6f  Bat:%d%%\n",
                src.latitude, src.longitude, GeoLinker.getBatteryPercent());

  int code = GeoLinker.gsm.httpPOST(
               "http://www.circuitdigest.cloud/api/v1/geolinker",
               API_KEY, "application/json", payload);

  Serial.printf("[CLOUD] HTTP %d\n", code);
  if (code == -1) {
    Serial.println("[CLOUD] Failed — will reattach next push.");
    gprsAttached = false;
  }
}

// ============================================================================
// Caretaker "LOCATION" SMS command
//
// NEW vs the reference code, which never read incoming SMS. We enable
// AT+CNMI so new messages get pushed straight to the UART as a "+CMT:"
// line (sender number + metadata) followed by the message body on the
// next line. We watch for that, and if the body is the word "LOCATION"
// (any case), we reply to WHOEVER sent it with the pet's last known fix.
//
// NOTE: confirm this URC format against your actual SIM868 firmware —
// some firmware revisions wrap it slightly differently. If testing shows
// a mismatch, the fix is just adjusting parseSmsSender()/the body-read
// step below, not the overall approach.
// ============================================================================
static String parseSmsSender(const String &cmtLine) {
  int firstQuote = cmtLine.indexOf('"');
  if (firstQuote < 0) return "";
  int secondQuote = cmtLine.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) return "";
  return cmtLine.substring(firstQuote + 1, secondQuote);
}

// ============================================================================
// Zone spoof testing — "<ZONE> IN" / "<ZONE> OUT" SMS commands
//
// Works for ANY configured zone, not just Home: "HOME IN", "HOME OUT",
// "PARK IN", "PARK OUT", "GARDEN IN", "GARDEN OUT", "RESTRICTED IN",
// "RESTRICTED OUT" all work out of the box, matched against zones[] by
// name prefix (case-insensitive), so you don't need to type the full
// "Restricted Area" — "RESTRICTED" is enough.
// ============================================================================
static int findZoneIndexByCode(const String &code) {
  for (int i = 0; i < ZONE_COUNT; i++) {
    String zname = zones[i].name;
    if (zname.length() >= code.length() &&
        zname.substring(0, code.length()).equalsIgnoreCase(code)) {
      return i;
    }
  }
  return -1;
}

static void simulateZoneTransition(int zoneIndex, bool entering, const char *replyTo) {
  if (zoneIndex < 0 || zoneIndex >= ZONE_COUNT) return;

  Serial.printf("[SMS-IN] Simulating %s %s (test only).\n",
                zones[zoneIndex].name, entering ? "entry" : "exit");

  zones[zoneIndex].insideNow = entering;
  saveZoneStates();

  // Update the e-paper FIRST, same as the real geofence path — the
  // display flips immediately, then the (slower) SMS/call go out.
  // "Lost" = currently inside ANY restricted zone (entering the
  // Restricted Area is what flips it to the "Oops! I'm lost" screen).
  bool anyRestrictedIn = false;
  for (int i = 0; i < ZONE_COUNT; i++) {
    if (zones[i].restricted && zones[i].insideNow) anyRestrictedIn = true;
  }
  petIsLost = anyRestrictedIn;
  epdUpdateIfNeeded();

  GPSData dummy = haveAnyFix ? lastGoodGPS : GPSData{};
  sendGeofenceAlert(zones[zoneIndex].name, entering,
                    zones[zoneIndex].restricted, &dummy, haveAnyFix);

  char reply[80];
  snprintf(reply, sizeof(reply), "TEST: %s set to %s",
           zones[zoneIndex].name, entering ? "INSIDE" : "OUTSIDE");
  sendReliableSMS(replyTo, reply);
}

static void checkIncomingSMS() {
  HardwareSerial &mdm = GeoLinker.getModemSerial();
  if (!mdm.available()) return;

  String line = mdm.readStringUntil('\n');
  line.trim();

  // TEMP DEBUG: print every non-empty line seen here, not just "+CMT:"
  // ones. If you send TESTOUT and NOTHING shows up under [SMS-RAW] at
  // all, the URC is being consumed elsewhere (most likely by
  // GeoLinker.update() while it's busy-looping during GPS acquisition)
  // before this function ever gets a turn to read it. If something DOES
  // show up but it doesn't look like "+CMT:...", the modem's URC format
  // differs from what parseSmsSender()/this function expects, and we
  // adjust the parsing to match. Remove this block once SMS-in is
  // confirmed working end to end.
  if (line.length() > 0) {
    Serial.printf("[SMS-RAW] \"%s\"\n", line.c_str());
  }

  if (line.indexOf("+CMT:") < 0) return;

  String sender = parseSmsSender(line);

  // Body arrives on the next line.
  uint32_t t = millis();
  String body;
  while (millis() - t < 1000) {
    if (mdm.available()) {
      body = mdm.readStringUntil('\n');
      body.trim();
      if (body.length() > 0) break;
    }
  }

  if (sender.length() == 0 || body.length() == 0) return;

  Serial.printf("[SMS-IN] From %s: \"%s\"\n", sender.c_str(), body.c_str());

  if (body.equalsIgnoreCase("LOCATION")) {
    Serial.println("[SMS-IN] LOCATION command recognised — replying.");
    char reply[200];
    buildLocationReplyMessage(reply, sizeof(reply));
    sendReliableSMS(sender.c_str(), reply);
  } else if (body.equalsIgnoreCase("TESTMODE ON") || body.equalsIgnoreCase("TESTMODE OFF")) {
    // Explicit ON/OFF rather than a toggle — a toggle is ambiguous if you
    // don't know the current state (e.g. after a reboot, which always
    // resets test mode to OFF), and sending it at the wrong moment just
    // flips it the wrong way with no visible sign anything went wrong.
    bool turnOn = body.equalsIgnoreCase("TESTMODE ON");
    testModeActive = turnOn;
    Serial.printf("[SMS-IN] TESTMODE %s recognised — test mode now %s.\n",
                  turnOn ? "ON" : "OFF", testModeActive ? "ON" : "OFF");

    if (!testModeActive) {
      // Turning test mode OFF: reset the display back to normal
      // (Toby/Hi) rather than leaving it stuck on whatever TESTOUT/TESTIN
      // last set. Real GPS geofence checks resume right after this and
      // will correct it again on the next fix if reality disagrees.
      petIsLost = false;
      epdUpdateIfNeeded();
    }

    char reply[80];
    snprintf(reply, sizeof(reply), "Test mode: %s%s",
             testModeActive ? "ON" : "OFF",
             testModeActive ? " (real GPS geofencing paused)" : " (real monitoring resumed)");
    sendReliableSMS(sender.c_str(), reply);
  } else if (body.equalsIgnoreCase("TESTOUT") || body.equalsIgnoreCase("TESTIN")) {
    // Shortcuts for the most common case: "TESTOUT"/"TESTIN" are just
    // "HOME OUT"/"HOME IN" under the hood. See the generic "<ZONE> IN" /
    // "<ZONE> OUT" handling below for Park/Garden/Restricted Area.
    bool simulateInside = body.equalsIgnoreCase("TESTIN");
    simulateZoneTransition(findZoneIndexByCode("HOME"), simulateInside, sender.c_str());
  } else {
    // Generic "<ZONE> IN" / "<ZONE> OUT" spoof command, works for any
    // configured zone. Splits on the LAST space so multi-word zone names
    // still work (e.g. "RESTRICTED AREA IN"), while short codes also work
    // ("RESTRICTED IN" matches "Restricted Area" via prefix match).
    int lastSpace = body.lastIndexOf(' ');
    if (lastSpace > 0) {
      String zoneCode = body.substring(0, lastSpace);
      String dir      = body.substring(lastSpace + 1);
      zoneCode.trim();
      dir.trim();

      bool wantIn  = dir.equalsIgnoreCase("IN");
      bool wantOut = dir.equalsIgnoreCase("OUT");

      if (wantIn || wantOut) {
        int zi = findZoneIndexByCode(zoneCode);
        if (zi >= 0) {
          simulateZoneTransition(zi, wantIn, sender.c_str());
        } else {
          Serial.printf("[SMS-IN] No zone matches \"%s\".\n", zoneCode.c_str());
        }
      }
    }
  }
}

// ============================================================================
// setup()
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Print WHY the device just (re)started. If it crashed or browned out
  // (e.g. right after drawing the e-paper "lost pet" screen) this line
  // tells us immediately instead of guessing from where the log cuts off.
  {
    const char *reasonStr;
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:   reasonStr = "Power-on (normal)"; break;
      case ESP_RST_EXT:       reasonStr = "External pin reset"; break;
      case ESP_RST_SW:        reasonStr = "Software reset (esp_restart)"; break;
      case ESP_RST_PANIC:     reasonStr = "SOFTWARE PANIC / CRASH"; break;
      case ESP_RST_INT_WDT:   reasonStr = "Interrupt watchdog timeout"; break;
      case ESP_RST_TASK_WDT:  reasonStr = "Task watchdog timeout"; break;
      case ESP_RST_WDT:       reasonStr = "Other watchdog timeout"; break;
      case ESP_RST_DEEPSLEEP: reasonStr = "Woke from deep sleep"; break;
      case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT (power supply dip)"; break;
      case ESP_RST_SDIO:      reasonStr = "SDIO reset"; break;
      default:                reasonStr = "Unknown"; break;
    }
    Serial.printf("[BOOT] Reset reason: %s\n", reasonStr);
  }

  Serial.println("==============================================");
  Serial.println(" PetGuard - Smart Pet Safety System");
  Serial.println("==============================================");

  loadZoneStates();

  GeoLinker.setOperatingMode(MODE_SMS_CALL);
  GeoLinker.setTimeOffset(TIME_OFFSET_HOURS, TIME_OFFSET_MINS);
  GeoLinker.enableFullPowerOff(false);
  GeoLinker.begin(DEVICE_ID, API_KEY);

  Serial.println("[INIT] Waiting for modem IDLE...");
  {
    uint32_t t = millis();
    while (GeoLinker.getState() != STATE_IDLE && millis() - t < 50000UL) {
      GeoLinker.update(); delay(100);
    }
    Serial.printf("[INIT] Modem ready (%lu ms)\n", millis() - t);
  }

  // Enable SMS text mode + push new messages straight to UART as URCs,
  // so checkIncomingSMS() in loop() can react to "LOCATION" requests.
  GeoLinker.sendATCommand("+CMGF=1", 2000);
  GeoLinker.sendATCommand("+CNMI=2,2,0,0,0", 2000);

  Serial.println("[INIT] Attaching GPRS...");
  if (GeoLinker.gsm.attachGPRS()) {
    gprsAttached = true;
    Serial.println("[INIT] GPRS attached.");
  } else {
    Serial.println("[INIT] GPRS failed — retrying on first push.");
  }

  GeoLinker.gpsOn();
  Serial.println("[GPS] GPS powered on.");

  epdInit();
  epdShowHealthy();           // sensible default until the first fix arrives
  displayShowsLost = false;

  Serial.println("\nConfigured Zones:");
  Serial.println("  #  Name       Lat         Lon        Radius   Alert     Inside?");
  Serial.println("  -- ---------- ----------- ---------- -------- --------- -------");
  for (int i = 0; i < ZONE_COUNT; i++) {
    Serial.printf("  %d  %-10s %.6f  %.6f  %.0f m    %-9s %s\n",
                  i + 1, zones[i].name,
                  zones[i].latitude, zones[i].longitude,
                  zones[i].radiusMeters,
                  zones[i].restricted ? "SMS+CALL" : "SMS only",
                  zones[i].insideNow  ? "YES (remembered)" : "no");
  }
  Serial.printf("\nPet name      : %s\n", PET_NAME);
  Serial.printf("Caretaker no. : %s\n", ALERT_NUMBER);
  Serial.printf("Poll interval : %lu ms\n", GPS_POLL_INTERVAL);
  Serial.printf("Cloud interval: %d s\n\n", CLOUD_SEND_INTERVAL);
  Serial.println("Monitoring started.\n");

  lastPollTime  = millis();
  lastCloudPush = millis();
}

// ============================================================================
// loop()
// ============================================================================
void loop() {
  GeoLinker.update();

  // Watch for an incoming "LOCATION" SMS on every tick — this is cheap to
  // check and shouldn't wait for the next GPS poll interval.
  checkIncomingSMS();

  // Pending call state machine
  if (pendingCall && (millis() - pendingCallTimer >= CALL_PENDING_DELAY)) {
    if (!callInProgress) {
      gprsDetach();
      Serial.println("[CALL] Initiating voice call...");
      if (makeRawCall(ALERT_NUMBER)) {
        callInProgress = true;
        callStartTime  = millis();
      } else {
        Serial.println("[CALL] Dial failed — reattaching GPRS.");
        gprsReattach();
      }
      pendingCall = false;
    }
  }

  tickCall();

  // GPS poll gate
  if (millis() - lastPollTime < GPS_POLL_INTERVAL) return;
  lastPollTime = millis();

  if (callInProgress) {
    Serial.println("[LOOP] Call in progress — skipping poll.");
    return;
  }

  GPSData gps = {};
  bool hasLoc = getValidLocation(&gps);

  if (hasLoc) {
    if (gpsFailStreak > 0)
      Serial.printf("[GPS] Restored after %d failure(s).\n", gpsFailStreak);
    gpsFailStreak    = 0;
    gpsLossAlertSent = false;
    lastGoodGPS      = gps;
    haveAnyFix       = true;
  } else {
    gpsFailStreak++;
    Serial.printf("[GPS] Failure %d/%d\n", gpsFailStreak, GPS_LOSS_ALERT_POLLS);
    if (gpsFailStreak >= GPS_LOSS_ALERT_POLLS && !gpsLossAlertSent) {
      sendGPSLossAlert();
      gpsLossAlertSent = true;
    }
  }

  // STEP 1: geofence check (alerts + display update)
  checkGeofences(&gps, hasLoc);

  // STEP 2: cloud push (only if no alert/call activity)
  if (!pendingCall && !callInProgress &&
      millis() - lastCloudPush >= (uint32_t)CLOUD_SEND_INTERVAL * 1000UL) {
    lastCloudPush = millis();
    if (hasLoc || haveAnyFix) {
      cloudPush(&gps, hasLoc);
    } else {
      Serial.println("[CLOUD] Skipping push — no GPS fix yet (avoids invalid-timestamp reject).");
    }
  }

  Serial.printf("[LOOP] Done. Next in %lu ms\n\n", GPS_POLL_INTERVAL);
}
