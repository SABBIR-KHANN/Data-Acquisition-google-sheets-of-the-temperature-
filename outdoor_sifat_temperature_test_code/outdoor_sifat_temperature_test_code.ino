// ================================================================
//  Outdoor Temperature → Google Sheets  (ESP8266)
//
//  Features:
//    • DS18B20 temperature sensor, sends every 5 minutes
//    • NTP time sync (UTC+0) — every reading gets exact timestamp
//    • 3-WiFi multi-network: picks strongest with internet
//    • LittleFS offline buffer:
//        - Saves readings to flash when WiFi/internet is down
//        - Timestamps are stored with each reading
//        - On reconnect: sends ALL buffered readings with original
//          timestamps → Apps Script fills the exact gap rows
//        - Buffer survives power cuts (flash memory)
//
//  Credentials → see the  credentials.h  tab
// ================================================================

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LittleFS.h>
#include <WiFiUDP.h>
#include <NTPClient.h>
#include "credentials.h"

// -------- DS18B20 Sensor --------
#define ONE_WIRE_BUS D4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// -------- NTP Time (UTC+0) --------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0);

// -------- Time Tracking --------
bool          ntpSynced      = false;
unsigned long ntpEpochAtSync = 0;
unsigned long millisAtSync   = 0;

// -------- Timing --------
unsigned long lastGoogleSend = 0;
unsigned long lastNTPRefresh = 0;
unsigned long lastWifiRetry  = 0;

const unsigned long SEND_INTERVAL  = 300000UL;   // 5 minutes
const unsigned long NTP_INTERVAL   = 3600000UL;  // 1 hour
const unsigned long WIFI_RETRY_GAP = 60000UL;    // 1 minute between wifi retries

// -------- Buffer file on Flash --------
#define BUFFER_FILE        "/buffer.csv"
#define BUFFER_TMP_FILE    "/buf_tmp.csv"
#define MAX_BUFFER_ENTRIES 500   // ~41 hours at 5-min intervals

// ================================================================
//  getCurrentEpoch()
//  Returns current Unix time using millis() offset from last sync.
//  Works without WiFi as long as power is on.
// ================================================================
unsigned long getCurrentEpoch() {
  if (!ntpSynced) return 0;
  unsigned long elapsed = (millis() - millisAtSync) / 1000UL;
  return ntpEpochAtSync + elapsed;
}

// ================================================================
//  syncNTP()
//  Tries up to 3 times before giving up.
//  Always updates ntpEpochAtSync + millisAtSync on success.
// ================================================================
void syncNTP() {
  timeClient.begin();
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("  NTP attempt %d/3 ... ", attempt);
    if (timeClient.update()) {
      ntpEpochAtSync = timeClient.getEpochTime();
      millisAtSync   = millis();
      ntpSynced      = true;
      Serial.printf("✅ synced: epoch=%lu  time=%s\n",
                    ntpEpochAtSync,
                    timeClient.getFormattedTime().c_str());
      return;
    }
    Serial.println("failed");
    delay(2000);
  }
  Serial.println("  ⚠️  NTP sync failed after 3 attempts — will retry later");
}

// ================================================================
//  hasInternet()
//  Quick check with explicit timeout so it never hangs.
// ================================================================
bool hasInternet() {
  WiFiClientSecure testClient;
  testClient.setInsecure();
  testClient.setTimeout(5000);   // 5 second hard timeout
  HTTPClient http;
  bool ok = false;
  if (http.begin(testClient, "https://www.google.com")) {
    int code = http.GET();
    ok = (code > 0);
    http.end();
  }
  return ok;
}

