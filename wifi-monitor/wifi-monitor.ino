#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <FS.h>
#include <LittleFS.h>
#include <WebServer.h> 

// --- HARDWARE CONFIG ---
#define RGB_PIN 48  
#define BUTTON_PIN 0  
#define NUMPIXELS 1
#define WIFI_CHANNEL 1

// --- TIMING CONFIG ---
#define LOG_INTERVAL_MS 900000       // Save to File every 15 Minutes
#define CALIBRATION_INTERVAL_MS 60000 // Learn Environment every 1 Minute

// --- AP CONFIG ---
const char* ap_ssid = "jsESP32-Wifi";
const char* ap_pass = "ESP32WifiPassword12#";

// --- OBJECTS ---
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

// --- VARIABLES ---
volatile int packetCount = 0;       
bool inAPMode = false; 

unsigned long minuteTotalPackets = 0;       
unsigned long fifteenMinuteTotalPackets = 0; 

// --- SMART HISTORY BUFFER ---
long history[15];
int historyIndex = 0;
bool historyFull = false;

// --- DYNAMIC THRESHOLDS ---
long dynamicLow = 200;  
long dynamicHigh = 800; 

// --- FADE VARS ---
int currentR = 0, currentG = 0, currentB = 0;
int targetR = 0, targetG = 0, targetB = 0;
int fadeSpeed = 5; 

// --- TIMERS ---
unsigned long lastTrafficCheck = 0;
unsigned long lastFadeUpdate = 0;
unsigned long lastCalibrationTime = 0; 
unsigned long lastLogTime = 0;         

// Sniffer Callback
void promiscuous_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  packetCount++;
}

// --- NEW: TIME FORMATTER HELPER ---
// Converts raw seconds (e.g., 3665) into "01:01:05"
String formatUptime(long totalSeconds) {
  long h = totalSeconds / 3600;
  long m = (totalSeconds % 3600) / 60;
  long s = totalSeconds % 60;
  
  char buffer[20];
  sprintf(buffer, "%02ld:%02ld:%02ld", h, m, s); // Format as HH:MM:SS
  return String(buffer);
}

// --- WEB HANDLERS ---
void handleRoot() {
  File file = LittleFS.open("/traffic_log.txt", "r");
  
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body{font-family:monospace; text-align:center; background-color:#121212; color:#ffffff;}";
  html += "table{margin:auto; border-collapse: collapse; width:90%; max-width:600px;}";
  html += "td, th {border: 1px solid #444; padding: 10px;}";
  html += "th {background-color:#333;}";
  html += "button {background:#e74c3c; color:white; padding:15px; border:none; border-radius:5px; font-size:16px; cursor:pointer;}";
  html += "</style>";
  html += "</head><body>";
  
  html += "<h2>Traffic History</h2>";
  html += "<p>Current Uptime: " + formatUptime(millis()/1000) + "</p>"; // Show live uptime at top
  
  if (!file || !file.available()) {
    html += "<p>No logs found.</p>";
  } else {
    html += "<table><tr><th>Time (HH:MM:SS)</th><th>Avg Packets/Sec</th></tr>";
    
    // Read file line by line
    while (file.available()) {
      String line = file.readStringUntil('\n');
      if (line.length() > 0) {
        int comma = line.indexOf(',');
        if (comma > 0) {
           String rawSeconds = line.substring(0, comma);
           String val = line.substring(comma+1);
           
           // Convert raw seconds to HH:MM:SS
           String niceTime = formatUptime(rawSeconds.toInt());

           html += "<tr><td>" + niceTime + "</td><td>" + val + "</td></tr>";
        }
      }
    }
    html += "</table>";
    file.close();
  }
  
  html += "<br><br><form action='/clear' method='get'><button>CLEAR LOGS</button></form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleClear() {
  LittleFS.remove("/traffic_log.txt");
  server.send(200, "text/html", "<h2>Logs cleared!</h2><p><a style='color:cyan' href='/'>Go Back</a></p>");
}

void appendFile(const char * path, String message){
  File file = LittleFS.open(path, FILE_APPEND);
  if(!file) return;
  file.print(message);
  file.close();
}

void calibrateThresholds() {
  if (historyIndex == 0 && !historyFull) return;
  long minVal = 999999;
  long maxVal = 0;
  int count = historyFull ? 15 : historyIndex;
  for (int i = 0; i < count; i++) {
    if (history[i] < minVal) minVal = history[i];
    if (history[i] > maxVal) maxVal = history[i];
  }
  if (maxVal - minVal < 50) maxVal = minVal + 50; 
  dynamicLow = minVal + ((maxVal - minVal) * 0.33);
  dynamicHigh = minVal + ((maxVal - minVal) * 0.66);
}

void enableAPMode() {
  Serial.println("Switching to AP Mode...");
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_pass);
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/clear", handleClear);
  server.begin();
  pixels.setPixelColor(0, pixels.Color(0, 255, 255));
  pixels.show();
  inAPMode = true;
}

