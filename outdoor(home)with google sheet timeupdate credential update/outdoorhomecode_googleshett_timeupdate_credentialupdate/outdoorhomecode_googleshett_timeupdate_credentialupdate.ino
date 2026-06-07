#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <EEPROM.h>

const char* GOOGLE_SCRIPT_URL = "*********************************************";
unsigned long lastGoogleSend = 0;
const unsigned long GOOGLE_SEND_INTERVAL = 300000UL;

// ---------------- EEPROM LAYOUT ----------------
#define EEPROM_SIZE  130
#define ADDR_SSID    0
#define ADDR_PASS    32
#define ADDR_MQTT    96
#define ADDR_FLAG    129
#define VALID_FLAG   0xAB

// ---------------- CREDENTIALS ----------------
char stored_ssid[32]     = "****************";
char stored_password[64] = "*************";
char stored_mqtt[32]     = "************";

// ---------------- WIFI + MQTT ----------------
WiFiClient espClient;
PubSubClient client(espClient);
const int mqtt_port = 1883;
const char* mqtt_control_topic = "home/relay/cmd";

// ---------------- TIME CONFIG ----------------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 6 * 3600);

// ---------------- SENSOR PINS ----------------
#define ONE_WIRE_BUS D4
#define PIR_PIN      D5
#define PIR_PIN2     D6
#define RELAY_PIN    D2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ---------------- NIGHT TIME CONFIG ----------------
int nightStartHour = 22;
int nightStopHour  = 5;

// ---------------- NEW: RELAY ACTIVE FLAG ----------------
// When false, the relay is fully deactivated (PIR and timer won't trigger it)
bool relayActive = true;

// ---------------- VARIABLES ----------------
bool relayState = false;
unsigned long relayOffTime = 0;
int extendMode = 0;
unsigned long lastPublish = 0;
unsigned long lastMotionTime = 0;
unsigned long lastMqttCheck = 0;
const unsigned long mqttCheckInterval = 5000;
String serialBuffer = "";

// ---------------- EEPROM: SAVE ----------------
void saveCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
  for (int i = 0; i < strlen(stored_ssid);     i++) EEPROM.write(ADDR_SSID  + i, stored_ssid[i]);
  for (int i = 0; i < strlen(stored_password); i++) EEPROM.write(ADDR_PASS  + i, stored_password[i]);
  for (int i = 0; i < strlen(stored_mqtt);     i++) EEPROM.write(ADDR_MQTT  + i, stored_mqtt[i]);
  EEPROM.write(ADDR_FLAG, VALID_FLAG);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("✅ Credentials saved to EEPROM.");
}

// ---------------- EEPROM: LOAD ----------------
void loadCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  byte flag = EEPROM.read(ADDR_FLAG);
  if (flag == VALID_FLAG) {
    for (int i = 0; i < 31; i++) stored_ssid[i]      = EEPROM.read(ADDR_SSID + i);
    stored_ssid[31] = '\0';
    for (int i = 0; i < 63; i++) stored_password[i]  = EEPROM.read(ADDR_PASS + i);
    stored_password[63] = '\0';
    for (int i = 0; i < 31; i++) stored_mqtt[i]      = EEPROM.read(ADDR_MQTT + i);
    stored_mqtt[31] = '\0';
    Serial.println("✅ Credentials loaded from EEPROM.");
  } else {
    Serial.println("ℹ️  No saved credentials — using defaults.");
  }
  EEPROM.end();
}

// ---------------- BLOCKING LINE READ ----------------
String readLineBlocking(unsigned long timeoutMs = 60000) {
  String line = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (line.length() > 0) break;
      } else {
        line += c;
        Serial.print(c);
      }
    }
  }
  Serial.println();
  return line;
}

// ---------------- PARSE TIME INPUT ----------------
int parseHourInput(String input) {
  input.trim();
  input.toUpperCase();
  int hour = -1;
  if (input.endsWith("PM")) {
    hour = input.substring(0, input.length() - 2).toInt();
    if (hour != 12) hour += 12;
    if (hour >= 24) hour = -1;
  } else if (input.endsWith("AM")) {
    hour = input.substring(0, input.length() - 2).toInt();
    if (hour == 12) hour = 0;
    if (hour < 0 || hour > 11) hour = -1;
  } else {
    hour = input.toInt();
    if (hour < 0 || hour > 23) hour = -1;
  }
  return hour;
}

