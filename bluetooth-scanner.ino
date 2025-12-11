#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <set>
#include <Adafruit_NeoPixel.h>

// ================= CONFIGURATION =================
const char* WIFI_SSID = "jsESP32-BLE";
const char* WIFI_PASS = "ESP32BluetoothPassword12#";
const char* DATA_PATH = "/ble_readings.txt";

// --- HARDWARE ---
#define NEOPIXEL_PIN  48  // (Set to 48 if using S3-DevKitC)
#define NUM_PIXELS    1
#define BRIGHTNESS    20
#define BUTTON_PIN    0   

// --- TIMING ---
const unsigned long SAVE_INTERVAL_MS = 15 * 60 * 1000; // Save every 15 mins
const int SCAN_TIME = 5; // Scan for 5 seconds at a time

// Colors
uint32_t COLOR_BLE    = Adafruit_NeoPixel::Color(0, 0, 255);   // Blue Flash
uint32_t COLOR_SERVER = Adafruit_NeoPixel::Color(255, 255, 255); // White Solid

// ================= GLOBALS =================
Adafruit_NeoPixel pixels(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);
BLEScan* pBLEScan;

std::set<String> foundDevices; 
bool isServerMode = false;
unsigned long lastSaveTime = 0;

// INTERRUPT VARIABLES (Volatile is required for ISRs)
volatile bool buttonWasPressed = false;
unsigned long lastButtonPressTime = 0;

// ================= HELPERS =================

void initFS() {
  if (!LittleFS.begin(true)) Serial.println("FS Mount Failed");
  else Serial.println("Storage Mounted.");
}

void saveToStorage(unsigned long timestamp, int count) {
  File file = LittleFS.open(DATA_PATH, FILE_APPEND);
  if (!file) return;
  file.printf("%lu,%d\n", timestamp, count);
  file.close();
  Serial.printf(">> SAVED to Storage: %d devices\n", count);
}

// ================= BUTTON INTERRUPT =================
// This runs immediately when button is pressed, even during scanning
void IRAM_ATTR handleButtonPress() {
  buttonWasPressed = true;
}

// ================= BLE CALLBACK =================
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      String address = String(advertisedDevice.getAddress().toString().c_str());
      
      if (foundDevices.find(address) == foundDevices.end()) {
        foundDevices.insert(address);
        
        // Flash LED Blue
        pixels.setPixelColor(0, COLOR_BLE);
        pixels.show();
        delay(10); // Very short delay
        pixels.setPixelColor(0, 0); 
        pixels.show();
      }
    }
};

// ================= WEB SERVER =================

String buildHtmlGraph() {
  if (!LittleFS.exists(DATA_PATH)) return "<h3>No Data Found.</h3>";
  
  File file = LittleFS.open(DATA_PATH, FILE_READ);
  String dataPoints = "";
  int maxVal = 1;
  int x = 0;

  // Pass 1: Get Max
  while (file.available()) {
    String line = file.readStringUntil('\n');
    int comma = line.indexOf(',');
    if (comma > 0) {
      int val = line.substring(comma + 1).toInt();
      if (val > maxVal) maxVal = val;
    }
  }
  file.seek(0); 

  // Pass 2: Build Points
  while (file.available()) {
    String line = file.readStringUntil('\n');
    int comma = line.indexOf(',');
    if (comma > 0) {
      int val = line.substring(comma + 1).toInt();
      int y = 200 - map(val, 0, maxVal, 0, 200); 
      dataPoints += String(x) + "," + String(y) + " ";
      x += 10; 
    }
  }
  file.close();

  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;text-align:center;padding:20px;background:#222;color:#fff;}";
  html += ".box{background:#333;padding:20px;border-radius:10px;margin-bottom:20px;}";
  html += "svg{background:#444;border-radius:5px;border:1px solid #555;}</style></head><body>";
  html += "<h1>Bluetooth Device History</h1>";
  html += "<div class='box'><div style='overflow-x:scroll'><svg width='" + String(x < 300 ? 300 : x) + "' height='210'>";
  html += "<polyline points='" + dataPoints + "' style='fill:none;stroke:#00aaff;stroke-width:3' />";
  html += "</svg></div><p>Peak: " + String(maxVal) + " Devices</p></div>";
  html += "<div class='box'><p><a href='/clear' style='color:#f55'>Clear Data</a></p></div></body></html>";
  return html;
}

void handleRoot() { server.send(200, "text/html", buildHtmlGraph()); }
void handleClear() { LittleFS.remove(DATA_PATH); server.send(200, "text/html", "Cleared <a href='/'>Back</a>"); }

// ================= MODE SWITCHING =================

void startSnifferMode() {
  Serial.println(">>> MODE: BLUETOOTH SNIFFER");
  isServerMode = false;
  
  WiFi.mode(WIFI_OFF);
  
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true); 
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99); 
  
  pixels.setPixelColor(0, 0); pixels.show();
}

void startServerMode() {
  Serial.println(">>> MODE: WIFI SERVER");
  isServerMode = true;

  BLEDevice::deinit(); 
  
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/clear", handleClear);
  server.begin();

  pixels.setPixelColor(0, COLOR_SERVER);
  pixels.show();
}

void toggleMode() {
  if (isServerMode) startSnifferMode();
  else startServerMode();
}

// ================= SETUP & LOOP =================

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // ATTACH INTERRUPT (Fix for unreliable button)
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);
  
  pixels.begin();
  pixels.setBrightness(BRIGHTNESS);
  initFS();

  startSnifferMode();
}

void loop() {
  unsigned long currentMillis = millis();

  // --- 1. HANDLE BUTTON PRESS ---
  // If the ISR flag was set, toggle mode
  if (buttonWasPressed) {
    // Basic debounce (ignore presses within 1 second of each other)
    if (currentMillis - lastButtonPressTime > 1000) {
      toggleMode();
      lastButtonPressTime = currentMillis;
    }
    buttonWasPressed = false; // Reset flag
  }

  // --- 2. SERVER MODE ---
  if (isServerMode) {
    server.handleClient();
    delay(5);
  } 
  
  // --- 3. BLUETOOTH MODE ---
  else {
    // Scan for 5 seconds (Blocking)
    // IMPORTANT: Removing the "BLEScanResults found =" fixed your compilation error
    pBLEScan->start(SCAN_TIME, false);
    
    // Clean up RAM
    pBLEScan->clearResults(); 
    
    // Save to storage if time is up
    if (currentMillis - lastSaveTime >= SAVE_INTERVAL_MS) {
      int count = foundDevices.size();
      saveToStorage(currentMillis, count);
      foundDevices.clear(); 
      lastSaveTime = currentMillis;
    }
  }
}
