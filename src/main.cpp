#include <Arduino.h>
#include <driver/i2s.h>         // ESP32 hardware library for reading I2S digital audio
#include <MD_MAX72xx.h>         // Library for controlling the MAX7219 LED matrices
#include <arduinoFFT.h>         // Mathematical library to convert raw audio into frequency bands (Bass, Treble, etc.)
#include <Preferences.h>        // ESP32 library to save data (like the current mode) permanently to flash memory
#include "esp_sleep.h"          // ESP32 library for power-saving sleep modes
#include "driver/gpio.h"        // ESP32 hardware pin control

// --- WIFI & OTA LIBRARIES ---
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h> 
#include <WebServer.h>  

// ================= PIN CONFIGURATION (ESP32-C3 Super Mini) =================
#define I2S_WS 0   
#define I2S_SCK 1  
#define I2S_SD 3   

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW 
#define MAX_DEVICES 4                     
#define DATA_PIN 7                        
#define CS_PIN 6                          
#define CLK_PIN 4                         

#define TOUCH_PIN 5       
#define BOOT_BTN_PIN 9    
#define BUILTIN_LED_PIN 8 

// ================= AUDIO AND SCREEN CONFIGURATION =================
#define I2S_PORT I2S_NUM_0      
#define SAMPLES 128             
#define SAMPLING_FREQ 10000     

// ================= GLOBAL VARIABLES ===================
MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
Preferences preferences; 

enum WifiState { WF_OFF, WF_CONNECTING, WF_CONNECTED, WF_OTA };
WifiState currentWifiState = WF_OFF;
bool wifiAlwaysOn = false;
unsigned long wifiOnStartTime = 0; 
const unsigned long WIFI_TIMEOUT_MS = 120000; 

bool bootBtnLastState = HIGH;
unsigned long bootBtnPressTime = 0;
bool bootBtnHandled = false;

WiFiManager wm;
WebServer server(80);

bool rotateScreen = true; 
int globalMaxBrightness = 15; 
int globalIdleBrightness = 4; 

bool inSettingsMenu = false;
unsigned long settingsEnterTime = 0; 
int settingsBrightnessDirection = 1; 
unsigned long lastRampTime = 0;

double vReal[SAMPLES]; 
double vImag[SAMPLES]; 
int32_t i2sData[SAMPLES]; 

ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);

float bands[4] = {0}; 
int peaks[32];        
float smoothVal[32];  

const float SMOOTH_FACTOR = 0.2; 
const float NOISE_GATE[4] = {800.0, 300.0, 200.0, 100.0}; 

float bandPeaks[4] = {5000, 5000, 5000, 5000}; 
float bandGain[4]  = {1.0, 1.0, 1.0, 1.0};     
const float AGC_ATTACK = 0.95;    
const float AGC_RELEASE = 0.99;   
const float TARGET_LEVEL = 10000; 

int currentMode = 0;             
bool lastTouchState = LOW;       
unsigned long touchStartTime = 0;
bool isLongPressHandled = false; 

int tapCount = 0;
unsigned long lastTapTime = 0;
int modeBeforeTaps = 0;
const unsigned long TAP_TIMEOUT = 500; 

struct Particle { float x, y, vy; bool active; };
struct Star { float x, y, z; };

// ================= HELPER FUNCTIONS =================

void drawColumn(int colIndex, uint8_t colData) {
  if (rotateScreen) {
    colData = (colData & 0xF0) >> 4 | (colData & 0x0F) << 4;
    colData = (colData & 0xCC) >> 2 | (colData & 0x33) << 2;
    colData = (colData & 0xAA) >> 1 | (colData & 0x55) << 1;
    colIndex = 31 - colIndex; 
  }
  mx.setColumn(colIndex, colData); 
}

void enterSleepMode() {
  if (currentWifiState == WF_OTA) {
      return; // OTA in progress! Sleep blocked.
  }

  mx.clear(); 
  mx.control(MD_MAX72XX::SHUTDOWN, 1); 

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  currentWifiState = WF_OFF;
  digitalWrite(BUILTIN_LED_PIN, HIGH); 

  gpio_wakeup_enable((gpio_num_t)TOUCH_PIN, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  
  while (digitalRead(TOUCH_PIN) == HIGH) { delay(10); }
  delay(50); 
  
  esp_light_sleep_start();

  while (digitalRead(TOUCH_PIN) == HIGH) { delay(10); }
  
  mx.control(MD_MAX72XX::SHUTDOWN, 0); 
  mx.clear();
  lastTouchState = LOW; 
  
  wifiOnStartTime = millis(); 
}

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), 
    .sample_rate = SAMPLING_FREQ,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, 
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE, 
    .data_in_num = I2S_SD             
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
}

void drawSettingsMenu() {
  uint8_t buffer[32] = {0};

  buffer[0] = 0x08; buffer[1] = 0x22; buffer[2] = 0x1C; buffer[3] = 0x5D; 
  buffer[4] = 0x1C; buffer[5] = 0x22; buffer[6] = 0x08;

  static const uint8_t numFont[10][3] = {
      {0x7F, 0x41, 0x7F}, {0x02, 0x7F, 0x00}, {0x79, 0x49, 0x4F}, {0x49, 0x49, 0x7F}, 
      {0x0F, 0x08, 0x7F}, {0x4F, 0x49, 0x79}, {0x7F, 0x49, 0x79}, {0x01, 0x01, 0x7F}, 
      {0x7F, 0x49, 0x7F}, {0x4F, 0x49, 0x7F}  
  };

  int displayVal = globalMaxBrightness - 6; 
  buffer[8]  = numFont[displayVal][0]; buffer[9]  = numFont[displayVal][1]; buffer[10] = numFont[displayVal][2];

  buffer[16] = 0x3F; buffer[17] = 0x40; buffer[18] = 0x40; buffer[19] = 0x3F;
  buffer[21] = 0x7F; buffer[22] = 0x09; buffer[23] = 0x06;
  buffer[25] = 0x02; buffer[26] = 0x7F; buffer[27] = 0x02;
  buffer[29] = 0x02; buffer[30] = 0x7F; buffer[31] = 0x02;

  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  for (int x = 0; x < 32; x++) {
      uint8_t colData = buffer[x]; 
      colData = (colData & 0xF0) >> 4 | (colData & 0x0F) << 4;
      colData = (colData & 0xCC) >> 2 | (colData & 0x33) << 2;
      colData = (colData & 0xAA) >> 1 | (colData & 0x55) << 1;
      drawColumn(x, colData); 
  }
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}

