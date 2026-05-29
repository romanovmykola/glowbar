#include <Arduino.h>
#include <MD_MAX72xx.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

// --- WIFI & OTA LIBRARIES ---
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h> 
#include <WebServer.h>  

// ================= PIN CONFIGURATION (ESP32-C3 Super Mini) =================
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW 
#define MAX_DEVICES 4                     
#define DATA_PIN 7                        
#define CS_PIN 6                          
#define CLK_PIN 4                         

#define TOUCH_PIN 5       
#define BOOT_BTN_PIN 9    
#define BUILTIN_LED_PIN 8 

// ================= GLOBAL VARIABLES ===================
MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// Wi-Fi States
enum WifiState { WF_OFF, WF_CONNECTING, WF_CONNECTED, WF_OTA };
WifiState currentWifiState = WF_OFF;
volatile bool wifiInitDone = false; 

WiFiManager wm;
WebServer server(80);

// Display parameters
bool rotateScreen = true; 
int globalBrightness = 4;

// Timer States & Variables
enum TimerState { TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_ALARM };
TimerState timerState = TIMER_IDLE;

const unsigned long START_TIME_MS = 30 * 60 * 1000; // 30 Minutes
unsigned long timeRemaining = START_TIME_MS;
unsigned long lastTickTime = 0;

// Input Variables
bool lastTouchState = LOW;
unsigned long touchStartTime = 0;
bool touchHandled = false;

// ================= HELPER FUNCTIONS & TASKS =================

void initWifiTask(void *pvParameters) {
  WiFi.setHostname("glowbar"); 
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false); 

  wm.setConnectTimeout(3); 
  wm.setConfigPortalBlocking(false); 
  wm.setConfigPortalTimeout(120);    
  
  wm.autoConnect("GlowBar-Timer");
  
  ArduinoOTA.setHostname("glowbar-timer");
  ArduinoOTA.onStart([]() { currentWifiState = WF_OTA; }); 
  ArduinoOTA.onEnd([]() { currentWifiState = WF_CONNECTED; }); 
  ArduinoOTA.begin();
  
  server.on("/", []() {
    server.send(200, "text/plain", "GlowBar Timer is running. Go to /update to flash new firmware.");
  });
  
  ElegantOTA.onStart([]() { currentWifiState = WF_OTA; }); 
  ElegantOTA.onEnd([](bool success) { 
      currentWifiState = WF_CONNECTED; 
      digitalWrite(BUILTIN_LED_PIN, success ? LOW : HIGH);
  }); 

  ElegantOTA.begin(&server);    
  server.begin();

  wifiInitDone = true; 
  vTaskDelete(NULL); 
}

void drawColumn(int colIndex, uint8_t colData) {
  if (rotateScreen) {
    colData = (colData & 0xF0) >> 4 | (colData & 0x0F) << 4;
    colData = (colData & 0xCC) >> 2 | (colData & 0x33) << 2;
    colData = (colData & 0xAA) >> 1 | (colData & 0x55) << 1;
    colIndex = 31 - colIndex; 
  }
  mx.setColumn(colIndex, colData); 
}

// ================= MAIN SETUP FUNCTION =================
void setup() {
  Serial.begin(115200);
  delay(100); 
  
  pinMode(TOUCH_PIN, INPUT); 
  pinMode(BUILTIN_LED_PIN, OUTPUT);
  digitalWrite(BUILTIN_LED_PIN, HIGH); 

  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, globalBrightness); 
  mx.clear();

  currentWifiState = WF_CONNECTING; 

  xTaskCreate(initWifiTask, "WiFiInit", 8192, NULL, 1, NULL); 
}

