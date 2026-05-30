/*
  ecoflow_dial.ino — EcoFlow Delta 2 Max remote
  Encoder scrolls screens (or selects controls on CTRL screen)
  Press jumps to CTRL screen, or activates selected control
*/

#include "M5Dial.h"
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"
#include "esp_rom_sys.h"
#include <esp_wifi.h>
#include "secrets.h"
#include "logo.h"

#define DBG(...) do { esp_rom_printf(__VA_ARGS__); esp_rom_printf("\n"); } while(0)

// ── NVS / Config ──────────────────────────────────────────────────────────────
#define NVS_NS         "efdial"
#define NVS_SERIAL     "serial"
#define NVS_DEV_NAME   "devName"
#define NVS_ACCESS_KEY "akey"
#define NVS_SECRET_KEY "skey"
#define POLL_INTERVAL_MS  8000
#define EF_HOST           "https://api-a.ecoflow.com"

// SIM_MODE=1 ramps the SOC automatically to preview every battery level + arc
// color without the real device. Set to 0 for live data.
#define SIM_MODE 0

String cfgAccessKey, cfgSecretKey, cfgSerial, cfgDevName;

// ── EcoFlow data ──────────────────────────────────────────────────────────────
struct EFData {
  int   soc=0, temp=0, cycles=0, soh=0;
  int   chgRemain=0, dsgRemain=0;
  float acInW=0, acOutW=0, solarW=0, pv2W=0;
  bool  acEnabled=false, powered=false, valid=false;
  int   maxChargeSoc=100, fastChgW=1800;
  unsigned long age=0;
} ef;

// ── UI state ──────────────────────────────────────────────────────────────────
// Four screens in scroll order: STAT → CTRL → INFO → RESET (and wrap).
// CTRL is "inactive" until pressed — scrolling passes through it like any
// other view. Pressing enters edit mode where scrolling selects rows.
// RESET is a dedicated screen — pressing the screen opens the confirm modal.
enum Screen : uint8_t { STAT=0, CTRL=1, INFO=2, RESET=3 };
const int SCREEN_COUNT = 4;
Screen screen = STAT;
bool   ctrlActive    = false;    // press once on CTRL → selecting rows
bool   ctrlAdjusting = false;    // press a row → adjusting that row's value
int    menuSel = 0;

bool   resetActive = false;      // press once on RESET → selecting an option
int    resetSel = 0;
const int RESET_COUNT = 4;
const char* RESET_LABELS[]  = { "WIFI",       "DEVICE",          "API KEYS",     "FACTORY" };
const char* RESET_DETAILS[] = { "WIFI ONLY?", "DEVICE SN ONLY?", "API KEYS ONLY?", "WIPE ALL CONFIG?" };

// ── Confirmation modal ────────────────────────────────────────────────────────
// For destructive/irreversible actions. Scroll toggles YES/NO; defaults to NO.
bool   confirmActive = false;
String confirmTitle;
String confirmDetail;
bool   confirmYes = false;
void  (*confirmFn)() = nullptr;
const int CTRL_COUNT = 3;
const char* CTRL_LABELS[] = { "AC OUTPUT", "MAX CHARGE", "FAST CHG" };

// Preset values the FAST CHG control cycles through
const int FAST_CHG_PRESETS[] = { 200, 400, 800, 1200, 1500, 1800 };
const int FAST_CHG_COUNT = sizeof(FAST_CHG_PRESETS) / sizeof(int);

long lastEnc = 0;
unsigned long lastPollMs = 0;
bool needsDraw = true;
volatile bool portalSavedParams = false;   // set by WiFiManager save callback
Preferences prefs;

// Brightness breathe — layered sines + random jitter for an organic CRT feel.
// Just touches the backlight, so it works on every screen without redrawing.
void pulseBrightness() {
  static uint32_t lastUpdate = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastUpdate < 30) return;     // ~33 fps update cap
  lastUpdate = nowMs;

  // Two sines beating together so the pattern doesn't feel repetitive
  float slow = (sinf(nowMs / 318.0f) + 1.0f) * 0.5f;   // 0..1, ~2s
  float fast = sinf(nowMs / 137.0f) * 0.15f;           // small faster wobble
  int8_t jitter = random(-6, 7);                       // ±6 unit of CRT hum

  int b = 140 + (int)(slow * 60) + (int)(fast * 30) + jitter;
  if (b < 110) b = 110;
  if (b > 220) b = 220;
  M5Dial.Display.setBrightness((uint8_t)b);
}

// ── API key loading ───────────────────────────────────────────────────────────
// Fallback chain: NVS (user-entered) → secrets.h (compiled-in) → empty.
// Returns true if both keys are present. Empty → the portal must prompt.
bool loadApiKeys() {
  prefs.begin(NVS_NS, true);
  cfgAccessKey = prefs.getString(NVS_ACCESS_KEY, "");
  cfgSecretKey = prefs.getString(NVS_SECRET_KEY, "");
  prefs.end();

  // Fall back to compiled-in keys (PERSONAL_BUILD) if NVS is empty
  if (cfgAccessKey.length() == 0) cfgAccessKey = SECRET_ACCESS_KEY;
  if (cfgSecretKey.length() == 0) cfgSecretKey = SECRET_SECRET_KEY;

  DBG("[KEYS] access len=%u secret len=%u", cfgAccessKey.length(), cfgSecretKey.length());
  return cfgAccessKey.length() > 0 && cfgSecretKey.length() > 0;
}

void saveApiKeys(const String& ak, const String& sk) {
  prefs.begin(NVS_NS, false);
  prefs.putString(NVS_ACCESS_KEY, ak);
  prefs.putString(NVS_SECRET_KEY, sk);
  prefs.end();
  cfgAccessKey = ak;
  cfgSecretKey = sk;
}

