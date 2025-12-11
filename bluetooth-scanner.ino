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
const char* WIFI_SSID = "ESP32_BLE_Counter";
const char* WIFI_PASS = "12345678";
const char* DATA_PATH = "/ble_readings.txt";

// --- HARDWARE ---
#define NEOPIXEL_PIN  48  
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
unsigned long lastButtonPress = 0;

// LED Vars
bool triggerLed = false;

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

// ================= BLE CALLBACK =================
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      // Get the MAC Address
      String address = String(advertisedDevice.getAddress().toString().c_str());
      
      // If this is a NEW device we haven't seen in this 15-min block
      if (foundDevices.find(address) == foundDevices.end()) {
        foundDevices.insert(address);
        Serial.printf("Found: %s (RSSI: %d)\n", address.c_str(), advertisedDevice.getRSSI());
        
        // Flash LED Blue immediately
        pixels.setPixelColor(0, COLOR_BLE);
        pixels.show();
        delay(50); // Short blocking delay is okay in BLE callbacks
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
  
  // Turn OFF WiFi to save power/radio
  WiFi.mode(WIFI_OFF);
  
  // Init BLE
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

  // Stop BLE to free up radio for WiFi
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
  
  pixels.begin();
  pixels.setBrightness(BRIGHTNESS);
  initFS();

  startSnifferMode();
}

void loop() {
  unsigned long currentMillis = millis();

  // Button Check
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (currentMillis - lastButtonPress > 1000) { // 1 sec debounce
        toggleMode();
        lastButtonPress = currentMillis;
      }
    }
  }

  if (isServerMode) {
    server.handleClient();
    delay(5);
  } 
  else {
    // BLE MODE: Scan for X seconds
    // This is blocking, but that's fine for a simple counter
    // CORRECTED: Just run the scan and ignore the return variable
    pBLEScan->start(SCAN_TIME, false);
    pBLEScan->clearResults(); // delete results from RAM to free memory
    
    // Save Data Check
    if (currentMillis - lastSaveTime >= SAVE_INTERVAL_MS) {
      int count = foundDevices.size();
      saveToStorage(currentMillis, count);
      foundDevices.clear(); // Reset buffer for next 15 mins
      lastSaveTime = currentMillis;
    }
  }
}