// ================= CORE TIMER DISPLAY ENGINE =================
void processTimer() {
  uint8_t buffer[32] = {0};

  // 1. Calculate Sandclock Bars
  float fraction = (float)timeRemaining / START_TIME_MS;
  int activeRows = ceil(fraction * 8.0); 
  uint8_t barCol = 0;
  
  for (int y = 0; y < activeRows; y++) {
      barCol |= (1 << (7 - y)); 
  }

  // 2. Evaluate Blinking Logic for the Bars (Final 5 Mins)
  bool showBars = true;
  if (timerState == TIMER_RUNNING && timeRemaining <= 300000 && timeRemaining > 0) {
      float urgency = 1.0 - ((float)timeRemaining / 300000.0);
      int blinkInterval = 1000 - (urgency * 850); 
      
      if ((millis() % blinkInterval) > (blinkInterval / 2)) {
          showBars = false;
      }
  }

  // Draw bars to the extreme edges
  if (showBars && timerState != TIMER_ALARM) {
      buffer[0] = barCol; buffer[1] = barCol;
      buffer[30] = barCol; buffer[31] = barCol;
  }

  // 3. Render the Text Elements
  int mins = timeRemaining / 60000;
  int secs = (timeRemaining % 60000) / 1000;
  int m1 = mins / 10; int m2 = mins % 10;
  int s1 = secs / 10; int s2 = secs % 10;

  bool showText = true;
  if (timerState == TIMER_ALARM) {
      showText = ((millis() % 500) < 250); 
  } else if (timerState == TIMER_PAUSED) {
      showText = ((millis() % 1000) < 500); 
  }

  // NEW 4x8 Custom Font mapping
  static const uint8_t numFont[10][4] = {
    {0xFF, 0x81, 0x81, 0xFF}, // 0
    {0x82, 0x83, 0xFF, 0x80}, // 1
    {0xF9, 0x89, 0x89, 0x8F}, // 2
    {0x89, 0x89, 0x89, 0xFF}, // 3
    {0x0F, 0x08, 0x08, 0xFF}, // 4
    {0x8F, 0x89, 0x89, 0xF9}, // 5
    {0xFF, 0x89, 0x89, 0xF9}, // 6
    {0x01, 0x01, 0x01, 0xFF}, // 7
    {0xFF, 0x89, 0x89, 0xFF}, // 8
    {0x8F, 0x89, 0x89, 0xFF}  // 9
  };

  if (showText) {
      // Print the digits spaced perfectly in the center
      for(int i=0; i<4; i++) {
          buffer[4+i]  = numFont[m1][i];
          buffer[9+i]  = numFont[m2][i];
          buffer[19+i] = numFont[s1][i];
          buffer[24+i] = numFont[s2][i];
      }
      
      bool showColon = (timerState == TIMER_RUNNING) ? ((millis() % 1000) < 500) : true;
      if (showColon) {
          buffer[15] = 0x66; // 01100110 draws a thick 2x2 double-dot colon
          buffer[16] = 0x66; 
      }
  }

  // 4. Output to Matrix
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  for (int x = 0; x < 32; x++) {
      uint8_t colData = buffer[x];
      
      // Vertical Bit-Flip hardware logic
      colData = (colData & 0xF0) >> 4 | (colData & 0x0F) << 4;
      colData = (colData & 0xCC) >> 2 | (colData & 0x33) << 2;
      colData = (colData & 0xAA) >> 1 | (colData & 0x55) << 1;
      
      drawColumn(x, colData); 
  }
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}

// ================= ARDUINO MAIN LOOP =================
void loop() {
  
  // Background processes
  if (wifiInitDone) {
      wm.process(); 
      server.handleClient(); 
      ElegantOTA.loop();     
      if (currentWifiState == WF_CONNECTING && WiFi.status() == WL_CONNECTED) {
          currentWifiState = WF_CONNECTED;
      }
      if (currentWifiState == WF_CONNECTED) {
          ArduinoOTA.handle(); 
      }
  }

  // ----------------------------------------------------
  // TOUCH BUTTON LOGIC
  // ----------------------------------------------------
  bool currentTouchState = digitalRead(TOUCH_PIN);
  
  if (currentTouchState == HIGH && lastTouchState == LOW) {
      touchStartTime = millis();
      touchHandled = false;
  } 
  else if (currentTouchState == HIGH && lastTouchState == HIGH) {
      // LONG PRESS -> Reset Timer
      if (!touchHandled && (millis() - touchStartTime > 800)) {
          touchHandled = true;
          timerState = TIMER_IDLE;
          timeRemaining = START_TIME_MS;
      }
  } 
  else if (currentTouchState == LOW && lastTouchState == HIGH) {
      // SHORT PRESS -> Play / Pause logic
      if (!touchHandled) {
          if (timerState == TIMER_IDLE) {
              timerState = TIMER_RUNNING;
              lastTickTime = millis();
          } else if (timerState == TIMER_RUNNING) {
              timerState = TIMER_PAUSED;
          } else if (timerState == TIMER_PAUSED) {
              timerState = TIMER_RUNNING;
              lastTickTime = millis();
          } else if (timerState == TIMER_ALARM) {
              timerState = TIMER_IDLE;
              timeRemaining = START_TIME_MS;
          }
      }
  }
  lastTouchState = currentTouchState; 

  // ----------------------------------------------------
  // TIME TICKING LOGIC
  // ----------------------------------------------------
  if (timerState == TIMER_RUNNING) {
      unsigned long now = millis();
      unsigned long delta = now - lastTickTime;
      
      if (delta <= timeRemaining) {
          timeRemaining -= delta;
      } else {
          timeRemaining = 0;
          timerState = TIMER_ALARM;
      }
      lastTickTime = now;
  }

  // Process the visuals
  processTimer(); 
  delay(15); 
}
