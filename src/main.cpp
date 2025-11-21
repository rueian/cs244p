#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "SparkFunLSM6DSO.h"

// ================= CONFIGURATION =================

// --- Wi-Fi Credentials ---
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// --- HTTP Endpoint ---
// The device sends data here. 
// If the response body is "false", the alarm stops.
const String serverUrl = ""; 

// --- Hardware Pins ---
// NOTE: We use GPIO 33 for LDR because it is on ADC1. 
// ADC2 pins (like 25) cannot be used when WiFi is active on ESP32.
#define LDR_PIN       33  
#define BUZZER_PIN    26  
// TTGO T-Display I2C Pins
#define I2C_SDA       21
#define I2C_SCL       22

// --- Thresholds ---
// Light value (0-4095). Higher = Brighter. 
// Adjust based on your specific LDR and resistor.
const int LIGHT_THRESHOLD = 50; 

// Motion sensitivity (g-force deviation). 
// 0.15 is sensitive, 0.5 is hard shaking.
const float MOTION_THRESHOLD = 0.2; 

// --- Timers ---
const unsigned long INTERVAL = 2000;

// ================= GLOBALS =================

LSM6DSO imu;
bool alarmActive = false;       // Is the alarm currently going off?
bool motionDetected = false;    // Momentary motion flag
bool lightDetected = false;   // Momentary light flag
int lastLightLevel = 0;

unsigned long lastNetTime = 0;
unsigned long buzzerTimer = 0;
bool buzzerState = false;

// Root CA for HTTPS (Optional: leaving empty often works for testing, 
// but for Azure/AWS you need the specific DigiCert/Baltimore cert)
const char* root_ca = \
"-----BEGIN CERTIFICATE-----\n"\
"MIIFrDCCBJSgAwIBAgIQCkOpUJsBNS+JlXnscgi6UDANBgkqhkiG9w0BAQwFADBh\n"\
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"\
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"\
"MjAeFw0yMzA2MDgwMDAwMDBaFw0yNjA4MjUyMzU5NTlaMF0xCzAJBgNVBAYTAlVT\n"\
"MR4wHAYDVQQKExVNaWNyb3NvZnQgQ29ycG9yYXRpb24xLjAsBgNVBAMTJU1pY3Jv\n"\
"c29mdCBBenVyZSBSU0EgVExTIElzc3VpbmcgQ0EgMDcwggIiMA0GCSqGSIb3DQEB\n"\
"AQUAA4ICDwAwggIKAoICAQC1ZF7KYus5OO3GWqJoR4xznLDNCjocogqeCIVdi4eE\n"\
"BmF3zIYeuXXNoJAUF+mn86NBt3yMM0559JZDkiSDi9MpA2By4yqQlTHzfbOrvs7I\n"\
"4LWsOYTEClVFQgzXqa2ps2g855HPQW1hZXVh/yfmbtrCNVa//G7FPDqSdrAQ+M8w\n"\
"0364kyZApds/RPcqGORjZNokrNzYcGub27vqE6BGP6XeQO5YDFobi9BvvTOO+ZA9\n"\
"HGIU7FbdLhRm6YP+FO8NRpvterfqZrRt3bTn8GT5LsOTzIQgJMt4/RWLF4EKNc97\n"\
"CXOSCZFn7mFNx4SzTvy23B46z9dQPfWBfTFaxU5pIa0uVWv+jFjG7l1odu0WZqBd\n"\
"j0xnvXggu564CXmLz8F3draOH6XS7Ys9sTVM3Ow20MJyHtuA3hBDv+tgRhrGvNRD\n"\
"MbSzTO6axNWvL46HWVEChHYlxVBCTfSQmpbcAdZOQtUfs9E4sCFrqKcRPdg7ryhY\n"\
"fGbj3q0SLh55559ITttdyYE+wE4RhODgILQ3MaYZoyiL1E/4jqCOoRaFhF5R++vb\n"\
"YpemcpWx7unptfOpPRRnnN4U3pqZDj4yXexcyS52Rd8BthFY/cBg8XIR42BPeVRl\n"\
"OckZ+ttduvKVbvmGf+rFCSUoy1tyRwQNXzqeZTLrX+REqgFDOMVe0I49Frc2/Avw\n"\
"3wIDAQABo4IBYjCCAV4wEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHQ4EFgQUzhUW\n"\
"O+oCo6Zr2tkr/eWMUr56UKgwHwYDVR0jBBgwFoAUTiJUIBiV5uNu5g/6+rkS7QYX\n"\
"jzkwDgYDVR0PAQH/BAQDAgGGMB0GA1UdJQQWMBQGCCsGAQUFBwMBBggrBgEFBQcD\n"\
"AjB2BggrBgEFBQcBAQRqMGgwJAYIKwYBBQUHMAGGGGh0dHA6Ly9vY3NwLmRpZ2lj\n"\
"ZXJ0LmNvbTBABggrBgEFBQcwAoY0aHR0cDovL2NhY2VydHMuZGlnaWNlcnQuY29t\n"\
"L0RpZ2lDZXJ0R2xvYmFsUm9vdEcyLmNydDBCBgNVHR8EOzA5MDegNaAzhjFodHRw\n"\
"Oi8vY3JsMy5kaWdpY2VydC5jb20vRGlnaUNlcnRHbG9iYWxSb290RzIuY3JsMB0G\n"\
"A1UdIAQWMBQwCAYGZ4EMAQIBMAgGBmeBDAECAjANBgkqhkiG9w0BAQwFAAOCAQEA\n"\
"bbV8m4/LCSvb0nBF9jb7MVLH/9JjHGbn0QjB4R4bMlGHbDXDWtW9pFqMPrRh2Q76\n"\
"Bqm+yrrgX83jPZAcvOd7F7+lzDxZnYoFEWhxW9WnuM8Te5x6HBPCPRbIuzf9pSUT\n"\
"/ozvbKFCDxxgC2xKmgp6NwxRuGcy5KQQh4xkq/hJrnnF3RLakrkUBYFPUneip+wS\n"\
"BzAfK3jHXnkNCPNvKeLIXfLMsffEzP/j8hFkjWL3oh5yaj1HmlW8RE4Tl/GdUVzQ\n"\
"D1x42VSusQuRGtuSxLhzBNBeJtyD//2u7wY2uLYpgK0o3X0iIJmwpt7Ovp6Bs4tI\n"\
"E/peia+Qcdk9Qsr+1VgCGA==\n"\
"-----END CERTIFICATE-----\n";
// (Note: You may need to replace this with the specific CA for your endpoint)

