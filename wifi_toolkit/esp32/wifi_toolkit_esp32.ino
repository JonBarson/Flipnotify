/*
 * WiFi Toolkit - ESP32 firmware for the official Flipper Zero WiFi Devboard (ESP32-S2).
 *
 * Speaks a simple newline-terminated text protocol with the Flipper over UART0 (Serial).
 * Scope v0.1: passive WiFi scan + connection management only. No offensive features.
 *
 * Flash with Arduino IDE (board: "ESP32S2 Dev Module") or arduino-cli / esptool.
 */
#include <WiFi.h>
#include <Preferences.h>

static const char* FW_VERSION = "0.1.0";
static const char* PROTO_VERSION = "1";
static const uint32_t UART_BAUD = 115200;
static const int MAX_SAVED = 8;
static const size_t RX_MAX = 256;

Preferences prefs;
String rx;
bool autoConnectEnabled = true;

static void line(const String& s) { Serial.print(s); Serial.print('\n'); }

static String sanitize(String s) {
  s.replace('|', ' ');
  s.replace('\n', ' ');
  s.replace('\r', ' ');
  return s;
}

static String secStr(wifi_auth_mode_t m) {
  switch(m) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2E";
    default: return "?";
  }
}

// ---------- saved credentials (NVS) ----------
static int savedCount() { return prefs.getInt("cnt", 0); }
static void setSavedCount(int c) { prefs.putInt("cnt", c); }
static String kS(int i) { return String("s") + i; }
static String kP(int i) { return String("p") + i; }
static String kA(int i) { return String("a") + i; }

static int savedIndex(const String& ssid) {
  int c = savedCount();
  for(int i = 0; i < c; i++) {
    if(prefs.getString(kS(i).c_str(), "") == ssid) return i;
  }
  return -1;
}

static void saveCred(const String& ssid, const String& pass) {
  int idx = savedIndex(ssid);
  if(idx < 0) {
    idx = savedCount();
    if(idx >= MAX_SAVED) return; // full
    setSavedCount(idx + 1);
  }
  prefs.putString(kS(idx).c_str(), ssid);
  prefs.putString(kP(idx).c_str(), pass);
  prefs.putInt(kA(idx).c_str(), 1);
}

static void deleteCred(const String& ssid) {
  int idx = savedIndex(ssid);
  if(idx < 0) return;
  int c = savedCount();
  for(int i = idx; i < c - 1; i++) {
    prefs.putString(kS(i).c_str(), prefs.getString(kS(i + 1).c_str(), ""));
    prefs.putString(kP(i).c_str(), prefs.getString(kP(i + 1).c_str(), ""));
    prefs.putInt(kA(i).c_str(), prefs.getInt(kA(i + 1).c_str(), 1));
  }
  setSavedCount(c - 1);
}

// ---------- commands ----------
static void cmdScan() {
  int n = WiFi.scanNetworks(false, true); // blocking, show hidden
  if(n < 0) { line("SCAN_ERROR"); return; }
  line(String("SCAN_BEGIN|") + n);
  for(int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if(ssid.length() == 0) ssid = "<hidden>";
    line(String("NET|") + sanitize(ssid) + "|" + WiFi.BSSIDstr(i) + "|" +
         WiFi.RSSI(i) + "|" + WiFi.channel(i) + "|" + secStr(WiFi.encryptionType(i)));
  }
  line("SCAN_END");
  WiFi.scanDelete();
}

static void sendStatus() {
  if(WiFi.status() == WL_CONNECTED) {
    line(String("STATUS|CONNECTED|") + sanitize(WiFi.SSID()) + "|" +
         WiFi.localIP().toString() + "|" + WiFi.RSSI() + "|" + WiFi.channel() + "|" +
         WiFi.gatewayIP().toString() + "|" + WiFi.dnsIP().toString());
  } else {
    line("STATUS|DISCONNECTED");
  }
}

