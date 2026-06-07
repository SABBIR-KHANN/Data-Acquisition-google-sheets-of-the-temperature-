// ================================================================
//  credentials.h
//  Edit your WiFi passwords and Google Sheets URL here.
//  This file appears as a separate tab in Arduino IDE.
// ================================================================
#pragma once

// -------- WiFi Networks (3 slots) --------
// The ESP will scan all three, pick the one with
// the strongest signal that actually has internet access.

#define WIFI_COUNT 3

const char* WIFI_SSID[WIFI_COUNT] = {
  "*********",        // <- WiFi 1  (your current network)
  "*****",     // <- WiFi 2  (replace with real SSID)
  "********"      // <- WiFi 3  (replace with real SSID)
};

const char* WIFI_PASS[WIFI_COUNT] = {
  "********",       // <- Password for WiFi 1
  "**********",     // <- Password for WiFi 2
  "*********"      // <- Password for WiFi 3
};

// -------- Google Sheets Script URL --------
const char* GOOGLE_SCRIPT_URL =
  "https://script.google.com/****/*/"
  "*********************************************************";