// ---------------- NEW: RELAY ACTIVATE/DEACTIVATE FLOW ----------------
void updateRelayActive() {
  Serial.println("-------------------------------------------");
  Serial.println("Do you want to deactivate the light? (yes/no)");
  Serial.print("  Current -> Light is : ");
  Serial.println(relayActive ? "ACTIVATED" : "DEACTIVATED");
  Serial.print("> ");

  String answer = readLineBlocking(30000);
  answer.trim();
  answer.toLowerCase();

  if (answer == "yes") {
    // Deactivate: turn relay off and disable it
    relayActive = false;
    relayState = false;
    digitalWrite(RELAY_PIN, HIGH);
    relayOffTime = 0;
    extendMode = 0;
    if (client.connected()) client.publish("home/light", "Light DEACTIVATED");
    Serial.println("🔴 Light DEACTIVATED — relay will not respond to motion or timer.");
  } else if (answer == "no") {
    // If currently deactivated, offer to reactivate
    if (!relayActive) {
      Serial.println("Do you want to re-activate the light? (yes/no)");
      Serial.print("> ");
      String reAnswer = readLineBlocking(30000);
      reAnswer.trim();
      reAnswer.toLowerCase();
      if (reAnswer == "yes") {
        relayActive = true;
        if (client.connected()) client.publish("home/light", "Light ACTIVATED");
        Serial.println("🟢 Light ACTIVATED — relay will respond to motion again.");
      } else {
        Serial.println("Keeping light DEACTIVATED.");
      }
    } else {
      Serial.println("Light remains ACTIVATED.");
    }
  } else {
    Serial.println("❌ Invalid input — no changes made.");
  }

  Serial.println("-------------------------------------------");
}

// ---------------- TIME UPDATE FLOW ----------------
void updateTimes() {
  Serial.println("-------------------------------------------");
  Serial.println("Do you want to update the relay working time? (yes/no)");
  Serial.print("  Current -> Night START : ");
  Serial.print(nightStartHour); Serial.println(":00");
  Serial.print("  Current -> Night STOP  : ");
  Serial.print(nightStopHour);  Serial.println(":00");
  Serial.print("> ");

  String timeAnswer = readLineBlocking(15000);
  timeAnswer.trim();
  timeAnswer.toLowerCase();

  if (timeAnswer == "yes") {
    Serial.print("  Night START (e.g. 9PM or 21): ");
    String startVal = readLineBlocking(30000);
    int parsedStart = parseHourInput(startVal);
    if (parsedStart >= 0) {
      nightStartHour = parsedStart;
      Serial.print("✅ Night starts at: ");
      Serial.print(nightStartHour); Serial.println(":00");
    } else {
      Serial.println("❌ Invalid — keeping old value.");
    }

    Serial.print("  Morning STOP (e.g. 5AM or 5): ");
    String stopVal = readLineBlocking(30000);
    int parsedStop = parseHourInput(stopVal);
    if (parsedStop >= 0) {
      nightStopHour = parsedStop;
      Serial.print("✅ Night stops at: ");
      Serial.print(nightStopHour); Serial.println(":00");
    } else {
      Serial.println("❌ Invalid — keeping old value.");
    }
  } else {
    Serial.println("Skipping time update.");
  }
  Serial.println("-------------------------------------------");
}