// ── HMAC-SHA256 ───────────────────────────────────────────────────────────────
String hmacSha256(const String& msg, const String& key) {
  uint8_t out[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  String hex = "";
  for (int i = 0; i < 32; i++) {
    if (out[i] < 0x10) hex += "0";
    hex += String(out[i], HEX);
  }
  return hex;
}

// ── EcoFlow GET ───────────────────────────────────────────────────────────────
String efGet(const String& path, const String& sn = "") {
  if (WiFi.status() != WL_CONNECTED) return "";
  String nonce = String(random(100000, 999999));
  String ts    = String((unsigned long long)time(nullptr) * 1000ULL);
  String signStr = (sn.length() ? "sn=" + sn + "&" : "")
                 + "accessKey=" + cfgAccessKey
                 + "&nonce="    + nonce
                 + "&timestamp="+ ts;
  String sign = hmacSha256(signStr, cfgSecretKey);
  String url  = String(EF_HOST) + path + (sn.length() ? "?sn=" + sn : "");
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.begin(client, url); http.setTimeout(6000);
  http.addHeader("accessKey", cfgAccessKey);
  http.addHeader("nonce",     nonce);
  http.addHeader("timestamp", ts);
  http.addHeader("sign",      sign);
  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();
  return body;
}

// ── EcoFlow PUT ───────────────────────────────────────────────────────────────
void efPut(const String& operateType, const String& jsonBody) {
  if (WiFi.status() != WL_CONNECTED) return;
  String nonce = String(random(100000, 999999));
  String ts    = String((unsigned long long)time(nullptr) * 1000ULL);
  String signStr = "operateType=" + operateType
                 + "&sn="          + cfgSerial
                 + "&accessKey="   + cfgAccessKey
                 + "&nonce="       + nonce
                 + "&timestamp="   + ts;
  String sign = hmacSha256(signStr, cfgSecretKey);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, String(EF_HOST) + "/iot-open/sign/device/quota");
  http.setTimeout(6000);
  http.addHeader("Content-Type", "application/json;charset=UTF-8");
  http.addHeader("accessKey", cfgAccessKey);
  http.addHeader("nonce",     nonce);
  http.addHeader("timestamp", ts);
  http.addHeader("sign",      sign);
  int code = http.PUT(jsonBody);
  DBG("[PUT] %s -> %d", operateType.c_str(), code);
  http.end();
}

// ── Controls — send the *current* ef.* value (caller adjusts it first) ────────
void sendAC() {
  char body[200];
  snprintf(body, sizeof(body),
    "{\"sn\":\"%s\",\"operateType\":\"acOutCfg\","
    "\"params\":{\"enabled\":%d,\"out_freq\":2,\"out_voltage\":120000,\"xboost\":1}}",
    cfgSerial.c_str(), ef.acEnabled ? 1 : 0);
  efPut("acOutCfg", body);
}
void sendChargeSoc() {
  char body[140];
  snprintf(body, sizeof(body),
    "{\"sn\":\"%s\",\"operateType\":\"upsConfig\",\"params\":{\"maxChargeSoc\":%d}}",
    cfgSerial.c_str(), ef.maxChargeSoc);
  efPut("upsConfig", body);
}
void sendFastChg() {
  char body[160];
  snprintf(body, sizeof(body),
    "{\"sn\":\"%s\",\"operateType\":\"acChgCfg\","
    "\"params\":{\"fastChgWatts\":%d,\"slowChgWatts\":%d,\"chgPauseFlag\":0}}",
    cfgSerial.c_str(), ef.fastChgW, ef.fastChgW);
  efPut("acChgCfg", body);
}

// Shared "RESETTING…" splash before rebooting
void showResetSplash(const char* what) {
  M5Dial.Display.fillScreen(BLACK);
  M5Dial.Display.setTextColor(RED, BLACK);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setFont(&fonts::Font4);
  M5Dial.Display.drawString("RESETTING", 120, 110);
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.drawString(what, 120, 140);
  delay(1500);
}

