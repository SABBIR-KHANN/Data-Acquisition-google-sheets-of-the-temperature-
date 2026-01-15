#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

const char* GOOGLE_SCRIPT_URL = "https://script.google.com/********************************************************************exec";
unsigned long lastGoogleSend = 0;
const unsigned long GOOGLE_SEND_INTERVAL = 300000UL;  // 5 minutes

// ---------------- WIFI + MQTT CONFIG ----------------
const char* ssid = "***************";
const char* password = "*****************";
const char* mqtt_server = "192.*************";
const int mqtt_port = 1883;
const char* mqtt_control_topic = "home/relay/cmd";
WiFiClient espClient;
PubSubClient client(espClient);

// ---------------- TIME CONFIG ----------------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 6 * 3600);

// ---------------- SENSOR PINS ----------------
#define ONE_WIRE_BUS D4
#define PIR_PIN D5
#define PIR_PIN2 D6
#define RELAY_PIN D2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ---------------- VARIABLES ----------------
bool relayState = false;
unsigned long relayOffTime = 0;
int extendMode = 0;
unsigned long lastPublish = 0;
unsigned long lastMotionTime = 0;
unsigned long lastMqttCheck = 0;
const unsigned long mqttCheckInterval = 5000;  // MQTT check every 5s

// ---------------- MQTT CALLBACK ----------------
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  Serial.println(message);

  if (message == "1") {
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

// ---------------- WIFI CONNECT (Non-Blocking Boot) ----------------
void setup_wifi() {
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {  // Try 20s max
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
  } else {
    Serial.println("\nWiFi failed—running offline (PIR/relay active)");
  }
}

// ---------------- MQTT QUICK CHECK (Non-Blocking) ----------------
void checkMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;  // Skip if no WiFi
  Serial.print("MQTT check...");
  if (client.connect("ESP8266_PIR_Temp")) {
    Serial.println("connected");
    client.subscribe(mqtt_control_topic);
  } else {
    Serial.print("failed, rc=");
    Serial.println(client.state());
  }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  setup_wifi();  // Non-blocking
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  sensors.begin();
  pinMode(PIR_PIN, INPUT);
  pinMode(PIR_PIN2, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("System started - Relay OFF");
  timeClient.begin();
  checkMqtt();  // Initial try
}

// ---------------- MAIN LOOP ----------------
void loop() {
  // Non-blocking MQTT check every 5s
  if (millis() - lastMqttCheck > mqttCheckInterval) {
    lastMqttCheck = millis();
    if (!client.connected()) checkMqtt();
    if (client.connected()) client.loop();  // Process if connected
  }

  timeClient.update();
  int pirValue = digitalRead(PIR_PIN);
  int pirValue2 = digitalRead(PIR_PIN2);
  sensors.requestTemperatures();
  float temperatureC = sensors.getTempCByIndex(0);

  // Google Sheets every 5 min (non-blocking, skip no WiFi)
  if (millis() - lastGoogleSend >= GOOGLE_SEND_INTERVAL) {
    lastGoogleSend = millis();
    if (temperatureC != DEVICE_DISCONNECTED_C && temperatureC > -50 && WiFi.status() == WL_CONNECTED) {
      sendToGoogleSheets(temperatureC);
    }
  }

  // MQTT publish every 10s (only if connected)
  if (millis() - lastPublish > 10000 && client.connected()) {
    lastPublish = millis();
    client.publish("home/pir", pirValue ? "Motion" : "No Motion");
    client.publish("home/pir2", pirValue2 ? "Motion" : "No Motion");
    char tempString[10];
    dtostrf(temperatureC, 1, 2, tempString);
    client.publish("home/temperature", tempString);
  }

  // Night check (adjust hours if needed, e.g., >=17 <6 for 5PM-6AM)
  int currentHour = timeClient.getHours();
  bool isNight = (currentHour >= 18 || currentHour < 7);

  // Motion handling (always runs, no MQTT needed)
  if (isNight && (pirValue == HIGH || pirValue2 == HIGH)) {
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

  // Relay timer (always runs)
  if (relayState && relayOffTime != 0 && millis() > relayOffTime) {
    relayState = false;
    digitalWrite(RELAY_PIN, HIGH);
    extendMode = 0;
    if (client.connected()) client.publish("home/light", "Light turned OFF");
    Serial.println("❌ Relay OFF - Timer expired");
  }
}

// Google Sheets function (unchanged—safe)
void sendToGoogleSheets(float temperature) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected - skipping Google Sheets");
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String(GOOGLE_SCRIPT_URL) + "?temp=" + String(temperature, 2);
  if (https.begin(client, url)) {
    int httpCode = https.GET();
    Serial.print("Google Sheets HTTP code: ");
    Serial.println(httpCode);
    https.end();
  } else {
    Serial.println("Unable to connect to Google Sheets");
  }
}