// ================= MAIN SETUP FUNCTION =================
void setup() {
  Serial.begin(115200);
  delay(1000); 
  
  pinMode(TOUCH_PIN, INPUT); 
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  pinMode(BUILTIN_LED_PIN, OUTPUT);
  digitalWrite(BUILTIN_LED_PIN, HIGH); 

  preferences.begin("glowbar", false);
  currentMode = preferences.getUInt("mode", 0);
  if (currentMode > 10) currentMode = 0; 
  
  rotateScreen = preferences.getBool("rotate", true); 
  globalMaxBrightness = preferences.getUInt("maxBri", 15);
  globalIdleBrightness = preferences.getUInt("idleBri", 4);
  wifiAlwaysOn = preferences.getBool("wifiOn", false);

  if (globalMaxBrightness > 15) globalMaxBrightness = 15;
  if (globalMaxBrightness < 6) globalMaxBrightness = 6;
  
  if (globalMaxBrightness >= 11) globalIdleBrightness = 4;
  else if (globalMaxBrightness == 6) globalIdleBrightness = 0; 
  else globalIdleBrightness = 1;

  inSettingsMenu = false; 
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, globalIdleBrightness); 
  mx.clear();
  setupI2S();

  WiFi.setHostname("glowbar"); 

  wm.setConfigPortalBlocking(false); 
  wm.setConfigPortalTimeout(120);    
  wm.autoConnect("GlowBar-Setup");
  
  ArduinoOTA.setHostname("glowbar");
  ArduinoOTA.onStart([]() { currentWifiState = WF_OTA; }); 
  ArduinoOTA.onEnd([]() { currentWifiState = WF_CONNECTED; }); 
  ArduinoOTA.begin();
  
  server.on("/", []() {
    server.send(200, "text/plain", "GlowBar is running. Go to /update to flash new firmware.");
  });
  
  ElegantOTA.onStart([]() { 
      currentWifiState = WF_OTA; 
  }); 

  ElegantOTA.onEnd([](bool success) { 
      currentWifiState = WF_CONNECTED; 
      if (success) {
          digitalWrite(BUILTIN_LED_PIN, LOW); 
      } else {
          digitalWrite(BUILTIN_LED_PIN, HIGH);
      }
  }); 

  ElegantOTA.begin(&server);    
  server.begin();

  wifiOnStartTime = millis(); 
  currentWifiState = WF_CONNECTING; 
}