// Wipe WiFi credentials only — re-runs captive portal on next boot
void doResetWifi() {
  showResetSplash("WIFI ONLY");
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// Wipe device serial only — re-runs the device picker on next boot
void doResetDevice() {
  showResetSplash("DEVICE SN");
  prefs.begin(NVS_NS, false);
  prefs.remove(NVS_SERIAL);
  prefs.remove(NVS_DEV_NAME);
  prefs.end();
  ESP.restart();
}

// Wipe API keys only — re-prompts on next boot (no effect if compiled-in keys
// exist via PERSONAL_BUILD, since the fallback chain will just reuse those)
void doResetKeys() {
  showResetSplash("API KEYS");
  prefs.begin(NVS_NS, false);
  prefs.remove(NVS_ACCESS_KEY);
  prefs.remove(NVS_SECRET_KEY);
  prefs.end();
  ESP.restart();
}

// Wipe everything — WiFi, device serial, and API keys
void doResetFactory() {
  showResetSplash("ALL CONFIG");
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// Legacy alias so existing call sites still link
void doReset() { doResetFactory(); }

// Adjust the current row's value by `dir` (-1 or +1) while in adjust mode
void adjustCurrent(int dir) {
  switch (menuSel) {
    case 0:
      ef.acEnabled = !ef.acEnabled;   // toggle on any scroll
      break;
    case 1: {
      int s = ef.maxChargeSoc + dir * 10;
      if (s < 50)  s = 50;
      if (s > 100) s = 100;
      ef.maxChargeSoc = s;
      break;
    }
    case 2: {
      int idx = 0;
      for (int i = 0; i < FAST_CHG_COUNT; i++) {
        if (FAST_CHG_PRESETS[i] >= ef.fastChgW) { idx = i; break; }
      }
      idx += dir;
      if (idx < 0) idx = 0;
      if (idx >= FAST_CHG_COUNT) idx = FAST_CHG_COUNT - 1;
      ef.fastChgW = FAST_CHG_PRESETS[idx];
      break;
    }
  }
}

void enterConfirm(const String& title, const String& detail, void (*fn)()) {
  confirmActive = true;
  confirmTitle = title;
  confirmDetail = detail;
  confirmYes = false;     // default NO for safety
  confirmFn = fn;
}

void commitCurrent() {
  switch (menuSel) {
    case 0:
      // AC OUTPUT: confirm before sending
      enterConfirm("AC OUTPUT",
                   ef.acEnabled ? "TURN ON?" : "TURN OFF?",
                   sendAC);
      break;
    case 1: sendChargeSoc();  break;
    case 2: sendFastChg();    break;
  }
}

// ── Data fetch ────────────────────────────────────────────────────────────────
void fetchData() {
  String resp = efGet("/iot-open/sign/device/quota/all", cfgSerial);
  if (!resp.length()) return;
  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) return;
  if (String(doc["code"].as<const char*>()) != "0") return;
  JsonObject d = doc["data"];
  ef.soc          = d["bms_bmsStatus.soc"]           | 0;
  ef.temp         = d["bms_bmsStatus.temp"]          | 0;
  ef.cycles       = d["bms_bmsStatus.cycles"]        | 0;
  ef.soh          = d["bms_bmsStatus.soh"]           | 0;
  ef.chgRemain    = d["bms_emsStatus.chgRemainTime"] | 0;
  ef.dsgRemain    = d["bms_emsStatus.dsgRemainTime"] | 0;
  ef.solarW       = d["mppt.inWatts"]                | 0.0f;
  ef.pv2W         = d["mppt.pv2InWatts"]             | 0.0f;
  ef.acInW        = d["inv.inputWatts"]              | 0.0f;
  ef.acOutW       = d["inv.outputWatts"]             | 0.0f;
  ef.acEnabled    = ((int)(d["inv.cfgAcEnabled"]     | 0)) == 1;
  ef.maxChargeSoc = d["bms_emsStatus.maxChargeSoc"]  | 100;
  ef.fastChgW     = d["inv.FastChgWatts"]            | 1800;
  ef.powered      = (ef.acInW > 5 || ef.solarW > 5);
  ef.valid        = true;
  ef.age          = millis();
  DBG("[FETCH] soc=%d acIn=%dW acOut=%dW ac=%d", ef.soc, (int)ef.acInW, (int)ef.acOutW, ef.acEnabled);
}

#if SIM_MODE
// Fake data that ramps the SOC down then up, flipping powered state at the ends,
// so you can watch every battery level + arc color transition without the real
// device. Called from loop() instead of fetchData() when SIM_MODE is on.
void simData() {
  static int   simSoc = 100;
  static int   dir    = -1;       // -1 draining, +1 charging
  simSoc += dir;
  if (simSoc <= 0)   { simSoc = 0;   dir = +1; }
  if (simSoc >= 100) { simSoc = 100; dir = -1; }

  ef.soc       = simSoc;
  ef.powered   = (dir > 0);                  // "charging" while ramping up
  ef.temp      = 25;
  ef.cycles    = 30;
  ef.soh       = 100;
  ef.chgRemain = ef.powered ? (100 - simSoc) * 3 : 5999;
  ef.dsgRemain = ef.powered ? 5999 : simSoc * 8;
  ef.solarW    = ef.powered ? 120 : 0;
  ef.pv2W      = ef.powered ? 80  : 0;
  ef.acInW     = ef.powered ? 200 : 0;
  ef.acOutW    = ef.powered ? 50  : 95;
  ef.acEnabled = true;
  ef.valid     = true;
  ef.age       = millis();
}
#endif

// ── Drawing helpers ───────────────────────────────────────────────────────────
uint16_t fgColor()  { return ef.powered ? GREEN : RED; }
uint16_t dimColor() { return ef.powered ? 0x03E0 : 0x7800; }

// Arc color shifts with SOC (independent of powered state):
//   >50 %: green   25–50 %: yellow   10–25 %: orange   <10 %: red
uint16_t arcColor(int soc) {
  if (soc > 50) return 0x07E0;  // green
  if (soc > 25) return 0xFFE0;  // yellow
  if (soc > 10) return 0xFD20;  // orange
  return 0xF800;                // red
}
uint16_t arcDim(int soc) {
  if (soc > 50) return 0x03E0;
  if (soc > 25) return 0x7BE0;
  if (soc > 10) return 0x7960;
  return 0x7800;
}

// Format remaining minutes as "Xh Ym" / "Zm" / "FULL" / "IDLE"
String timeStr(int mins, bool charging) {
  if (mins >= 5999) return charging ? "FULL" : "IDLE";
  if (mins <= 0)    return "--";
  int h = mins / 60, m = mins % 60;
  if (h == 0) return String(m) + "m";
  return String(h) + "h" + String(m) + "m";
}

String ctrlValue(int i) {
  if (i==0) return ef.acEnabled ? "ON" : "OFF";
  if (i==1) return String(ef.maxChargeSoc) + "%";
  if (i==2) return String(ef.fastChgW) + "W";
  return "—";
}

// Instructions for the user while the WiFi captive portal is active.
// Drawn once when WiFiManager fires its setAPCallback.
void drawWifiSetup(const String& ssid, const String& ip) {
  M5Dial.Display.fillScreen(BLACK);
  M5Dial.Display.setTextDatum(middle_center);

  // Title
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextSize(1.3f);
  M5Dial.Display.setTextColor(GREEN, BLACK);
  M5Dial.Display.drawString("WIFI SETUP", 120, 26);

  // Step 1
  M5Dial.Display.setTextSize(1.0f);
  M5Dial.Display.setTextColor(0x03E0, BLACK);
  M5Dial.Display.drawString("1. CONNECT PHONE TO:", 120, 62);
  M5Dial.Display.setTextSize(1.2f);
  M5Dial.Display.setTextColor(GREEN, BLACK);
  M5Dial.Display.drawString(ssid, 120, 86);

  // Step 2
  M5Dial.Display.setTextSize(1.0f);
  M5Dial.Display.setTextColor(0x03E0, BLACK);
  M5Dial.Display.drawString("2. OPEN BROWSER TO:", 120, 122);
  M5Dial.Display.setTextSize(1.2f);
  M5Dial.Display.setTextColor(GREEN, BLACK);
  M5Dial.Display.drawString(ip, 120, 146);

  // Step 3
  M5Dial.Display.setTextSize(1.0f);
  M5Dial.Display.setTextColor(0x03E0, BLACK);
  M5Dial.Display.drawString("3. ENTER WIFI + KEYS", 120, 182);

  M5Dial.Display.setTextColor(0x03E0, BLACK);
  M5Dial.Display.drawString("WAITING...", 120, 215);
}

// Shown when WiFi is already connected but API keys are missing — we reopen the
// portal just to collect the keys.
void drawApiKeySetup(const String& ssid, const String& ip) {
  M5Dial.Display.fillScreen(BLACK);
  M5Dial.Display.setTextDatum(middle_center);

  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextSize(1.3f);
  M5Dial.Display.setTextColor(GREEN, BLACK);
  M5Dial.Display.drawString("API KEY SETUP", 120, 26);

  M5Dial.Display.setTextSize(1.0f);
  M5Dial.Display.setTextColor(0x03E0, BLACK);
  M5Dial.Display.drawString("1. JOIN WIFI:", 120, 62);
  M5Dial.Display.setTextSize(1.2f);
  M5Dial.Display.setTextColor(GREEN, BLACK);
  M5Dial.Display.drawString(ssid, 120, 86);

  M5Dial.Display.setTextSize(1.0f);
  M5Dial.Display.setTextColor(0x03E0, BLACK);
  M5Dial.Display.drawString("2. BROWSE TO:", 120, 122);
  M5Dial.Display.setTextSize(1.2f);
  M5Dial.Display.setTextColor(GREEN, BLACK);
  M5Dial.Display.drawString(ip, 120, 146);

  M5Dial.Display.setTextSize(1.0f);
  M5Dial.Display.setTextColor(0x03E0, BLACK);
  M5Dial.Display.drawString("3. ENTER EF KEYS", 120, 182);
  M5Dial.Display.drawString("WAITING...", 120, 215);
}

// Full-screen error, holds for `ms`
void drawError(const String& l1, const String& l2, int ms) {
  M5Dial.Display.fillScreen(BLACK);
  M5Dial.Display.setTextColor(RED, BLACK);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setFont(&fonts::Font4);
  M5Dial.Display.drawString(l1, 120, 110);
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.drawString(l2, 120, 142);
  delay(ms);
}

// Boot splash — green field with EF logo silhouette + hard scanlines.
// Drawn once per phase. Brightness pulse handled separately so no full-frame redraw.
void drawSplash(const char* /*status*/) {
  M5Dial.Display.fillScreen(GREEN);

  int lx = (240 - EF_LOGO_W) / 2;
  int ly = (240 - EF_LOGO_H) / 2;
  M5Dial.Display.drawBitmap(lx, ly, ef_logo, EF_LOGO_W, EF_LOGO_H, BLACK);

  // Hard CRT scanlines every 3px
  for (int y = 0; y < 240; y += 3) {
    M5Dial.Display.drawFastHLine(0, y, 240, BLACK);
  }
}

// CRT scanline overlay — drawn LAST, over everything. Spaced wider + dimmer
// so it stays atmospheric without obscuring text.
void drawScanlines() {
  uint16_t c = 0x0820;   // very dark, barely-there
  for (int y = 0; y < 240; y += 4) {
    M5Dial.Display.drawFastHLine(0, y, 240, c);
  }
}

// Vault-Tec style corner brackets
void drawCorners(uint16_t c) {
  // top-left
  M5Dial.Display.drawFastHLine(8,  8,  18, c);
  M5Dial.Display.drawFastVLine(8,  8,  18, c);
  // top-right
  M5Dial.Display.drawFastHLine(214, 8, 18, c);
  M5Dial.Display.drawFastVLine(231, 8, 18, c);
  // bottom-left
  M5Dial.Display.drawFastHLine(8,  231, 18, c);
  M5Dial.Display.drawFastVLine(8,  214, 18, c);
  // bottom-right
  M5Dial.Display.drawFastHLine(214, 231, 18, c);
  M5Dial.Display.drawFastVLine(231, 214, 18, c);
}

// Signal-strength bars top-right. (Clock removed — it was just UTC time and got
// clipped by the round display edge.)
void drawStatusBar() {
  uint16_t fg = fgColor(), dim = dimColor();
  int rssi = WiFi.isConnected() ? WiFi.RSSI() : -100;
  int bars = 0;
  if      (rssi > -55) bars = 4;
  else if (rssi > -65) bars = 3;
  else if (rssi > -75) bars = 2;
  else if (rssi > -85) bars = 1;
  for (int i = 0; i < 4; i++) {
    int x = 180 + i * 6;
    int h = 3 + i * 2;
    int y = 14 - h;
    M5Dial.Display.fillRect(x, y, 4, h, i < bars ? fg : dim);
  }
}

// Top chrome: corners + clock + signal bars. Safe to draw before arc.
void drawChrome() {
  drawCorners(dimColor());
  drawStatusBar();

  // bottom screen indicator dots
  for (int i = 0; i < SCREEN_COUNT; i++) {
    int x = 120 - (SCREEN_COUNT - 1) * 9 + i * 18;
    if (i == screen) {
      M5Dial.Display.fillCircle(x, 222, 4, fgColor());
    } else {
      M5Dial.Display.drawCircle(x, 222, 4, dimColor());
    }
  }
}

// Title block — must be drawn LAST so it sits on top of any arc/content
void drawTitle(const char* title) {
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(fgColor(), BLACK);
  char buf[32];
  snprintf(buf, sizeof(buf), "[[ %s ]]", title);
  M5Dial.Display.drawString(buf, 120, 30);

  // dashed separator
  for (int x = 30; x < 210; x += 6) {
    M5Dial.Display.drawFastHLine(x, 40, 3, dimColor());
  }
}

// Convenience: draws everything (used by screens with no arc overlap)
void drawHeader(const char* title) {
  drawChrome();
  drawTitle(title);
}

// ── STAT screen ──────────────────────────────────────────────────────────────
void drawStat() {
  M5Dial.Display.fillScreen(BLACK);
  drawChrome();   // corners + status bar + nav dots — under the arc is fine

  const int cx = 120, cy = 122;
  const int rOuter = 108, rInner = 96;
  const float startDeg = 140.0f, sweep = 260.0f;
  uint16_t fg = fgColor(), dim = dimColor();
  uint16_t aFg = arcColor(ef.soc), aDim = arcDim(ef.soc);

  // Background ring
  M5Dial.Display.fillArc(cx, cy, rInner, rOuter, startDeg, startDeg + sweep, aDim);
  // Filled portion = SOC% (colored by charge level)
  if (ef.soc > 0) {
    float fillEnd = startDeg + (ef.soc / 100.0f) * sweep;
    M5Dial.Display.fillArc(cx, cy, rInner, rOuter, startDeg, fillEnd, aFg);
  }
  // 10% tick marks
  for (int i = 0; i <= 10; i++) {
    float a = (startDeg + i * (sweep / 10.0f)) * DEG_TO_RAD;
    int x1 = cx + (int)((rInner - 4) * cosf(a));
    int y1 = cy + (int)((rInner - 4) * sinf(a));
    int x2 = cx + (int)((rInner - 10) * cosf(a));
    int y2 = cy + (int)((rInner - 10) * sinf(a));
    M5Dial.Display.drawLine(x1, y1, x2, y2, aDim);
  }
  // Position dot
  if (ef.soc > 0 && ef.soc < 100) {
    float endRad = (startDeg + (ef.soc / 100.0f) * sweep) * DEG_TO_RAD;
    int dx = cx + (int)((rInner + 6) * cosf(endRad));
    int dy = cy + (int)((rInner + 6) * sinf(endRad));
    M5Dial.Display.fillCircle(dx, dy, 5, aFg);
  }

  // Big SOC number — measure the actual rendered width and center precisely
  // on (cx-10, cy-32) so it stays put as the value goes 100 → 78 → 8.
  M5Dial.Display.setFont(&fonts::Font7);
  M5Dial.Display.setTextSize(1.2f);
  M5Dial.Display.setTextColor(fg, BLACK);
  String socStr = String(ef.soc);
  int   socW    = M5Dial.Display.textWidth(socStr);
  int   socH    = M5Dial.Display.fontHeight();
  M5Dial.Display.setTextDatum(top_left);
  M5Dial.Display.drawString(socStr, (cx - 10) - socW / 2, (cy - 32) - socH / 2);
  M5Dial.Display.setTextDatum(middle_center);   // restore for the lines below

  // Charging / on-battery status
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextSize(1.2f);
  M5Dial.Display.setTextColor(dim, BLACK);
  M5Dial.Display.drawString(ef.powered ? "CHARGING" : "ON BATTERY", cx, cy + 8);

  // Time remaining — emphasized
  int rem = ef.powered ? ef.chgRemain : ef.dsgRemain;
  M5Dial.Display.setTextSize(1.4f);
  M5Dial.Display.setTextColor(fg, BLACK);
  M5Dial.Display.drawString(timeStr(rem, ef.powered), cx, cy + 32);

  // Power detail — two lines
  M5Dial.Display.setTextSize(1.3f);
  M5Dial.Display.setTextColor(dim, BLACK);
  char pbuf[40];
  snprintf(pbuf, sizeof(pbuf), "AC %d  OUT %d",
           (int)ef.acInW, (int)ef.acOutW);
  M5Dial.Display.drawString(pbuf, cx, cy + 56);
  snprintf(pbuf, sizeof(pbuf), "PV1 %d  PV2 %d",
           (int)ef.solarW, (int)ef.pv2W);
  M5Dial.Display.drawString(pbuf, cx, cy + 76);
  M5Dial.Display.setTextSize(1.0f);

  drawTitle("PWR.CELL");   // drawn LAST so it sits cleanly on top of the arc
  drawScanlines();
}

// ── CTRL screen ──────────────────────────────────────────────────────────────
void drawCtrl() {
  M5Dial.Display.fillScreen(BLACK);
  drawHeader("DATA");
  M5Dial.Display.setFont(&fonts::Font2);
  uint16_t fg = fgColor(), dim = dimColor();

  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextSize(1.1f);
  for (int i = 0; i < CTRL_COUNT; i++) {
    int y = 60 + i * 32;
    bool sel = ctrlActive && (i == menuSel);
    String label = String(sel ? "> " : "  ") + CTRL_LABELS[i];
    String value = ctrlValue(i);
    if (sel && ctrlAdjusting) value = "< " + value + " >";

    if (sel) {
      M5Dial.Display.fillRoundRect(20, y - 14, 200, 28, 4, dim);
      M5Dial.Display.setTextColor(BLACK, dim);
    } else {
      M5Dial.Display.setTextColor(ctrlActive ? fg : dim, BLACK);
    }
    M5Dial.Display.setTextDatum(middle_left);
    M5Dial.Display.drawString(label, 30, y);
    M5Dial.Display.setTextDatum(middle_right);
    M5Dial.Display.drawString(value, 210, y);
  }
  M5Dial.Display.setTextSize(1.0f);

  // Bottom hint changes per state
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(dim, BLACK);
  if (!ctrlActive) {
    M5Dial.Display.drawString("PRESS TO EDIT", 120, 200);
  } else if (ctrlAdjusting) {
    M5Dial.Display.drawString("SCROLL / PRESS TO SAVE", 120, 200);
  }

  drawScanlines();
}

// ── INFO screen ──────────────────────────────────────────────────────────────
void drawInfo() {
  M5Dial.Display.fillScreen(BLACK);
  drawHeader("SYS.LOG");
  M5Dial.Display.setFont(&fonts::Font2);
  uint16_t fg = fgColor(), dim = dimColor();

  struct Row { const char* k; String v; };
  Row rows[] = {
    { "DEV",    cfgDevName.length() ? cfgDevName : cfgSerial },
    { "NET",    WiFi.isConnected() ? WiFi.SSID() : String("OFFLINE") },
    { "RSSI",   String(WiFi.RSSI()) + "dBm" },
    { "TEMP",   String(ef.temp) + "C" },
    { "CYC",    String(ef.cycles) },
    { "SOH",    String(ef.soh) + "%" },
  };
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextSize(1.3f);
  for (int i = 0; i < 6; i++) {
    int y = 60 + i * 24;
    M5Dial.Display.setTextColor(dim, BLACK);
    M5Dial.Display.setTextDatum(middle_left);
    M5Dial.Display.drawString(String(">") + rows[i].k, 30, y);
    M5Dial.Display.setTextColor(fg, BLACK);
    M5Dial.Display.setTextDatum(middle_right);
    M5Dial.Display.drawString(rows[i].v, 210, y);
    // dotted separator
    for (int x = 30; x < 210; x += 4) {
      M5Dial.Display.drawPixel(x, y + 12, dim);
    }
  }
  M5Dial.Display.setTextSize(1.0f);

  drawScanlines();
}

// ── RESET screen ─────────────────────────────────────────────────────────────
void drawReset() {
  uint16_t fg = fgColor(), dim = dimColor();
  M5Dial.Display.fillScreen(BLACK);
  drawHeader("RESET");

  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextSize(1.1f);
  for (int i = 0; i < RESET_COUNT; i++) {
    int y = 68 + i * 33;
    bool sel = resetActive && (i == resetSel);
    String label = String(sel ? "> " : "  ") + RESET_LABELS[i];
    if (sel) {
      M5Dial.Display.fillRoundRect(20, y - 14, 200, 28, 4, dim);
      M5Dial.Display.setTextColor(BLACK, dim);
    } else {
      M5Dial.Display.setTextColor(resetActive ? fg : dim, BLACK);
    }
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.drawString(label, 120, y);
  }
  M5Dial.Display.setTextSize(1.0f);

  // Bottom hint
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(dim, BLACK);
  M5Dial.Display.drawString(resetActive ? "PRESS TO WIPE" : "PRESS TO EDIT", 120, 205);
}

void drawConfirm() {
  uint16_t fg = fgColor(), dim = dimColor();
  M5Dial.Display.fillScreen(BLACK);
  drawCorners(dim);

  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextDatum(middle_center);

  // Big header
  M5Dial.Display.setTextSize(1.4f);
  M5Dial.Display.setTextColor(fg, BLACK);
  M5Dial.Display.drawString("CONFIRM", 120, 40);

  // Action title and detail
  M5Dial.Display.setTextSize(1.2f);
  M5Dial.Display.setTextColor(fg, BLACK);
  M5Dial.Display.drawString(confirmTitle, 120, 85);
  M5Dial.Display.setTextSize(1.0f);
  M5Dial.Display.setTextColor(dim, BLACK);
  M5Dial.Display.drawString(confirmDetail, 120, 110);

  // YES / NO buttons side by side
  M5Dial.Display.setTextSize(1.3f);
  int noX = 75, yesX = 165, by = 160, bw = 70, bh = 36;
  if (confirmYes) {
    M5Dial.Display.fillRoundRect(yesX - bw/2, by - bh/2, bw, bh, 5, fg);
    M5Dial.Display.drawRoundRect(noX  - bw/2, by - bh/2, bw, bh, 5, dim);
    M5Dial.Display.setTextColor(BLACK, fg);
    M5Dial.Display.drawString("YES", yesX, by);
    M5Dial.Display.setTextColor(dim, BLACK);
    M5Dial.Display.drawString("NO",  noX,  by);
  } else {
    M5Dial.Display.fillRoundRect(noX  - bw/2, by - bh/2, bw, bh, 5, fg);
    M5Dial.Display.drawRoundRect(yesX - bw/2, by - bh/2, bw, bh, 5, dim);
    M5Dial.Display.setTextColor(BLACK, fg);
    M5Dial.Display.drawString("NO",  noX,  by);
    M5Dial.Display.setTextColor(dim, BLACK);
    M5Dial.Display.drawString("YES", yesX, by);
  }
  M5Dial.Display.setTextSize(1.0f);

  M5Dial.Display.setTextColor(dim, BLACK);
  M5Dial.Display.drawString("SCROLL  /  PRESS", 120, 210);
}

void render() {
  if (confirmActive) { drawConfirm(); return; }
  switch (screen) {
    case STAT:  drawStat();  break;
    case CTRL:  drawCtrl();  break;
    case INFO:  drawInfo();  break;
    case RESET: drawReset(); break;
  }
}

// Validate API keys by hitting the device-list endpoint.
// Returns: 1 = keys OK, 0 = auth/key error, -1 = network/other (don't blame keys)
int verifyApiKeys() {
  String resp = efGet("/iot-open/sign/device/list");
  if (!resp.length()) return -1;                  // no response → network issue
  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) return -1;
  String code = String(doc["code"].as<const char*>());
  DBG("[KEYS] verify code=%s", code.c_str());
  return (code == "0") ? 1 : 0;                    // non-zero code → bad keys
}