// ================================================================
//  connectBestWifi()
//  Scans all networks, sorts by RSSI, tries each in order.
//  Picks the first one that has real internet.
//  Syncs NTP immediately after a successful connection.
// ================================================================
void connectBestWifi() {
  Serial.println("\n--- WiFi Scan ---");

  int rssiOf[WIFI_COUNT];
  for (int k = 0; k < WIFI_COUNT; k++) rssiOf[k] = -1000;

  int n = WiFi.scanNetworks();
  Serial.printf("  Networks visible: %d\n", n);

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int    rssi = WiFi.RSSI(i);
    for (int k = 0; k < WIFI_COUNT; k++) {
      if (ssid == WIFI_SSID[k] && rssi > rssiOf[k])
        rssiOf[k] = rssi;
    }
  }
  WiFi.scanDelete();

  // Sort by RSSI descending (strongest first)
  int order[WIFI_COUNT] = {0, 1, 2};
  for (int i = 0; i < WIFI_COUNT - 1; i++)
    for (int j = 0; j < WIFI_COUNT - i - 1; j++)
      if (rssiOf[order[j]] < rssiOf[order[j+1]]) {
        int t = order[j]; order[j] = order[j+1]; order[j+1] = t;
      }

  for (int i = 0; i < WIFI_COUNT; i++) {
    int k = order[i];

    if (rssiOf[k] == -1000) {
      Serial.printf("  [%d] %-20s — not in range\n", k+1, WIFI_SSID[k]);
      continue;
    }

    Serial.printf("  [%d] %-20s  RSSI: %d dBm — connecting...\n",
                  k+1, WIFI_SSID[k], rssiOf[k]);

    WiFi.disconnect();
    delay(200);
    WiFi.begin(WIFI_SSID[k], WIFI_PASS[k]);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("  ❌ Could not connect to '%s'\n", WIFI_SSID[k]);
      continue;
    }

    Serial.print("  Checking internet ... ");
    if (hasInternet()) {
      Serial.printf("✅ Online via '%s'  IP: %s\n",
                    WIFI_SSID[k],
                    WiFi.localIP().toString().c_str());
      syncNTP();
      lastNTPRefresh = millis();  // reset NTP refresh timer
      return;
    } else {
      Serial.printf("⚠️  '%s' has no internet — trying next\n", WIFI_SSID[k]);
    }
  }

  Serial.println("  ❌ No WiFi with internet found.");
}

// ================================================================
//  saveToBuffer()
//  Appends one "timestamp,temp" line to flash.
// ================================================================
void saveToBuffer(unsigned long ts, float temp) {
  File f = LittleFS.open(BUFFER_FILE, "a");
  if (!f) {
    Serial.println("  ❌ Cannot open buffer file");
    return;
  }
  f.printf("%lu,%.2f\n", ts, temp);
  f.close();
  Serial.printf("  📝 Buffered → ts=%lu  temp=%.2f°C\n", ts, temp);
}

// ================================================================
//  sendOneReading()
//  Sends one reading with its original timestamp to Sheets.
//  Returns true on any HTTP response (success or redirect).
// ================================================================
bool sendOneReading(unsigned long ts, float temp) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  secureClient.setTimeout(8000);   // 8 second timeout per send
  HTTPClient https;

  String url = String(GOOGLE_SCRIPT_URL)
               + "?sensor=2&temp=" + String(temp, 2)
               + "&ts="            + String(ts);

  if (https.begin(secureClient, url)) {
    int code = https.GET();
    https.end();
    return (code > 0);
  }
  return false;
}