// ================= CORE AUDIO PROCESSING LOOP =================
void processAudio() {
  if (inSettingsMenu) {
      drawSettingsMenu();
      delay(15);
      return; 
  }

  size_t bytesIn = 0;
  esp_err_t result = i2s_read(I2S_PORT, &i2sData, sizeof(int32_t) * SAMPLES, &bytesIn, portMAX_DELAY);
  
  if (result == ESP_OK) {

    double dcOffset = 0;
    for (int i = 0; i < SAMPLES; i++) { dcOffset += (double)(i2sData[i] >> 14); }
    dcOffset /= SAMPLES;

    for (int i = 0; i < SAMPLES; i++) {
      vReal[i] = ((double)(i2sData[i] >> 14)) - dcOffset; 
      vImag[i] = 0.0;
    }
    
    int32_t rawWaveCenter = (i2sData[SAMPLES/2] >> 14) - dcOffset; 

    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();

    for (int i = 2; i < SAMPLES/2; i++) {
        if (i < 8) vReal[i] *= 0.5;         
        else if (i > 26) vReal[i] *= 0.5;   
    }

    int bandBins[5] = {2, 8, 16, 32, SAMPLES/2}; 

    for (int b = 0; b < 4; b++) {
      double bandSum = 0;
      int binCount = 0;
      for (int i = bandBins[b]; i < bandBins[b + 1]; i++) {
        bandSum += vReal[i];
        binCount++;
      }
      float avgMag = (binCount > 0) ? (bandSum / binCount) : 0;

      if (avgMag < NOISE_GATE[b]) bands[b] = 0;
      else bands[b] = avgMag;

      if (bands[b] > bandPeaks[b]) {
        bandPeaks[b] = bandPeaks[b] * AGC_ATTACK + bands[b] * (1.0 - AGC_ATTACK);
      } else {
        bandPeaks[b] = bandPeaks[b] * AGC_RELEASE;
        if (bandPeaks[b] < 1200) bandPeaks[b] = 1200; 
      }
      
      bandGain[b] = TARGET_LEVEL / bandPeaks[b];
      bandGain[b] = constrain(bandGain[b], 0.1, 6.0); 
    }

    float currentTotalGainAdjusted = (bands[0]*bandGain[0]) + (bands[1]*bandGain[1]) + (bands[2]*bandGain[2]) + (bands[3]*bandGain[3]);
    static float smoothedTotalAudio = 0;
    smoothedTotalAudio = (currentTotalGainAdjusted * SMOOTH_FACTOR) + (smoothedTotalAudio * (1.0 - SMOOTH_FACTOR));
    
    static float ultraSmoothAudio = 0;
    ultraSmoothAudio = (currentTotalGainAdjusted * 0.05) + (ultraSmoothAudio * 0.95);

    static uint8_t frameTick = 0;
    frameTick++;
    
    mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);

    switch (currentMode) {
      
      case 0: { 
        for (int i = 0; i < 32; i++) {
          int fftBin = i + 2; 
          float val = vReal[fftBin];
          int g = (i < 6) ? 0 : (i < 14) ? 1 : (i < 24) ? 2 : 3;
          float rawAmplified = val * bandGain[g];
          if (bands[g] == 0) rawAmplified = 0;
          smoothVal[i] = (rawAmplified * SMOOTH_FACTOR) + (smoothVal[i] * (1.0 - SMOOTH_FACTOR));
          int height = map((int)smoothVal[i], 0, (int)TARGET_LEVEL, 0, 8);
          height = constrain(height, 0, 8);
          if (height >= peaks[i]) peaks[i] = height; 
          else if (peaks[i] > 0) peaks[i]--; 
          uint8_t columnData = 0;
          for (int j = 0; j < peaks[i]; j++) columnData |= (1 << j);
          drawColumn(i, columnData); 
        }
        break;
      }
      
      case 1: { 
        static float terrain[32] = {1};
        static float horizonPhase = 0;
        const float HORIZON_SPEED = 0.4;  
        const float MAX_SLOPE = 1;        
        const float FILL_SPEED = 1.5;     
        
        horizonPhase += HORIZON_SPEED;
        if (horizonPhase >= 1.0) { 
          horizonPhase -= 1.0;
          for (int i = 0; i < 31; i++) terrain[i] = terrain[i + 1];
          float bassLevel = bands[0] * bandGain[0];
          float rawPeak = map(constrain(bassLevel, 0, TARGET_LEVEL), 0, TARGET_LEVEL, 1, 7);
          if (bands[2] * bandGain[2] > TARGET_LEVEL * 0.5) rawPeak += random(-1, 2); 
          terrain[31] = max((float)constrain(rawPeak, 1, 8), terrain[30] - MAX_SLOPE); 
        }
        for (int k = 30; k >= 0; k--) {
           float idealHeight = terrain[k+1] - MAX_SLOPE;
           if (terrain[k] < idealHeight) {
              terrain[k] += FILL_SPEED; 
              if (terrain[k] > idealHeight) terrain[k] = idealHeight;
           }
        }
        for (int i = 0; i < 32; i++) {
          uint8_t wireframeCol = 0;
          if (terrain[i] >= 1) wireframeCol = (1 << ((int)terrain[i] - 1));
          drawColumn(31 - i, wireframeCol);
        }
        break;
      }

      case 2: { 
        static uint8_t ribbon[16]; 
        float avgGain = (bandGain[0] + bandGain[1] + bandGain[2] + bandGain[3]) / 4.0;
        int32_t amplifiedWave = rawWaveCenter * avgGain;
        int dotY = constrain(map(amplifiedWave, -10000, 10000, 0, 7), 0, 7);
        if (frameTick % 2 == 0) {
          for(int i = 15; i > 0; i--) ribbon[i] = ribbon[i - 1]; 
          ribbon[0] = (1 << dotY); 
        }
        for (int i = 0; i < 16; i++) {
          drawColumn(31 - (15 - i), ribbon[i]); 
          drawColumn(31 - (16 + i), ribbon[i]); 
        }
        break;
      }

      case 3: { 
        static uint8_t gol[32];
        static uint8_t next_gol[32];
        static uint8_t decay[32][8] = {0}; 
        const uint8_t STICKY_FRAMES = 8;   

        if (frameTick % 4 == 0) { 
          if (random(0, 200) == 0) gol[random(0, 32)] ^= (1 << random(0, 8));
          if (bands[0] * bandGain[0] > TARGET_LEVEL * 0.6) { gol[1] |= 0b00001110; gol[2] |= 0b00000010; gol[3] |= 0b00000100; }
          if (bands[3] * bandGain[3] > TARGET_LEVEL * 0.5) { gol[29] |= (1 << random(0, 8)); gol[30] |= random(0, 255); }
          for (int x = 0; x < 32; x++) {
            uint8_t newCol = 0;
            for (int y = 0; y < 8; y++) {
              int neighbors = 0;
              for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                  if (dx == 0 && dy == 0) continue;
                  int nx = x + dx, ny = y + dy;
                  if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8) { if (gol[nx] & (1 << ny)) neighbors++; }
                }
              }
              bool alive = (gol[x] & (1 << y)) > 0;
              if (alive && (neighbors == 2 || neighbors == 3)) newCol |= (1 << y); 
              else if (!alive && neighbors == 3) newCol |= (1 << y);                
            }
            next_gol[x] = newCol;
          }
          for(int i = 0; i < 32; i++) gol[i] = next_gol[i];
        }
        for (int x = 0; x < 32; x++) {
          uint8_t renderCol = 0;
          for (int y = 0; y < 8; y++) {
            if (gol[x] & (1 << y)) { decay[x][y] = STICKY_FRAMES; renderCol |= (1 << y); } 
            else if (decay[x][y] > 0) { decay[x][y]--; renderCol |= (1 << y); }
          }
          drawColumn(31 - x, renderCol);
        }
        break;
      }

      case 4: { 
        static float timeOffset = 0; 
        static float snakeOffset = 0;
        timeOffset += 0.15; 
        snakeOffset += 0.03 + constrain((ultraSmoothAudio / TARGET_LEVEL) * 0.15, 0.0, 0.15); 
        float dynAmplitude = constrain((ultraSmoothAudio / (TARGET_LEVEL * 1.5)) * 3.5, 0.0, 3.5);
        for(int x = 0; x < 32; x++){
            int y = constrain((int)round(3.5 + dynAmplitude * sin((x * 0.2) - snakeOffset)), 0, 7);
            drawColumn(31 - x, (1 << y));
        }
        break;
      }

      case 5: { 
        static float ringR[6] = {0.0, 3.0, 6.0, 9.0, 12.0, 15.0}; 
        static float tunnelSpeed = 0.015; 
        float bassKick = (bands[0] * bandGain[0]) / TARGET_LEVEL;
        if(bassKick > 0.6) tunnelSpeed += (bassKick * 0.05); 
        if(tunnelSpeed > 0.25) tunnelSpeed = 0.25; 
        tunnelSpeed = (tunnelSpeed * 0.95) + (0.015 * 0.05); 
        uint8_t buffer[32] = {0};
        for(int i = 0; i < 6; i++) {
           ringR[i] += tunnelSpeed * (0.5 + (ringR[i] * 0.1)); 
           if(ringR[i] >= 20.0) ringR[i] -= 20.0; 
           int r = round(ringR[i]);
           if (r > 0 && r < 18) {
               int topY = 3 - r/2; int botY = 4 + r/2;
               if (topY >= 0 && topY < 8) { for(int x = max(0, 15-r); x <= min(31, 16+r); x++) buffer[x] |= (1 << topY); }
               if (botY >= 0 && botY < 8) { for(int x = max(0, 15-r); x <= min(31, 16+r); x++) buffer[x] |= (1 << botY); }
               for(int y = max(0, topY); y <= min(7, botY); y++) {
                   if (15-r >= 0 && 15-r < 32) buffer[15-r] |= (1 << y);
                   if (16+r >= 0 && 16+r < 32) buffer[16+r] |= (1 << y);
               }
           }
        }
        if((bands[3] * bandGain[3]) > TARGET_LEVEL * 0.4) { buffer[15] |= 0b00011000; buffer[16] |= 0b00011000; }
        for(int x=0; x<32; x++) drawColumn(31-x, buffer[x]);
        break;
      }

      case 6: { 
        uint8_t buffer[32] = {0};
        for(int b = 0; b < 4; b++) {
            float gainVal = bands[b] * bandGain[b];
            int halfHeight = map(constrain((long)gainVal, 0, TARGET_LEVEL), 0, TARGET_LEVEL, 0, 4);
            int flippedB = 3 - b; 
            int startX = flippedB * 8; 
            for(int x = startX; x < startX + 8; x++) { 
                for(int y = 3 - halfHeight; y <= 4 + halfHeight; y++) {
                    if (y >= 0 && y < 8) buffer[x] |= (1 << y);
                }
            }
        }
        for(int x=0; x<32; x++) drawColumn(31-x, buffer[x]);
        break;
      }

      case 7: { 
        static Particle particles[30];
        uint8_t buffer[32] = {0};
        float bassKick = bands[0] * bandGain[0];
        if (bassKick > TARGET_LEVEL * 0.6 || (bands[3] * bandGain[3]) > TARGET_LEVEL * 0.5) {
            int toSpawn = (bassKick > TARGET_LEVEL * 0.8) ? 3 : 1;
            for(int i = 0; i < 30 && toSpawn > 0; i++) {
                if(!particles[i].active) {
                    particles[i].active = true;
                    particles[i].x = random(0, 32);
                    particles[i].y = 0.0; 
                    particles[i].vy = (random(8, 20)/10.0 + (bassKick/(TARGET_LEVEL * 1.5))); 
                    toSpawn--;
                }
            }
        }
        for(int i = 0; i < 30; i++) {
            if(particles[i].active) {
                int px = (int)particles[i].x;
                int py = constrain((int)particles[i].y, 0, 7); 
                if (px >= 0 && px < 32) buffer[px] |= (1 << py);
                particles[i].vy -= 0.25; 
                if (particles[i].y < 0.0 && particles[i].vy < 0.0) {
                    if (particles[i].vy < -0.4) particles[i].vy = -0.4; 
                }
                particles[i].y += particles[i].vy;
                if(particles[i].y < -3.0) particles[i].active = false;
            }
        }
        for(int x=0; x<32; x++) drawColumn(31-x, buffer[x]);
        break;
      }

      case 8: { 
        static Star stars[30];
        static bool initStars = false;
        static float currentSpeed = 0.005; 
        if (!initStars) {
            for(int i=0; i<30; i++) {
                stars[i].x = random(-100, 100) / 100.0;
                stars[i].y = random(-100, 100) / 100.0;
                stars[i].z = random(10, 200) / 100.0;
            }
            initStars = true;
        }
        uint8_t buffer[32] = {0};
        float targetSpeed = 0.005 + (bands[0] * bandGain[0]) / (TARGET_LEVEL * 12.0); 
        if (targetSpeed > 0.06) targetSpeed = 0.06;
        if (targetSpeed > currentSpeed) currentSpeed = (currentSpeed * 0.7) + (targetSpeed * 0.3); 
        else currentSpeed = (currentSpeed * 0.93) + (targetSpeed * 0.07); 
        for(int i = 0; i < 30; i++) {
            stars[i].z -= currentSpeed;
            if(stars[i].z <= 0.1) { 
                stars[i].x = random(-100, 100) / 100.0;
                stars[i].y = random(-100, 100) / 100.0;
                stars[i].z = 2.0;
            }
            int px = 15.5 + (stars[i].x / stars[i].z) * 16;
            int py = 3.5 + (stars[i].y / stars[i].z) * 4;
            if(px >= 0 && px < 32 && py >= 0 && py < 8) {
                buffer[px] |= (1 << py);
                if (currentSpeed > 0.03) {
                    int prevPx = 15.5 + (stars[i].x / (stars[i].z + currentSpeed)) * 16;
                    int prevPy = 3.5 + (stars[i].y / (stars[i].z + currentSpeed)) * 4;
                    if (prevPx >= 0 && prevPx < 32 && prevPy >= 0 && prevPy < 8) buffer[prevPx] |= (1 << prevPy);
                }
            }
        }
        for(int x=0; x<32; x++) drawColumn(31-x, buffer[x]);
        break;
      }

      case 9: { 
        static float rainY[32] = {0};
        static float rainSpeed[32] = {0};
        float globalGravity = 0.2 + (bands[0] * bandGain[0] / (TARGET_LEVEL * 1.5)); 
        for(int x = 0; x < 32; x++) {
          if (rainSpeed[x] == 0) rainSpeed[x] = random(3, 8) / 10.0;
          rainY[x] += rainSpeed[x] * globalGravity; 
          bool hiHatsTriggered = (bands[3] * bandGain[3] > TARGET_LEVEL * 0.4);
          if (rainY[x] > 12.0 && random(0, 100) < (5 + (hiHatsTriggered ? 15 : 0))) {
            rainY[x] = 0;
            rainSpeed[x] = random(3, 8) / 10.0;
          }
          uint8_t colData = 0;
          int head = (int)rainY[x];
          if (head >= 0 && head < 8) colData |= (1 << (7 - head));              
          if (head - 1 >= 0 && head - 1 < 8) colData |= (1 << (7 - (head - 1))); 
          if (head - 3 >= 0 && head - 3 < 8) colData |= (1 << (7 - (head - 3))); 
          drawColumn(31 - x, colData);
        }
        break;
      }

      case 10: { 
        enum DecoderTheme { THEME_RAVE, THEME_ADEO };
        DecoderTheme currentTheme = THEME_ADEO; 
        const int GLITCH_CHANCE = 15;                
        const int SOLVE_CHANCE = 40;                 
        const int MAX_BEATS_FALLBACK = 64;           
        const unsigned long STICKY_DURATION = 600;  

        static const uint8_t font5x8[26][5] = {
          {0xFE, 0x11, 0x11, 0x11, 0xFE}, // A
          {0xFF, 0x89, 0x89, 0x89, 0x76}, // B
          {0x7E, 0x81, 0x81, 0x81, 0x42}, // C
          {0xFF, 0x81, 0x81, 0x42, 0x3C}, // D
          {0xFF, 0x89, 0x89, 0x89, 0x81}, // E
          {0xFF, 0x09, 0x09, 0x09, 0x01}, // F
          {0x7E, 0x81, 0x91, 0x91, 0x72}, // G 
          {0xFF, 0x08, 0x08, 0x08, 0xFF}, // H
          {0x00, 0x81, 0xFF, 0x81, 0x00}, // I
          {0x60, 0x80, 0x81, 0x7F, 0x01}, // J
          {0xFF, 0x18, 0x24, 0x42, 0x81}, // K 
          {0xFF, 0x80, 0x80, 0x80, 0x80}, // L
          {0xFF, 0x02, 0x0C, 0x02, 0xFF}, // M 
          //{0xFF, 0x06, 0x18, 0x60, 0xFF}, // N (Thicker diagonal bridge)
          {0xFF, 0x04, 0x08, 0x10, 0xFF}, // N (Thinner diagonal bridge)
          {0x7E, 0x81, 0x81, 0x81, 0x7E}, // O
          {0xFF, 0x11, 0x11, 0x11, 0x0E}, // P
          {0x7E, 0x81, 0xA1, 0x41, 0xBE}, // Q 
          {0xFF, 0x11, 0x31, 0x51, 0x8E}, // R
          {0x46, 0x89, 0x89, 0x89, 0x72}, // S
          {0x01, 0x01, 0xFF, 0x01, 0x01}, // T
          {0x7F, 0x80, 0x80, 0x80, 0x7F}, // U
          {0x3F, 0x40, 0x80, 0x40, 0x3F}, // V 
          {0x7F, 0x80, 0x70, 0x80, 0x7F}, // W 
          {0xC3, 0x24, 0x18, 0x24, 0xC3}, // X
          {0x07, 0x08, 0xF0, 0x08, 0x07}, // Y 
          {0xE1, 0x91, 0x89, 0x85, 0x83}  // Z 
        };

        static const char* raveWords[] = {
          "BASS", "DROP", "BEAT", "RAVE", "VIBE", "LOUD", "TECH", "SYNC",
          "BUMP", "KICK", "DAWN", "TRIP", "MIND", "SOUL", "FIRE", "WILD",
          "DUBZ", "HARD", "DEEP", "DARK", "LIVE", "PLAY", "JUMP", "EPIC",
          "ROCK", "NEON", "GLOW", "NOVA", "STAR", "BADD", "MASS", "RUDE", 
          "STEP", "ROLL", "AMEN", "CHOP", "DIRT", "GRIM", "HEVY", "CORE", 
          "SPIN", "HYPE", "MOSH", "DNBZ", "FLUX", "CREW", "WACK", "SICK", 
          "DOPE", "ILLZ", "BOMB", "ACID", "BOOM"
        };
        static const int numRaveWords = 53;

        static const char* adeoWords[] = {
          "CODE", "VIBE", "AIUX", "PMAP", "DATA", "BMAD", "ITER", "FLOW", 
          "USER", "OPEN", "TECH", "TEAM", "PLAN", "CART", "PROD", "LMUA", 
          "TEST", "LEAD", "GOAL", "TIME", "LOOP", "TASK", "PUSH", "PLAY", 
          "STEP", "SYNC", "MUST", "SOFT", "MINI", "NEXT", "SHIP", "WIRE", 
          "UXUX", "CLEV", "ZERO", "INFO", "NODE", "EPIC", "EDGE", "SCRM", 
          "RANK", "ADEO", "LOIC", "DIMA", "ALEX", "FRAN", "KNOW", "LLMS", 
          "MCPS", "TUNE", "CHAT", "AGNT", "ROAD", "SPEC", "DEMO", "LMFR", 
          "HOME", "SHOP"
        };
        static const int numAdeoWords = 58;

        const char* const* activeWords = (currentTheme == THEME_RAVE) ? raveWords : adeoWords;
        int numWords = (currentTheme == THEME_RAVE) ? numRaveWords : numAdeoWords;

        static const char* bannedWords[] = {
          "ANAL", "ANUS", "ARSE", "BOOB", "BOMB", "CLIT", "COCK", "CRAP", "CUNT", 
          "DAMN", "DICK", "DONG", "DYKE", "FART", "FUCK", "HELL", "HOMO", "JIZZ", 
          "KILL", "METH", "MUFF", "NAZI", "ORAL", "PIMP", "PISS", "PORN", "RAPE", 
          "SCAT", "SHIT", "SLAG", "SLIT", "SLUT", "SMUT", "SUCK", "TITS", "TURD", 
          "TWAT", "WANK", "WEED"
        };
        static const int numBannedWords = sizeof(bannedWords) / sizeof(bannedWords[0]);

        static int blockLetters[4] = {0, 0, 0, 0};     
        static int targetWordIdx = 0;                  
        static int beatCount = 0;                      
        static unsigned long lastTypeTime = 0;         
        static float moduleBright[4] = {(float)globalIdleBrightness, (float)globalIdleBrightness, (float)globalIdleBrightness, (float)globalIdleBrightness}; 
        static bool hasCelebrated = false;             
        static bool isWordSticky = false;              
        static unsigned long wordSolvedTime = 0;       
        
        auto isWordSafe = [&](int blockToChange, int newLetter) -> bool {
            char testStr[5];
            for(int i = 0; i < 4; i++) {
                if (i == blockToChange) testStr[i] = (char)('A' + newLetter);
                else testStr[i] = (char)('A' + blockLetters[i]);
            }
            testStr[4] = '\0'; 
            for(int w = 0; w < numBannedWords; w++) {
                if (strcmp(testStr, bannedWords[w]) == 0) return false; 
            }
            return true; 
        };

        static bool initWords = false;
        if (!initWords) {
            bool safe = false;
            while (!safe) {
                for(int i = 0; i < 4; i++) blockLetters[i] = random(0, 26);
                safe = isWordSafe(-1, 0); 
            }
            targetWordIdx = random(0, numWords);
            initWords = true;
        }

        if (targetWordIdx >= numWords) targetWordIdx = 0;

        float bass = bands[0] * bandGain[0];
        float treble = bands[3] * bandGain[3];

        if ((bass > TARGET_LEVEL * 0.6 || treble > TARGET_LEVEL * 0.6) && millis() - lastTypeTime > 200) {
          if (isWordSticky) {
              for(int i=0; i<4; i++) moduleBright[i] = globalMaxBrightness; 
              if (millis() - wordSolvedTime > STICKY_DURATION) {
                  isWordSticky = false;  
                  hasCelebrated = false; 
                  beatCount = 0;         
                  int newIdx = random(0, numWords);
                  while(newIdx == targetWordIdx) newIdx = random(0, numWords); 
                  targetWordIdx = newIdx;
              }
              lastTypeTime = millis();
          } 
          else {
              beatCount++;
              if (beatCount > MAX_BEATS_FALLBACK) { 
                  int newIdx = random(0, numWords);
                  while(newIdx == targetWordIdx) newIdx = random(0, numWords);
                  targetWordIdx = newIdx;
                  beatCount = 0;
                  hasCelebrated = false; 
              }
              int correctBlocks[4]; int numCorrect = 0;
              int wrongBlocks[4];   int numWrong = 0;
              for (int i=0; i<4; i++) {
                  if (blockLetters[i] == (activeWords[targetWordIdx][i] - 'A')) correctBlocks[numCorrect++] = i;
                  else wrongBlocks[numWrong++] = i;
              }

              bool wordIsComplete = (numCorrect == 4);

              if (wordIsComplete && !hasCelebrated) {
                  for(int i=0; i<4; i++) moduleBright[i] = globalMaxBrightness; 
                  hasCelebrated = true; 
                  isWordSticky = true;       
                  wordSolvedTime = millis(); 
                  lastTypeTime = millis();
              } 
              else {
                  int activeBlock = -1;
                  bool doGlitch = (random(0, 100) < GLITCH_CHANCE) && (numCorrect > 0);
                  if (numWrong == 0) doGlitch = true; 

                  if (doGlitch) {
                      activeBlock = correctBlocks[random(0, numCorrect)];
                      int targetLetter = activeWords[targetWordIdx][activeBlock] - 'A';
                      int chaoticLetter;
                      do { chaoticLetter = random(0, 26); } while (chaoticLetter == targetLetter || !isWordSafe(activeBlock, chaoticLetter));
                      blockLetters[activeBlock] = chaoticLetter;
                  } 
                  else {
                      activeBlock = wrongBlocks[random(0, numWrong)];
                      int targetLetter = activeWords[targetWordIdx][activeBlock] - 'A';
                      if (random(0, 100) < SOLVE_CHANCE) {
                          blockLetters[activeBlock] = targetLetter; 
                      } else {
                          int chaoticLetter;
                          do { chaoticLetter = random(0, 26); } 
                          while (chaoticLetter == blockLetters[activeBlock] || chaoticLetter == targetLetter || !isWordSafe(activeBlock, chaoticLetter));
                          blockLetters[activeBlock] = chaoticLetter;
                      }
                  }
                  moduleBright[activeBlock] = globalMaxBrightness; 
                  lastTypeTime = millis();
              }
          }
        }
        uint8_t buffer[32] = {0};
        for (int m = 0; m < 4; m++) {
          int letter = blockLetters[m];
          int startX = m * 8 + 1; 
          for (int c = 0; c < 5; c++) {
            uint8_t colData = font5x8[letter][c]; 
            uint8_t flippedCol = 0;
            for(int b = 0; b < 8; b++) { if(colData & (1 << b)) flippedCol |= (1 << (7 - b)); }
            buffer[startX + c] = flippedCol; 
          }
        }
        for (int x = 0; x < 32; x++) { drawColumn(x, buffer[x]); }
        for (int m = 0; m < 4; m++) {
            moduleBright[m] -= 0.5; 
            if (moduleBright[m] < globalIdleBrightness) moduleBright[m] = globalIdleBrightness; 
            int physDevice = 3 - m; 
            mx.control(physDevice, MD_MAX72XX::INTENSITY, (int)moduleBright[m]);
        }
        break;
      }
    }
    
    if (currentMode != 10) { 
        int idleBrightness = globalIdleBrightness;                 
        int maxBrightness = globalMaxBrightness;
        int intendedBrightness = idleBrightness;
        
        float bassVal = bands[0] * bandGain[0];
        if (bassVal > TARGET_LEVEL * 0.75) {    
            intendedBrightness = map((long)bassVal, (long)(TARGET_LEVEL * 0.75), (long)TARGET_LEVEL,
                                     idleBrightness + 3, maxBrightness);   
        } else if ((bands[3] * bandGain[3]) > TARGET_LEVEL * 0.5) {
            intendedBrightness = map((long)(bands[3] * bandGain[3]),
                                     (long)(TARGET_LEVEL * 0.5), (long)TARGET_LEVEL,
                                     idleBrightness + 2, constrain(maxBrightness - 3, idleBrightness, maxBrightness));   
        }
        
        static float currentBrightness = (float)globalIdleBrightness;  
        if (intendedBrightness > currentBrightness) {
            currentBrightness = intendedBrightness;
        } else {
            currentBrightness -= 1.5;            
            if (currentBrightness < idleBrightness) currentBrightness = idleBrightness;
        }
        mx.control(MD_MAX72XX::INTENSITY, constrain((int)currentBrightness, idleBrightness, maxBrightness));
    }

    mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON); 
  }
}