static void cmdConnect(const String& ssid, const String& pass) {
  line("STATUS|CONNECTING");
  WiFi.disconnect();
  delay(50);
  if(pass.length() > 0) WiFi.begin(ssid.c_str(), pass.c_str());
  else WiFi.begin(ssid.c_str());
  uint32_t t0 = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  if(WiFi.status() == WL_CONNECTED) {
    saveCred(ssid, pass);
    prefs.putString("last", ssid);
    sendStatus();
  } else {
    line("STATUS|ERROR|AUTH_FAILED");
    WiFi.disconnect();
  }
}

static void cmdRssi(const String& bssid) {
  if(WiFi.status() == WL_CONNECTED && WiFi.BSSIDstr() == bssid) {
    line(String("RSSI|") + bssid + "|" + WiFi.RSSI());
    return;
  }
  int n = WiFi.scanNetworks(false, true);
  int val = 0; bool found = false;
  for(int i = 0; i < n; i++) {
    if(WiFi.BSSIDstr(i) == bssid) { val = WiFi.RSSI(i); found = true; break; }
  }
  WiFi.scanDelete();
  if(found) line(String("RSSI|") + bssid + "|" + val);
  else line(String("RSSI|") + bssid + "|NA");
}

static void cmdSavedList() {
  int c = savedCount();
  line(String("SAVED_BEGIN|") + c);
  for(int i = 0; i < c; i++) {
    line(String("SAVED|") + sanitize(prefs.getString(kS(i).c_str(), "")) + "|" +
         prefs.getInt(kA(i).c_str(), 1));
  }
  line("SAVED_END");
}

static void process(String s) {
  s.trim();
  if(s.length() == 0) return;
  int p1 = s.indexOf('|');
  String cmd = p1 < 0 ? s : s.substring(0, p1);
  String rest = p1 < 0 ? "" : s.substring(p1 + 1);

  if(cmd == "PING") {
    line(String("PONG|") + PROTO_VERSION + "|" + FW_VERSION);
  } else if(cmd == "SCAN") {
    cmdScan();
  } else if(cmd == "CONNECT") {
    int q = rest.indexOf('|');
    String ssid = q < 0 ? rest : rest.substring(0, q);
    String pass = q < 0 ? "" : rest.substring(q + 1);
    cmdConnect(ssid, pass);
  } else if(cmd == "DISCONNECT") {
    WiFi.disconnect();
    line("STATUS|DISCONNECTED");
  } else if(cmd == "STATUS") {
    sendStatus();
  } else if(cmd == "RSSI") {
    cmdRssi(rest);
  } else if(cmd == "SAVED_LIST") {
    cmdSavedList();
  } else if(cmd == "SAVED_DELETE") {
    deleteCred(rest);
    line(String("SAVED_DELETED|") + sanitize(rest));
  } else if(cmd == "AUTOCONNECT") {
    autoConnectEnabled = (rest == "1");
    prefs.putInt("auto", autoConnectEnabled ? 1 : 0);
    line(String("AUTOCONNECT|") + (autoConnectEnabled ? "1" : "0"));
  } else if(cmd == "VERSION") {
    line(String("VERSION|") + PROTO_VERSION + "|" + FW_VERSION);
  } else {
    line(String("ERROR|UNKNOWN_CMD|") + sanitize(cmd));
  }
}

void setup() {
  Serial.begin(UART_BAUD);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setAutoReconnect(true);
  prefs.begin("wifitk", false);
  autoConnectEnabled = prefs.getInt("auto", 1) != 0;
  rx.reserve(RX_MAX);

  // Optional: reconnect to last saved network on boot.
  if(autoConnectEnabled) {
    String last = prefs.getString("last", "");
    int idx = last.length() ? savedIndex(last) : -1;
    if(idx >= 0 && prefs.getInt(kA(idx).c_str(), 1) != 0) {
      String pass = prefs.getString(kP(idx).c_str(), "");
      if(pass.length() > 0) WiFi.begin(last.c_str(), pass.c_str());
      else WiFi.begin(last.c_str());
    }
  }
}

void loop() {
  while(Serial.available() > 0) {
    char c = (char)Serial.read();
    if(c == '\r') continue;
    if(c == '\n') {
      String cur = rx;
      rx = "";
      process(cur);
    } else {
      if(rx.length() < RX_MAX) rx += c;
      else rx = ""; // overflow: drop the malformed line, keep buffer bounded
    }
  }
}