// ---------------- CREDENTIAL UPDATE FLOW ----------------
void updateCredentials() {
  Serial.println("===========================================");
  Serial.println("  CREDENTIAL UPDATE                       ");
  Serial.println("===========================================");

  Serial.print("  Enter SSID     : ");
  String newSSID = readLineBlocking();

  Serial.print("  Enter Password : ");
  String newPass = readLineBlocking();

  Serial.print("  Enter MQTT IP  : ");
  String newMQTT = readLineBlocking();

  if (newSSID.length() == 0 || newPass.length() == 0 || newMQTT.length() == 0) {
    Serial.println("❌ Update cancelled — fields cannot be empty.");
    updateRelayActive();
    updateTimes();
    return;
  }

  newSSID.toCharArray(stored_ssid,     32);
  newPass.toCharArray(stored_password, 64);
  newMQTT.toCharArray(stored_mqtt,     32);
  saveCredentials();

  Serial.println("-------------------------------------------");
  Serial.print("  SSID     : "); Serial.println(stored_ssid);
  Serial.print("  Password : "); Serial.println(stored_password);
  Serial.print("  MQTT IP  : "); Serial.println(stored_mqtt);
  Serial.println("✅ Credentials saved!");

  // Reconnect WiFi
  WiFi.disconnect();
  delay(500);
  WiFi.begin(stored_ssid, stored_password);
  Serial.print("  Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi reconnected!");
    client.setServer(stored_mqtt, mqtt_port);
  } else {
    Serial.println("\n❌ WiFi failed — check credentials.");
  }

  // Ask relay activate/deactivate, then time update
  updateRelayActive();
  updateTimes();
}

// ---------------- NON-BLOCKING SERIAL HANDLER ----------------
void handleSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() == 0) continue;

      String line = serialBuffer;
      serialBuffer = "";
      line.trim();

      // ---- UPDATE COMMAND ----
      if (line.equalsIgnoreCase("update")) {
        Serial.println();
        Serial.println("Do you want to update credentials? (yes/no)");
        Serial.print("> ");
        String confirm = readLineBlocking(30000);
        confirm.trim();
        confirm.toLowerCase();
        if (confirm == "yes") {
          updateCredentials();  // includes relay + time update inside
        } else {
          Serial.println("Skipping credential update.");
          updateRelayActive();  // ask relay activate/deactivate
          updateTimes();        // ask time update
        }
      }

      // ---- STATUS ----
      else if (line.equalsIgnoreCase("status")) {
        Serial.println("-------------------------------------------");
        Serial.println("  Current Config                           ");
        Serial.println("-------------------------------------------");
        Serial.print("  SSID      : "); Serial.println(stored_ssid);
        Serial.print("  MQTT IP   : "); Serial.println(stored_mqtt);
        Serial.print("  Night ON  : "); Serial.print(nightStartHour); Serial.println(":00");
        Serial.print("  Night OFF : "); Serial.print(nightStopHour);  Serial.println(":00");
        Serial.print("  Light     : "); Serial.println(relayActive ? "ACTIVATED" : "DEACTIVATED");
        Serial.print("  WiFi      : "); Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
        Serial.print("  MQTT      : "); Serial.println(client.connected() ? "Connected" : "Disconnected");
        Serial.println("-------------------------------------------");
      }

      // ---- UNKNOWN ----
      else {
        Serial.println("❓ Unknown command.");
        Serial.println("   Commands: update | status");
      }

    } else {
      serialBuffer += c;
    }
  }
}

// ---------------- MQTT CALLBACK ----------------
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) message += (char)payload[i];
  Serial.print("MQTT ["); Serial.print(topic); Serial.print("]: ");
  Serial.println(message);

  if (message == "1") {
    if (!relayActive) {
      Serial.println("⚠️  Relay is DEACTIVATED — ignoring ON command.");
      if (client.connected()) client.publish("home/light", "Relay DEACTIVATED — command ignored");
      return;
    }
    relayState = true;
    digitalWrite(RELAY_PIN, LOW);
    relayOffTime = 0;
    extendMode = 0;
    client.publish("home/light", "Light turned ON (Manual)");
    Serial.println("Manual: Relay ON");
  } else if (message == "0") {
    relayState = false;
    digitalWrite(RELAY_PIN, HIGH);
    relayOffTime = 0;
    extendMode = 0;
    client.publish("home/light", "Light turned OFF (Manual)");
    Serial.println("Manual: Relay OFF");
  }
}

// ---------------- WIFI CONNECT ----------------
void setup_wifi() {
  WiFi.begin(stored_ssid, stored_password);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected!");
  } else {
    Serial.println("\n❌ WiFi failed — running offline (PIR/relay active)");
  }
}