// ================= ARDUINO MAIN LOOP =================
void loop() {
  
  wm.process(); 
  server.handleClient(); 
  ElegantOTA.loop();     

  bool shouldWifiBeOn = wifiAlwaysOn || (millis() - wifiOnStartTime < WIFI_TIMEOUT_MS) || (currentWifiState == WF_OTA);

  if (shouldWifiBeOn) {
      if (currentWifiState == WF_OFF) {
          WiFi.setHostname("glowbar"); 
          WiFi.mode(WIFI_STA);
          WiFi.begin();
          currentWifiState = WF_CONNECTING;
      }
      
      if (currentWifiState != WF_OTA) {
          if (WiFi.status() == WL_CONNECTED) {
              currentWifiState = WF_CONNECTED;
              ArduinoOTA.handle(); 
          } else {
              currentWifiState = WF_CONNECTING;
          }
      } else {
          ArduinoOTA.handle(); 
      }
  } else {
      if (currentWifiState != WF_OFF) {
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          currentWifiState = WF_OFF;
      }
  }

  // ----------------------------------------------------
  // LED BLINK LOGIC
  // ----------------------------------------------------
  static unsigned long lastLedToggle = 0;
  static bool ledState = HIGH; 

  if (currentWifiState == WF_OFF) {
      digitalWrite(BUILTIN_LED_PIN, HIGH); // OFF
  } else if (currentWifiState == WF_CONNECTING) {
      if (millis() - lastLedToggle > 500) { 
          // Slow, even blink while searching for network
          ledState = !ledState;
          digitalWrite(BUILTIN_LED_PIN, ledState);
          lastLedToggle = millis();
      }
  } else if (currentWifiState == WF_CONNECTED) {
      if (wifiAlwaysOn) {
          // PERMANENT MODE: Solid ON
          digitalWrite(BUILTIN_LED_PIN, LOW); 
      } else {
          // TEMPORARY 2-MIN MODE: Mostly ON, brief flick OFF every 1 second
          int cycle = millis() % 1000;
          if (cycle < 50) {
              digitalWrite(BUILTIN_LED_PIN, HIGH); // OFF (Brief flick)
          } else {
              digitalWrite(BUILTIN_LED_PIN, LOW);  // ON (Mostly on)
          }
      }
  }

  bool bootBtnState = digitalRead(BOOT_BTN_PIN);
  
  if (bootBtnState == LOW && bootBtnLastState == HIGH) {
      bootBtnPressTime = millis();
      bootBtnHandled = false;
  } 
  else if (bootBtnState == LOW && bootBtnLastState == LOW) {
      if (!bootBtnHandled && (millis() - bootBtnPressTime > 1500)) {
          bootBtnHandled = true;
          
          if (currentWifiState == WF_OTA) {
              return; 
          }

          if (currentWifiState != WF_OFF) {
              wifiAlwaysOn = false;
              wifiOnStartTime = millis() - WIFI_TIMEOUT_MS - 1000; 
          } else {
              wifiAlwaysOn = true;
              wifiOnStartTime = millis(); 
          }
          
          preferences.putBool("wifiOn", wifiAlwaysOn);

          for(int i=0; i<4; i++) {
              digitalWrite(BUILTIN_LED_PIN, LOW); delay(80);
              digitalWrite(BUILTIN_LED_PIN, HIGH); delay(80);
          }
      }
  } 
  else if (bootBtnState == HIGH && bootBtnLastState == LOW) {
      if (!bootBtnHandled && (millis() - bootBtnPressTime > 50)) {
          wifiOnStartTime = millis(); 
      }
  }
  bootBtnLastState = bootBtnState;

  processAudio(); 

  bool currentTouchState = digitalRead(TOUCH_PIN);
  
  if (inSettingsMenu) {
      if (millis() - settingsEnterTime < 1000) { } 
      else {
          if (currentTouchState == HIGH && lastTouchState == LOW) {
              touchStartTime = millis();
              isLongPressHandled = false;
          } 
          else if (currentTouchState == HIGH && lastTouchState == HIGH) {
              if (!isLongPressHandled && (millis() - touchStartTime > 400)) {
                  isLongPressHandled = true; 
                  lastRampTime = millis();
                  if (globalMaxBrightness >= 15) settingsBrightnessDirection = -1; 
                  else if (globalMaxBrightness <= 6) settingsBrightnessDirection = 1;  
              }
              if (isLongPressHandled && (millis() - lastRampTime > 300)) { 
                  lastRampTime = millis();
                  int nextVal = globalMaxBrightness + settingsBrightnessDirection;
                  bool hitLimit = false;

                  if (nextVal > 15) { nextVal = 15; hitLimit = true; } 
                  else if (nextVal < 6) { nextVal = 6; hitLimit = true; }

                  globalMaxBrightness = nextVal;
                  mx.control(MD_MAX72XX::INTENSITY, globalMaxBrightness); 

                  if (hitLimit) {
                      mx.control(MD_MAX72XX::SHUTDOWN, 1);
                      delay(100);
                      mx.control(MD_MAX72XX::SHUTDOWN, 0);
                  }
                  
                  preferences.putUInt("maxBri", globalMaxBrightness);
                  if (globalMaxBrightness >= 11) globalIdleBrightness = 4;
                  else if (globalMaxBrightness == 6) globalIdleBrightness = 0;
                  else globalIdleBrightness = 1;
                  preferences.putUInt("idleBri", globalIdleBrightness);
              }
          } 
          else if (currentTouchState == LOW && lastTouchState == HIGH) {
              if (isLongPressHandled) {
                  settingsBrightnessDirection *= -1;
              } else {
                  if (millis() - lastTapTime > TAP_TIMEOUT) tapCount = 1;
                  else tapCount++;
                  lastTapTime = millis();

                  if (tapCount == 2) {
                      rotateScreen = !rotateScreen;
                      preferences.putBool("rotate", rotateScreen);
                      tapCount = 0; 
                      mx.clear();
                  }
              }
          }
          if (tapCount > 0 && currentTouchState == LOW && (millis() - lastTapTime > TAP_TIMEOUT)) {
              if (tapCount == 1) {
                  inSettingsMenu = false;
                  mx.clear();
                  mx.control(MD_MAX72XX::INTENSITY, globalIdleBrightness); 
              }
              tapCount = 0;
          }
      }
  } 
  else {
      if (currentTouchState == HIGH && lastTouchState == LOW) {
        touchStartTime = millis();
        isLongPressHandled = false;
      } 
      else if (currentTouchState == HIGH && lastTouchState == HIGH) {
        if (!isLongPressHandled && (millis() - touchStartTime > 800)) {
          isLongPressHandled = true;
          tapCount = 0; 
          enterSleepMode(); 
        }
      } 
      else if (currentTouchState == LOW && lastTouchState == HIGH) {
        if (!isLongPressHandled) {
          if (millis() - lastTapTime > TAP_TIMEOUT) {
             tapCount = 1;
             modeBeforeTaps = currentMode; 
          } else { tapCount++; }
          
          lastTapTime = millis();

          if (tapCount == 5) {
             inSettingsMenu = true;
             settingsEnterTime = millis();
             tapCount = 0;
             currentMode = modeBeforeTaps; 
             mx.clear(); 
             mx.control(MD_MAX72XX::INTENSITY, globalMaxBrightness); 
          } 
          else {
             currentMode++;
             if (currentMode > 10) currentMode = 0; 
             preferences.putUInt("mode", currentMode); 
             mx.clear(); 
          }
        }
      }
      if (tapCount > 0 && currentTouchState == LOW && (millis() - lastTapTime > TAP_TIMEOUT)) tapCount = 0;
  }
  
  lastTouchState = currentTouchState; 

  if (Serial.available() > 0) {
    String incomingStr = Serial.readStringUntil('\n');
    int newMode = incomingStr.toInt() - 1; 
    if (newMode >= 0 && newMode <= 10 && currentMode != newMode) { 
      currentMode = newMode;
      preferences.putUInt("mode", currentMode); 
      mx.clear();
    }
  }

  delay(15); 
}