void disableAPMode() {
  Serial.println("Switching back to Sniffer Mode...");
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  inAPMode = false;
}

void setup() {
  Serial.begin(115200);
  delay(1000); 

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Mount Failed");
    return;
  }

  pixels.begin();
  pixels.setBrightness(20);
  pixels.show();

  for(int i=0; i<15; i++) history[i] = 0;
  disableAPMode(); 
  
  lastLogTime = millis();
  lastCalibrationTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // --- BUTTON LISTENER ---
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); 
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (inAPMode) disableAPMode();
      else enableAPMode();
      while(digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
  }

  // --- MODE 1: AP ---
  if (inAPMode) {
    server.handleClient();
    int b = (millis() / 10) % 255; 
    if (b>127) b = 255 - b; 
    pixels.setPixelColor(0, pixels.Color(0, b*2, b*2)); 
    pixels.show();
    return; 
  }

  // --- MODE 2: SNIFFER ---
  if (currentMillis - lastCalibrationTime > CALIBRATION_INTERVAL_MS) {
    long minuteAvg = minuteTotalPackets / 60; 
    history[historyIndex] = minuteAvg;
    historyIndex++;
    if (historyIndex >= 15) { historyIndex = 0; historyFull = true; }
    calibrateThresholds();
    minuteTotalPackets = 0;
    lastCalibrationTime = currentMillis;
  }

  if (currentMillis - lastLogTime > LOG_INTERVAL_MS) {
    long fifteenMinAvg = fifteenMinuteTotalPackets / (LOG_INTERVAL_MS / 1000);
    // Logging logic remains same (stores raw seconds)
    String logEntry = String(millis()/1000) + "," + String(fifteenMinAvg) + "\n";
    appendFile("/traffic_log.txt", logEntry);
    fifteenMinuteTotalPackets = 0;
    lastLogTime = currentMillis;
  }

  if (currentMillis - lastTrafficCheck > 1000) {
    int currentPackets = packetCount; 
    packetCount = 0; 
    minuteTotalPackets += currentPackets;
    fifteenMinuteTotalPackets += currentPackets;
    Serial.printf("Traffic:%d pckts/sec \n", currentPackets);

    if (currentPackets == 0) { targetR = 0; targetG = 0; targetB = 0; } 
    else if (currentPackets < dynamicLow) { targetR = 0; targetG = 255; targetB = 0; } 
    else if (currentPackets < dynamicHigh) { targetR = 255; targetG = 180; targetB = 0; } 
    else { targetR = 255; targetG = 0; targetB = 0; }
    lastTrafficCheck = currentMillis;
  }

  if (currentMillis - lastFadeUpdate > 10) {
    if (currentR < targetR) currentR += fadeSpeed;
    if (currentR > targetR) currentR -= fadeSpeed;
    if (abs(currentR - targetR) < fadeSpeed) currentR = targetR; 
    if (currentG < targetG) currentG += fadeSpeed;
    if (currentG > targetG) currentG -= fadeSpeed;
    if (abs(currentG - targetG) < fadeSpeed) currentG = targetG;
    if (currentB < targetB) currentB += fadeSpeed;
    if (currentB > targetB) currentB -= fadeSpeed;
    if (abs(currentB - targetB) < fadeSpeed) currentB = targetB;
    pixels.setPixelColor(0, pixels.Color(currentR, currentG, currentB));
    pixels.show();
    lastFadeUpdate = currentMillis;
  }
}