// ---------------- MQTT CHECK ----------------
void checkMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.print("MQTT check...");
  if (client.connect("ESP8266_PIR_Temp")) {
    Serial.println("connected");
    client.subscribe(mqtt_control_topic);
  } else {
    Serial.print("failed, rc="); Serial.println(client.state());
  }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(500);

  loadCredentials();

  Serial.println("===========================================");
  Serial.println("  System starting...                      ");
  Serial.println("  Type  update  anytime to change config  ");
  Serial.println("  Type  status  anytime to see config     ");
  Serial.println("===========================================");

  setup_wifi();
  client.setServer(stored_mqtt, mqtt_port);
  client.setCallback(callback);
  sensors.begin();
  pinMode(PIR_PIN,   INPUT);
  pinMode(PIR_PIN2,  INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("Relay OFF — System ready.");
  timeClient.begin();
  checkMqtt();
}

// ---------------- MAIN LOOP ----------------
void loop() {
  handleSerialInput();

  if (millis() - lastMqttCheck > mqttCheckInterval) {
    lastMqttCheck = millis();
    if (!client.connected()) checkMqtt();
    if (client.connected()) client.loop();
  }

  timeClient.update();
  int pirValue  = digitalRead(PIR_PIN);
  int pirValue2 = digitalRead(PIR_PIN2);
  sensors.requestTemperatures();
  float temperatureC = sensors.getTempCByIndex(0);

  // Google Sheets every 5 min
  if (millis() - lastGoogleSend >= GOOGLE_SEND_INTERVAL) {
    lastGoogleSend = millis();
    if (temperatureC != DEVICE_DISCONNECTED_C && temperatureC > -50 && WiFi.status() == WL_CONNECTED) {
      sendToGoogleSheets(temperatureC);
    }
  }

  // MQTT publish every 10s
  if (millis() - lastPublish > 10000 && client.connected()) {
    lastPublish = millis();
    client.publish("home/pir",  pirValue  ? "Motion" : "No Motion");
    client.publish("home/pir2", pirValue2 ? "Motion" : "No Motion");
    char tempString[10];
    dtostrf(temperatureC, 1, 2, tempString);
    client.publish("home/temperature", tempString);
  }

  // ---------------- NIGHT CHECK ----------------
  int currentHour = timeClient.getHours();
  bool isNight;
  if (nightStartHour > nightStopHour) {
    isNight = (currentHour >= nightStartHour || currentHour < nightStopHour);
  } else {
    isNight = (currentHour >= nightStartHour && currentHour < nightStopHour);
  }

  // Motion handling — only if relay is ACTIVE
  if (relayActive && isNight && (pirValue == HIGH || pirValue2 == HIGH)) {
    if (millis() - lastMotionTime > 10000) {
      lastMotionTime = millis();
      if (!relayState) {
        relayState = true;
        digitalWrite(RELAY_PIN, LOW);
        relayOffTime = millis() + 30000UL;
        extendMode = 0;
        if (client.connected()) client.publish("home/light", "Light turned ON (30 sec)");
        Serial.println("✅ Relay ON - Motion Detected (30 sec)");
      } else {
        if (extendMode == 0) {
          relayOffTime = millis() + 60000UL;
          extendMode = 1;
          if (client.connected()) client.publish("home/light", "Extended to 60 seconds");
          Serial.println("⏰ Extended to 60 seconds");
        } else {
          relayOffTime += 30000UL;
          extendMode++;
          if (client.connected()) client.publish("home/light", "Extended +30 seconds");
          Serial.println("⏰ Extended +30 seconds");
        }
      }
    }
  }

  // Relay timer
  if (relayState && relayOffTime != 0 && millis() > relayOffTime) {
    relayState = false;
    digitalWrite(RELAY_PIN, HIGH);
    extendMode = 0;
    if (client.connected()) client.publish("home/light", "Light turned OFF");
    Serial.println("❌ Relay OFF - Timer expired");
  }
}

// ---------------- GOOGLE SHEETS ----------------
void sendToGoogleSheets(float temperature) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected - skipping Google Sheets");
    return;
  }
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient https;
  String url = String(GOOGLE_SCRIPT_URL) + "?temp=" + String(temperature, 2);
  if (https.begin(secureClient, url)) {
    int httpCode = https.GET();
    Serial.print("Google Sheets HTTP code: ");
    Serial.println(httpCode);
    https.end();
  } else {
    Serial.println("Unable to connect to Google Sheets");
  }
}