// ================================================================
//  flushBuffer()
//  Sends all stored readings to Google Sheets with original
//  timestamps. Stops immediately if WiFi drops mid-flush.
//  Failed sends are re-buffered for next time.
// ================================================================
void flushBuffer() {
  if (!LittleFS.exists(BUFFER_FILE)) return;

  File f = LittleFS.open(BUFFER_FILE, "r");
  if (!f) return;

  Serial.println("\n📤 Flushing offline buffer → Google Sheets");

  struct Entry { unsigned long ts; float temp; };
  static Entry entries[MAX_BUFFER_ENTRIES];
  int count = 0;

  while (f.available() && count < MAX_BUFFER_ENTRIES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int comma = line.indexOf(',');
    if (comma < 0) continue;
    entries[count].ts   = (unsigned long)line.substring(0, comma).toInt();
    entries[count].temp = line.substring(comma + 1).toFloat();
    count++;
  }
  f.close();

  if (count == 0) {
    LittleFS.remove(BUFFER_FILE);
    return;
  }

  Serial.printf("  %d readings to send\n", count);

  File tmp = LittleFS.open(BUFFER_TMP_FILE, "w");
  int sent = 0, failed = 0;

  for (int i = 0; i < count; i++) {

    // Stop immediately if WiFi drops — re-buffer everything remaining
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n  ⚠️  WiFi lost during flush — stopping early");
      for (int j = i; j < count; j++) {
        if (tmp) tmp.printf("%lu,%.2f\n", entries[j].ts, entries[j].temp);
        failed++;
      }
      break;
    }

    Serial.printf("  [%d/%d] ts=%lu  %.2f°C ... ",
                  i+1, count, entries[i].ts, entries[i].temp);

    if (sendOneReading(entries[i].ts, entries[i].temp)) {
      Serial.println("✅");
      sent++;
      delay(600);   // small gap to avoid overloading Google's servers
    } else {
      Serial.println("❌ re-buffered");
      if (tmp) tmp.printf("%lu,%.2f\n", entries[i].ts, entries[i].temp);
      failed++;
    }
  }

  if (tmp) tmp.close();

  LittleFS.remove(BUFFER_FILE);
  if (failed > 0) {
    LittleFS.rename(BUFFER_TMP_FILE, BUFFER_FILE);
    Serial.printf("  ⚠️  %d sent, %d re-buffered for next time\n", sent, failed);
  } else {
    LittleFS.remove(BUFFER_TMP_FILE);
    Serial.printf("  ✅ All %d readings sent — buffer cleared\n", sent);
  }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n==========================================");
  Serial.println("  Outdoor Temp → Google Sheets + Buffer  ");
  Serial.println("  ESP8266 | DS18B20 | LittleFS | NTP     ");
  Serial.println("==========================================");

  // Start LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS failed — formatting flash...");
    LittleFS.format();
    LittleFS.begin();
  }
  Serial.println("✅ LittleFS ready");

  // Show existing buffer size if any
  if (LittleFS.exists(BUFFER_FILE)) {
    File f = LittleFS.open(BUFFER_FILE, "r");
    if (f) {
      Serial.printf("📂 Buffer file found (%d bytes)\n", f.size());
      f.close();
    }
  }

  sensors.begin();
  WiFi.mode(WIFI_STA);
  connectBestWifi();

  // On boot: flush any readings stored during previous outage
  if (WiFi.status() == WL_CONNECTED) {
    flushBuffer();
  }

  // Trigger first reading immediately on boot
  lastGoogleSend = millis() - SEND_INTERVAL;
}

// ================================================================
//  LOOP
// ================================================================
void loop() {

  // ---- WiFi Watchdog ----
  // Only retry every 60 seconds — avoids hammering the radio
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiRetry >= WIFI_RETRY_GAP) {
      lastWifiRetry = millis();
      Serial.println("\n⚠️  WiFi lost — scanning for best network...");
      connectBestWifi();
      if (WiFi.status() == WL_CONNECTED) {
        flushBuffer();   // send all stored readings immediately
      }
    }
  }

  // ---- Refresh NTP every hour (proper re-sync, not just update) ----
  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastNTPRefresh >= NTP_INTERVAL) {
      lastNTPRefresh = millis();
      syncNTP();
    }
  }

  // ---- Read & Send Temperature every 5 minutes ----
  if (millis() - lastGoogleSend >= SEND_INTERVAL) {
    lastGoogleSend = millis();

    sensors.requestTemperatures();
    float tempC = sensors.getTempCByIndex(0);

    // -100 is safer threshold — DS18B20 reads down to -55°C
    if (tempC == DEVICE_DISCONNECTED_C || tempC < -100) {
      Serial.println("❌ Sensor error — check wiring. Skipping.");
    } else {
      Serial.printf("\n🌡️  Temperature: %.2f °C\n", tempC);
      unsigned long ts = getCurrentEpoch();

      if (ts == 0) {
        // No NTP yet — if online try to sync once more, then skip
        Serial.println("  ⚠️  No time sync — attempting NTP before skip...");
        if (WiFi.status() == WL_CONNECTED) syncNTP();
        ts = getCurrentEpoch();
        if (ts == 0) {
          Serial.println("  ❌ Still no time sync — reading skipped");
        }
      }

      if (ts != 0) {
        if (WiFi.status() == WL_CONNECTED) {
          if (!sendOneReading(ts, tempC)) {
            Serial.println("  Send failed — saving to buffer");
            saveToBuffer(ts, tempC);
          }
        } else {
          saveToBuffer(ts, tempC);
          Serial.println("  No WiFi — saved to buffer");
        }
      }
    }
  }

  delay(1000);  // 1-second heartbeat
}