// ================= HELPER FUNCTIONS =================

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void initIMU() {
  Wire.begin(I2C_SDA, I2C_SCL);
  if (imu.begin() != IMU_SUCCESS) {
    Serial.println("IMU failed to initialize!");
  }
  imu.initialize(BASIC_SETTINGS);
  Serial.println("IMU Initialized.");
}

void handleSensors() {
  // 1. Read Light
  lastLightLevel = analogRead(LDR_PIN);

  // 2. Read Accelerometer
  // We calculate the Magnitude of the vector to detect movement in ANY direction
  float x = imu.readFloatAccelX();
  float y = imu.readFloatAccelY();
  float z = imu.readFloatAccelZ();
  
  // Calculate total force magnitude: sqrt(x^2 + y^2 + z^2)
  float magnitude = sqrt((x*x) + (y*y) + (z*z));
  
  // Normal gravity is ~1.0g. If magnitude deviates significantly, it's moving.
  if (!motionDetected && abs(magnitude - 1.0) > MOTION_THRESHOLD) {
    motionDetected = true;
    Serial.println("Motion Detected!");
  }

  if (!lightDetected && lastLightLevel > LIGHT_THRESHOLD) {
    lightDetected = true;
    Serial.println("Light Detected!");
  }

  // 3. Trigger Alarm Logic
  // If light is too bright OR motion detected -> Trigger Alarm
  if (lightDetected || motionDetected) {
    if (!alarmActive) {
      Serial.println("ALARM TRIGGERED!");
      alarmActive = true;
    }
  }
}

void handleBuzzer() {
  if (!alarmActive) {
    if (buzzerState) {
      noTone(BUZZER_PIN);
      buzzerState = false;
    }
  } else {
    if (!buzzerState) {
      tone(BUZZER_PIN, 2000); // 2kHz sound
      buzzerState = true;
    }
  }
}

void sendTelemetry() {
  WiFiClientSecure client;
  client.setInsecure(); // Use .setCACert(root_ca) for production security
  
  HTTPClient http;
  
  // Prepare JSON payload
  ArduinoJson::JsonDocument doc;
  doc["light_level"] = lastLightLevel;
  doc["motion_detected"] = motionDetected;
  doc["alarm_active"] = alarmActive;
  doc["vault_status"] = (lightDetected) ? "OPEN" : "CLOSED";

  String requestBody;
  serializeJson(doc, requestBody);

  Serial.print("Sending: ");
  Serial.println(requestBody);

  // Start Connection
  http.begin(client, serverUrl);
  http.addHeader("Content-Type", "application/json");
  
  // Send POST
  int httpResponseCode = http.POST(requestBody);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.print("Response Code: "); Serial.println(httpResponseCode);
    Serial.print("Response Body: "); Serial.println(response);

    // === REMOTE RESET LOGIC ===
    // If the server returns the text "false", we turn off the alarm.
    // We also check "false" inside quotes just in case it returns JSON string "false"
    if (response.indexOf("false") != -1) {
       Serial.println("Received Reset Command from Server.");
       alarmActive = false;
    }
  } else {
    Serial.print("Error on sending POST: ");
    Serial.println(httpResponseCode);
  }
  
  http.end();

  motionDetected = false;
  lightDetected = false;
}

// ================= MAIN LOOPS =================

void setup() {
  Serial.begin(9600);
  delay(1000);

  Serial.println("Starting VaultAlert...");
  
  pinMode(LDR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  initIMU();
  setupWifi();
  
  Serial.println("VaultAlert System Ready...");
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Monitor Sensors
  handleSensors();

  // 2. Manage Buzzer (only if alarm is active)
  handleBuzzer();

  // 3. Network Communication at INTERVAL
  if (currentMillis - lastNetTime >= INTERVAL) {
    if (WiFi.status() == WL_CONNECTED) {
      sendTelemetry();
    } else {
      Serial.println("WiFi Disconnected, attempting reconnect...");
      WiFi.reconnect();
    }
    lastNetTime = currentMillis;
  }
}