// ── Device picker — fetches list, lets user scroll & press to choose ─────────
bool runDevicePicker() {
  M5Dial.Display.fillScreen(BLACK);
  M5Dial.Display.setTextColor(GREEN, BLACK);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.drawString("FETCHING DEVICES...", 120, 120);

  String resp = efGet("/iot-open/sign/device/list");
  if (!resp.length()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok ||
      String(doc["code"].as<const char*>()) != "0") return false;
  JsonArray arr = doc["data"];
  int count = arr.size();
  if (count == 0) return false;

  struct Device { String sn, name; bool online; };
  std::vector<Device> devices;
  for (JsonObject d : arr)
    devices.push_back({ d["sn"].as<String>(),
                        d["deviceName"].as<String>(),
                        d["online"].as<int>() == 1 });

  auto save = [&](int i) {
    cfgSerial  = devices[i].sn;
    cfgDevName = devices[i].name;
    prefs.begin(NVS_NS, false);
    prefs.putString(NVS_SERIAL,   cfgSerial);
    prefs.putString(NVS_DEV_NAME, cfgDevName);
    prefs.end();
  };

  // Only one device → just take it
  if (count == 1) { save(0); return true; }

  // Multi-device picker UI
  int sel = 0;
  long lastEnc = M5Dial.Encoder.read();
  bool needsRedraw = true;

  while (true) {
    M5Dial.update();
    pulseBrightness();

    long enc = M5Dial.Encoder.read();
    long delta = enc - lastEnc;
    if (abs(delta) >= 4) {
      sel = (sel + (delta > 0 ? 1 : -1) + count) % count;
      lastEnc = enc;
      needsRedraw = true;
    }

    if (needsRedraw) {
      M5Dial.Display.fillScreen(BLACK);
      M5Dial.Display.setFont(&fonts::Font2);
      M5Dial.Display.setTextDatum(middle_center);
      M5Dial.Display.setTextSize(1.2f);
      M5Dial.Display.setTextColor(GREEN, BLACK);
      M5Dial.Display.drawString("SELECT DEVICE", 120, 28);
      // dashed separator
      for (int x = 30; x < 210; x += 6)
        M5Dial.Display.drawFastHLine(x, 42, 3, 0x03E0);

      M5Dial.Display.setTextSize(1.0f);
      // Show up to 4 rows centered around the selection
      int firstRow = sel - 1;
      if (firstRow < 0) firstRow = 0;
      if (firstRow > count - 4) firstRow = (count > 4) ? count - 4 : 0;
      int rows = (count < 4) ? count : 4;
      for (int r = 0; r < rows; r++) {
        int i = firstRow + r;
        int y = 70 + r * 30;
        if (i == sel) {
          M5Dial.Display.fillRoundRect(16, y - 13, 208, 26, 4, 0x03E0);
          M5Dial.Display.setTextColor(BLACK, 0x03E0);
        } else {
          M5Dial.Display.setTextColor(GREEN, BLACK);
        }
        M5Dial.Display.setTextDatum(middle_left);
        M5Dial.Display.drawString(devices[i].name, 24, y);
        M5Dial.Display.setTextDatum(middle_right);
        M5Dial.Display.setTextColor(devices[i].online ? GREEN : 0x7800, BLACK);
        M5Dial.Display.drawString(devices[i].online ? "ON" : "OFF", 216, y);
      }

      M5Dial.Display.setTextSize(1.0f);
      M5Dial.Display.setTextDatum(middle_center);
      M5Dial.Display.setTextColor(0x03E0, BLACK);
      M5Dial.Display.drawString("PRESS TO SELECT", 120, 210);
      needsRedraw = false;
    }

    if (M5Dial.BtnA.wasPressed()) {
      save(sel);
      M5Dial.Display.fillScreen(BLACK);
      M5Dial.Display.setTextColor(GREEN, BLACK);
      M5Dial.Display.setTextDatum(middle_center);
      M5Dial.Display.setFont(&fonts::Font2);
      M5Dial.Display.setTextSize(1.2f);
      M5Dial.Display.drawString(devices[sel].name, 120, 120);
      delay(1200);
      return true;
    }
    delay(30);
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  DBG(">>> EcoFlow Dial booting");

  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);
  M5Dial.Display.setRotation(0);
  M5Dial.Display.setBrightness(180);

  // Load API keys: NVS (user-entered) → secrets.h (compiled-in PERSONAL_BUILD)
  bool haveKeys = loadApiKeys();

  // Detect saved WiFi creds via ESP-IDF (works before WiFi.begin)
  WiFi.mode(WIFI_STA);
  wifi_config_t wconf = {};
  esp_wifi_get_config(WIFI_IF_STA, &wconf);
  bool hasSavedWifi = (wconf.sta.ssid[0] != 0);
  DBG("[BOOT] hasSavedWifi=%d ssid=%s haveKeys=%d",
      hasSavedWifi, (const char*)wconf.sta.ssid, haveKeys);

  // If we'll need setup (no WiFi OR no keys), jump to the setup screen.
  if (hasSavedWifi && haveKeys) {
    drawSplash(nullptr);
  } else {
    drawWifiSetup("EcoFlow-Dial", "192.168.4.1");
  }

  WiFiManager wm;
  wm.setConnectTimeout(20);
  wm.setConnectRetries(3);
  wm.setSaveConnect(true);
  wm.setBreakAfterConfig(true);   // return from autoConnect even if first connect fails
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager* wmp) {
    drawWifiSetup("EcoFlow-Dial", WiFi.softAPIP().toString());
  });

  // Inject EcoFlow API-key fields into the captive portal. Pre-fill with any
  // keys we already have so the user can see / edit rather than retype.
  WiFiManagerParameter pAccess("akey", "EcoFlow Access Key", cfgAccessKey.c_str(), 48);
  WiFiManagerParameter pSecret("skey", "EcoFlow Secret Key", cfgSecretKey.c_str(), 48);
  wm.addParameter(&pAccess);
  wm.addParameter(&pSecret);
  wm.setSaveParamsCallback([](){ portalSavedParams = true; });

  wm.autoConnect("EcoFlow-Dial");

  // If the portal collected key fields, persist them
  if (portalSavedParams) {
    if (strlen(pAccess.getValue()) > 0 && strlen(pSecret.getValue()) > 0) {
      saveApiKeys(pAccess.getValue(), pSecret.getValue());
      haveKeys = true;
      DBG("[KEYS] saved from portal");
    }
  }

  // After autoConnect returns, WiFi might still be in the middle of connecting
  // (or about to retry). Just wait — don't call WiFi.begin() again or you get
  // "sta is connecting, cannot set config". Pulse the backlight so the user
  // knows it's alive while we wait.
  if (WiFi.status() != WL_CONNECTED) {
    String savedSsid = wm.getWiFiSSID();
    DBG("[WIFI] waiting on connect, ssid='%s'", savedSsid.c_str());

    M5Dial.Display.fillScreen(BLACK);
    M5Dial.Display.setTextColor(GREEN, BLACK);
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setFont(&fonts::Font2);
    M5Dial.Display.setTextSize(1.2f);
    M5Dial.Display.drawString("CONNECTING TO:", 120, 100);
    M5Dial.Display.drawString(savedSsid, 120, 130);
    M5Dial.Display.setTextSize(1.0f);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
      pulseBrightness();
      delay(200);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    M5Dial.Display.fillScreen(BLACK);
    M5Dial.Display.setTextColor(RED, BLACK);
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setFont(&fonts::Font4);
    M5Dial.Display.drawString("WIFI FAILED", 120, 110);
    M5Dial.Display.setFont(&fonts::Font2);
    M5Dial.Display.drawString("RESTARTING", 120, 140);
    delay(2000);
    ESP.restart();
  }
  DBG("[WIFI] IP=%s", WiFi.localIP().toString().c_str());

  configTime(0, 0, "pool.ntp.org", "time.google.com");
  time_t now = 0;
  for (int i = 0; i < 200 && now < 1000000000UL; i++) {
    pulseBrightness();
    delay(50);
    now = time(nullptr);
  }
  DBG("[NTP] time=%ld", (long)now);

  // ── Ensure we have API keys ────────────────────────────────────────────────
  // WiFi was already saved (so the portal never opened) but keys are missing —
  // e.g. a distribution unit whose WiFi was set but keys never were. Reopen the
  // portal on demand just to collect the keys.
  if (!haveKeys) {
    DBG("[KEYS] missing after connect, opening portal");
    drawApiKeySetup("EcoFlow-Dial", WiFi.softAPIP().toString());
    portalSavedParams = false;
    wm.setAPCallback([](WiFiManager* wmp) {
      drawApiKeySetup("EcoFlow-Dial", WiFi.softAPIP().toString());
    });
    wm.startConfigPortal("EcoFlow-Dial");
    if (strlen(pAccess.getValue()) > 0 && strlen(pSecret.getValue()) > 0) {
      saveApiKeys(pAccess.getValue(), pSecret.getValue());
      haveKeys = true;
    }
    if (!haveKeys) { drawError("NO API KEYS", "RESTARTING", 2500); ESP.restart(); }
  }

  // ── Validate the keys against the EcoFlow API ──────────────────────────────
  {
    int v = verifyApiKeys();
    if (v == 0) {
      // Auth failure — keys are wrong. Wipe them so the portal re-prompts.
      DBG("[KEYS] auth failed, clearing");
      prefs.begin(NVS_NS, false);
      prefs.remove(NVS_ACCESS_KEY);
      prefs.remove(NVS_SECRET_KEY);
      prefs.end();
      drawError("BAD API KEYS", "RE-ENTER", 3000);
      ESP.restart();
    }
    // v == -1 (network) → don't blame keys; continue and let polling retry
  }

  prefs.begin(NVS_NS, true);
  cfgSerial  = prefs.getString(NVS_SERIAL,   "");
  cfgDevName = prefs.getString(NVS_DEV_NAME, "");
  prefs.end();
  if (cfgSerial.length() == 0 && !runDevicePicker()) { delay(2000); ESP.restart(); }

  fetchData();
  render();
  lastEnc    = M5Dial.Encoder.read();
  lastPollMs = millis();
  DBG("[BOOT] complete");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  M5Dial.update();
  pulseBrightness();   // subtle backlight breathe — same waveform as splash

  // Encoder: 4 ticks per detent on this encoder; use threshold of 4
  long enc = M5Dial.Encoder.read();
  long delta = enc - lastEnc;
  if (abs(delta) >= 4) {
    int dir = (delta > 0) ? 1 : -1;
    lastEnc = enc;
    if (confirmActive) {
      // Modal: scroll toggles YES/NO
      confirmYes = !confirmYes;
    } else if (screen == CTRL && ctrlActive && ctrlAdjusting) {
      // Adjust the highlighted control's value
      adjustCurrent(dir);
    } else if (screen == CTRL && ctrlActive) {
      // Row-select mode: scroll changes row, scrolling past edges exits CTRL
      int next = menuSel + dir;
      if (next < 0) {
        screen = STAT;
        ctrlActive = false;
      } else if (next >= CTRL_COUNT) {
        screen = INFO;
        ctrlActive = false;
      } else {
        menuSel = next;
      }
    } else if (screen == RESET && resetActive) {
      // Reset menu row-select: scroll cycles options, edges exit
      int next = resetSel + dir;
      if (next < 0) {
        screen = INFO;
        resetActive = false;
      } else if (next >= RESET_COUNT) {
        screen = STAT;            // wraps past last reset option back to STAT
        resetActive = false;
      } else {
        resetSel = next;
      }
    } else {
      // STAT, INFO, or non-active CTRL/RESET: cycle screens
      int next = (int)screen + dir;
      screen = (Screen)((next + SCREEN_COUNT) % SCREEN_COUNT);
    }
    M5Dial.Speaker.tone(4000, 15);
    needsDraw = true;
  }

  // Button
  if (M5Dial.BtnA.wasPressed()) {
    M5Dial.Speaker.tone(2000, 30);
    if (confirmActive) {
      // Modal: press executes if YES, cancels if NO
      void (*fn)() = confirmFn;
      bool yes = confirmYes;
      confirmActive = false;
      confirmFn = nullptr;
      ctrlAdjusting = false;
      if (yes && fn) {
        fn();         // may not return (e.g., doReset → ESP.restart)
        delay(500);
      }
      fetchData();    // resync local values (also corrects cancelled AC toggle)
    } else if (screen == CTRL && ctrlActive && ctrlAdjusting) {
      // Press while adjusting → commit the new value and exit adjust mode
      commitCurrent();
      ctrlAdjusting = false;
      // If commitCurrent opened a confirmation modal, leave it on screen.
      // Otherwise: API was sent immediately (MAX CHARGE / FAST CHG) → refetch.
      if (!confirmActive) {
        delay(500);
        fetchData();
      }
    } else if (screen == CTRL && ctrlActive) {
      // Press a highlighted row → enter adjust mode for that row
      ctrlAdjusting = true;
    } else if (screen == CTRL) {
      // First press on inactive CTRL → enter row-select mode
      ctrlActive = true;
      menuSel = 0;
    } else if (screen == RESET && resetActive) {
      // Press an active reset row → confirm the specific wipe
      void (*fn)() = (resetSel == 0) ? doResetWifi
                   : (resetSel == 1) ? doResetDevice
                   : (resetSel == 2) ? doResetKeys
                                     : doResetFactory;
      enterConfirm(String("RESET ") + RESET_LABELS[resetSel],
                   RESET_DETAILS[resetSel], fn);
    } else if (screen == RESET) {
      // First press on inactive RESET → enter row-select mode
      resetActive = true;
      resetSel = 0;
    } else {
      // Press from STAT or INFO → jump into CTRL row-select mode
      screen = CTRL;
      ctrlActive = true;
      menuSel = 0;
    }
    needsDraw = true;
  }

  // Periodic poll
#if SIM_MODE
  // Fast tick so the SOC sweep is visible (every 150ms = ~1%/0.15s)
  if (millis() - lastPollMs >= 150) {
    simData();
    lastPollMs = millis();
    needsDraw = true;
  }
#else
  if (millis() - lastPollMs >= POLL_INTERVAL_MS) {
    fetchData();
    lastPollMs = millis();
    needsDraw = true;
  }
#endif

  if (needsDraw) { render(); needsDraw = false; }
  delay(20);
}
