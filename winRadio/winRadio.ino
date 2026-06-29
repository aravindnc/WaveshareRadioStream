#include "Arduino.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include "Audio.h"
#include "SD_MMC.h"
#include "FS.h"
#include <Arduino_GFX_Library.h>
#include <LovyanGFX.hpp> 
#include "es8311.h"
#include "esp_check.h"
#include "Wire.h"
#include <time.h>
#include <Preferences.h>

// Uncomment below and put your own WIFI_SSID/WIFI_PASS
#include "arduino_secrets.h"

// Libraries used by this project
// Note that these are all behind current releases. In particular, the
// audio library made big changes in callbacks that will require
// some changes before catching up
//
// ESP32-audioI2S 3.4.0 https://github.com/schreibfaul1/ESP32-audioI2S
// Arduino_GFX 1.6.0 https://github.com/moononournation/Arduino_GFX
// LovyanGFX 1.2.19 https://github.com/lovyan03/LovyanGFX

#define PA_CTRL 7
#define I2S_MCLK 8
#define I2S_BCLK 9
#define I2S_DOUT 12
#define I2S_LRC 10

#define I2C_SDA 42
#define I2C_SCL 41

#define EXAMPLE_SAMPLE_RATE (16000)
#define EXAMPLE_MCLK_MULTIPLE (256)  // If not using 24-bit data width, 256 should be enough
#define EXAMPLE_MCLK_FREQ_HZ (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define EXAMPLE_VOICE_VOLUME (75)

// ---- Configuration Constants ----
const char* STATIONS_URL = "https://fm.aravindnc.com/stations.txt";
const unsigned long IDLE_SLEEP_TIMEOUT = 45UL * 60UL * 1000UL; // 45 minutes
const unsigned long STOPPED_SLEEP_TIMEOUT = 5UL * 60UL * 1000UL; // 5 minutes
const unsigned long SCREENSAVER_TIMEOUT = 30000; // 60 seconds
const unsigned long SCREEN_DIM_TIMEOUT = 15000; // 15 seconds
const int SCREEN_BRIGHTNESS_NORMAL = 90; // 0-255
const int SCREEN_BRIGHTNESS_DIM = 2;      // 0-255
const long GMT_OFFSET_SEC = 19800;        // UTC+5:30 (India)
const int DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER = "pool.ntp.org";
const char* WIFI_SSID = SECRET_SSID;
const char* WIFI_PASS = SECRET_PASS;
// ---------------------------------

LGFX_Sprite sprite; 
LGFX_Sprite sprite2; 
Preferences preferences; 

String curStation="";
String songPlaying="";
long bitrate=0;
bool connected=false;
bool isScreensaver=false;
int songposition=-220;
float voltage=4.20;
int batLevel=0;
unsigned long lastInteraction = 0;
unsigned long lastAudioTime = 0;

Audio audio;

bool canDraw=0;
bool deb=0;
bool deb2=0;
int rssi=0;

int clk = 16;
int cmd = 15;
int d0 = 17;
int d1 = 18;
int d2 = 13;
int d3 = 14;


int chosen=0; //current station
int volume=4;
String letters[3]={"P","S","V"};

unsigned short grays[18];
unsigned short gray;
unsigned short light;

int g[14]={0};  //graph

struct Station {
  String name;
  String url;
};
Station station_list[100];
int ns = 0;

#define GFX_BL 46
Arduino_DataBus* bus = new Arduino_ESP32SPI(45 /* DC */, 21 /* CS */, 38 /* SCK */, 39 /* MOSI */, -1 /* MISO */);
Arduino_GFX* gfx = new Arduino_ST7789(
bus, 40 /* RST */, 0 /* rotation */, true, 240, 240);                

static esp_err_t es8311_codec_init(void) {

  es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,
    .sample_frequency = EXAMPLE_SAMPLE_RATE
  };

  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE), TAG, "set es8311 sample frequency failed");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");

  return ESP_OK;
}




void setup() {

  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  gpio_hold_dis((gpio_num_t)2);
  pinMode(0, INPUT_PULLUP); // left na GPIO0
  pinMode(5, INPUT_PULLUP); // mid button
  pinMode(4, INPUT_PULLUP); // right button

  // batt enable
  pinMode(2,OUTPUT);
  digitalWrite(2,HIGH);

  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);
  es8311_codec_init();
  gpio_hold_en((gpio_num_t)2);

  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);

  analogWrite(GFX_BL, SCREEN_BRIGHTNESS_NORMAL);   //SCREEN BRIGHTNESS 0-255
  lastInteraction = millis();
  lastAudioTime = millis();

  gfx->setCursor(0, 16);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_GREEN, BLACK);
  gfx->println("Connecting to WI-FI");
  gfx->setCursor(0, 32);
  gfx->println("Scanning...");

  sprite.setColorDepth(16);     // RGB565
  sprite.createSprite(240, 240); 
  sprite2.createSprite(230, 16); 
  
     int co = 214;
    for (int i = 0; i < 18; i++) {
    grays[i] = sprite.color565(co, co, co+40);
    co = co - 13;
    }

  sprite2.setTextColor(grays[0],TFT_BLACK);

  // 1.8 Register WiFi Event Listener for verbose diagnostics
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
      Serial.printf("WiFi Event: %d\n", (int)event);
      if (event == ARDUINO_EVENT_WIFI_STA_START) {
          Serial.println("STA Mode Started");
      } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
          Serial.println("Connected to AP");
      } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
          Serial.printf("Disconnected! Reason: %d\n", (int)info.wifi_sta_disconnected.reason);
      } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
          Serial.print("Got IP: ");
          Serial.println(WiFi.localIP().toString());
      }
  });

  bool conn_success = false;

  gfx->fillRect(0, 16, 240, 100, RGB565_BLACK);
  gfx->setCursor(0, 16);
  gfx->setTextColor(RGB565_GREEN, BLACK);
  gfx->println("Connecting WiFi:");
  gfx->setTextColor(RGB565_YELLOW, BLACK);
  gfx->println(WIFI_SSID);
  gfx->setTextColor(RGB565_GREEN, BLACK);

  Serial.println("Connecting WiFi...");

  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(100);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (attempts < 60) { // Try for 15 seconds (60 * 250ms)
      if (WiFi.status() == WL_CONNECTED) {
          conn_success = true;
          break;
      }
      Serial.printf("[%d] Waiting for connection... Status: %d\n", attempts, WiFi.status());
      gfx->print(".");
      delay(250);
      attempts++;

      // Handle sleep button
      if (digitalRead(0) == LOW) {
          gfx->fillRect(0, 80, 240, 40, RGB565_BLACK);
          gfx->setCursor(0, 80);
          gfx->setTextColor(RGB565_RED, BLACK);
          gfx->println("Sleeping...");
          delay(1000);
          analogWrite(GFX_BL, 0); // Turn off backlight
          esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Wake up on left button
          digitalWrite(PA_CTRL, LOW);
          delay(200);
          esp_deep_sleep_start();
      }
  }

  if (!conn_success) {
      Serial.println("WiFi Connection Failed.");
      gfx->fillRect(0, 16, 240, 200, RGB565_BLACK);
      gfx->setCursor(0, 16);
      gfx->setTextColor(RGB565_RED, BLACK);
      gfx->println("WiFi Connection Failed.");
      gfx->println("Please restart or");
      gfx->println("check router settings.");
      gfx->println("");
      gfx->setTextColor(RGB565_GREEN, BLACK);
      gfx->println("Press MID to retry,");
      gfx->println("or LEFT to sleep.");
      
      while (true) {
          if (digitalRead(5) == LOW) { // Mid button to retry
              gfx->fillRect(0, 80, 240, 40, RGB565_BLACK);
              gfx->setCursor(0, 80);
              gfx->println("Retrying...");
              delay(1000);
              ESP.restart();
          }
          if (digitalRead(0) == LOW) { // Left button to sleep
              gfx->fillRect(0, 80, 240, 40, RGB565_BLACK);
              gfx->setCursor(0, 80);
              gfx->setTextColor(RGB565_RED, BLACK);
              gfx->println("Sleeping...");
              delay(1000);
              analogWrite(GFX_BL, 0); // Turn off backlight
              esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Wake up on left button
              digitalWrite(PA_CTRL, LOW);
              delay(200);
              esp_deep_sleep_start();
          }
          delay(50);
      }
  }

  Serial.println("\nWiFi connected!");
  Serial.println(WiFi.localIP());
  gfx->fillRect(0, 32, 240, 200, RGB565_BLACK);
  gfx->setCursor(0, 32);
  gfx->setTextColor(RGB565_GREEN, BLACK);
  gfx->println("WiFi Connected!");
  gfx->println(WiFi.localIP().toString());
  delay(1500);
configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

Serial.println("Fetching remote stations list...");
HTTPClient http;
http.begin(STATIONS_URL);
int httpCode = http.GET();
if (httpCode == HTTP_CODE_OK) {
  String payload = http.getString();
  int lineStart = 0;
  while(lineStart < payload.length() && ns < 100) {
    int lineEnd = payload.indexOf('\n', lineStart);
    if (lineEnd == -1) lineEnd = payload.length();
    String line = payload.substring(lineStart, lineEnd);
    line.trim();
    if (line.length() > 0 && line.indexOf('|') > 0) {
      int pipeIdx = line.indexOf('|');
      String sname = line.substring(0, pipeIdx);
      String surl = line.substring(pipeIdx + 1);
      sname.trim();
      surl.trim();
      
      // Auto-convert https to http to prevent SSL OOM errors
      if (surl.startsWith("https://")) {
          surl.replace("https://", "http://");
      }
      
      station_list[ns].name = sname;
      station_list[ns].url = surl;
      ns++;
    }
    lineStart = lineEnd + 1;
  }
} else {
  Serial.println("Failed to fetch stations, using fallback.");
}
http.end();

if (ns == 0) {
  station_list[0].name = "Fallback Station";
  station_list[0].url = "http://air.pc.cdn.bitgravity.com/air/live/pbaudio230/playlist.m3u8";
  ns = 1;
}

  // Load last played station from preferences
  preferences.begin("radio", true);
  String lastStation = preferences.getString("last_station", "");
  preferences.end();

  if (lastStation.length() > 0) {
      for (int i = 0; i < ns; i++) {
          if (station_list[i].name == lastStation) {
              chosen = i;
              break;
          }
      }
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  audio.setVolume(volume*2); // 0...21
  audio.connecttohost(station_list[chosen].url.c_str());
}

void drawScreensaver()
{
    sprite.fillSprite(TFT_BLACK);
    
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
        char timeStr[10];
        char ampmStr[5];
        int hour = timeinfo.tm_hour % 12;
        if (hour == 0) hour = 12;
        sprintf(timeStr, "%d:%02d", hour, timeinfo.tm_min);
        strftime(ampmStr, sizeof(ampmStr), "%p", &timeinfo);
        // Calculate combined width of (Time in Font 0 size 5) + (Gap) + (AM/PM in Font 0 size 2)
        sprite.setFont(&fonts::Font0);
        sprite.setTextSize(5);
        int clockWidth = sprite.textWidth(timeStr);
        
        sprite.setFont(&fonts::Font0);
        sprite.setTextSize(2);
        int ampmWidth = sprite.textWidth(ampmStr);
        
        int combinedWidth = clockWidth + 6 + ampmWidth;
        int clockX = 120 - (combinedWidth / 2);
        int ampmX = clockX + clockWidth + 6;
        
        // Draw time digits (shifted up to y=42 to make room for wrapping station name)
        sprite.setFont(&fonts::Font0);
        sprite.setTextSize(5);
        sprite.setTextDatum(0); // Top Left
        sprite.setTextColor(TFT_GREEN, TFT_BLACK);
        sprite.drawString(timeStr, clockX, 42);
        
        // Draw AM/PM next to clock
        sprite.setFont(&fonts::Font0);
        sprite.setTextSize(2);
        sprite.setTextDatum(0); // Top Left
        sprite.setTextColor(grays[4], TFT_BLACK);
        sprite.drawString(ampmStr, ampmX, 54); // Centered vertically next to clock (y=62 - 8 = 54)
        sprite.setTextSize(1); // Reset
        
        // Wrap station name to 1 or 2 lines for Size 3 (wrap at 9 characters without breaking words)
        String line1 = "";
        String line2 = "";
        String name = station_list[chosen].name;
        name.trim();

        int pos = 0;
        while (pos < name.length()) {
            int nextSpace = name.indexOf(' ', pos);
            String word;
            if (nextSpace == -1) {
                word = name.substring(pos);
                pos = name.length();
            } else {
                word = name.substring(pos, nextSpace);
                pos = nextSpace + 1;
            }
            word.trim();
            if (word.length() == 0) continue;

            if (line1.length() == 0) {
                if (word.length() <= 9) {
                    line1 = word;
                } else {
                    line1 = word.substring(0, 9);
                }
            } else if (line1.length() + 1 + word.length() <= 9) {
                line1 += " " + word;
            } else {
                if (line2.length() == 0) {
                    if (word.length() <= 9) {
                        line2 = word;
                    } else {
                        line2 = word.substring(0, 9);
                    }
                } else if (line2.length() + 1 + word.length() <= 9) {
                    line2 += " " + word;
                } else {
                    // Word breaks/truncates, so skip it and stop wrapping
                    break;
                }
            }
        }
        
        // Draw station name lines (manually centered with Size 3 for larger look)
        sprite.setFont(&fonts::Font0);
        sprite.setTextSize(3);
        sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
        
        if (line2.length() > 0) {
            int w1 = sprite.textWidth(line1);
            sprite.drawString(line1, 120 - (w1 / 2), 102);
            
            int w2 = sprite.textWidth(line2);
            sprite.drawString(line2, 120 - (w2 / 2), 132);
        } else {
            int w1 = sprite.textWidth(line1);
            sprite.drawString(line1, 120 - (w1 / 2), 117);
        }
        sprite.setTextSize(1); // Reset scale
        
        // Calculate remaining sleep time
        unsigned long idleTime = millis() - lastInteraction;
        unsigned long stoppedTime = millis() - lastAudioTime;
        unsigned long remainingMs = 0;
        if (audio.isRunning()) {
            if (IDLE_SLEEP_TIMEOUT > idleTime) {
                remainingMs = IDLE_SLEEP_TIMEOUT - idleTime;
            }
        } else {
            unsigned long remIdle = (IDLE_SLEEP_TIMEOUT > idleTime) ? (IDLE_SLEEP_TIMEOUT - idleTime) : 0;
            unsigned long remStop = (STOPPED_SLEEP_TIMEOUT > stoppedTime) ? (STOPPED_SLEEP_TIMEOUT - stoppedTime) : 0;
            remainingMs = (remIdle < remStop) ? remIdle : remStop;
        }
        unsigned long remainingMins = (remainingMs + 59999) / 60000;
        char sleepBuf[30];
        if (remainingMs == 0) {
            sprintf(sleepBuf, "Sleeping...");
        } else if (remainingMins == 1) {
            sprintf(sleepBuf, "Sleeps in 1 min");
        } else {
            sprintf(sleepBuf, "Sleeps in %lu mins", remainingMins);
        }

        // Draw sleep timer status below station name (manually centered with 2x scaled Font 0 for 8-bit look)
        sprite.setFont(&fonts::Font0);
        sprite.setTextSize(2);
        int sleepWidth = sprite.textWidth(sleepBuf);
        int sleepX = 120 - (sleepWidth / 2);
        sprite.setTextDatum(0); // Use Top-Left alignment to bypass datum reset bugs
        sprite.setTextColor(grays[4], TFT_BLACK);
        sprite.drawString(sleepBuf, sleepX, 182); // Centered vertically at 190 (190 - 8 = 182)
        sprite.setTextSize(1); // Reset scale
        
        sprite.setTextDatum(0); // reset to top left
    } else {
        sprite.setFont(&fonts::Font0);
        sprite.setTextSize(2);
        sprite.setTextDatum(4);
        sprite.setTextColor(grays[0], TFT_BLACK);
        sprite.drawString("Syncing Time...", 120, 130);
        sprite.setTextDatum(0);
        sprite.setTextSize(1);
    }

    uint16_t *buf = (uint16_t*)sprite.getBuffer();
    int total = 240 * 240;
    for (int i = 0; i < total; i++) {
        buf[i] = __builtin_bswap16(buf[i]);
    }
    gfx->draw16bitRGBBitmap(0, 0, buf, 240, 240);
    canDraw = 0;
}

void draw2()
{
    sprite.setFont(&fonts::Font0);
    sprite.setTextSize(1);

    gray=grays[16];
    light=grays[12];
    sprite.fillRect(0,0,240,240,gray);
    
    //stations frame
    sprite.fillRect(4,20,150,172,BLACK);
    sprite.drawRect(4,20,150,172,light);

    // time and grapg frame
    sprite.fillRect(160,20,74,60,BLACK);
    sprite.drawRect(160,20,74,60,light);
    
    // 1. Draw Wifi signal bars (Row 1: WIFI: <bars>)
    sprite.setTextColor(TFT_GREEN, TFT_BLACK);
    sprite.setTextSize(1);
    sprite.drawString("WIFI", 165, 26);

    int numBars = 0;
    if (rssi > -60) numBars = 4;
    else if (rssi > -70) numBars = 3;
    else if (rssi > -80) numBars = 2;
    else if (rssi > -90) numBars = 1;
    
    for (int b = 0; b < 4; b++) {
        uint16_t barColor = (b < numBars) ? TFT_GREEN : grays[4];
        int barHeight = 3 + (b * 2);
        sprite.fillRect(214 + (b * 4), 35 - barHeight, 2, barHeight, barColor);
    }

    uint16_t batColor = (voltage < 3.4) ? TFT_RED : TFT_GREEN;
    sprite.setTextColor(batColor, TFT_BLACK);
    sprite.setTextSize(1);
    sprite.drawString(String(voltage) + "V", 165, 42);

    sprite.drawRect(214, 42, 15, 8, batColor);
    sprite.fillRect(229, 44, 1, 4, batColor); // battery tip

    if (voltage >= 4.15) {
        // Draw orange lightning bolt in the middle
        sprite.fillRect(221, 43, 2, 1, ORANGE);
        sprite.fillRect(220, 44, 2, 1, ORANGE);
        sprite.fillRect(219, 45, 4, 1, ORANGE);
        sprite.fillRect(220, 46, 2, 1, ORANGE);
        sprite.fillRect(219, 47, 2, 1, ORANGE);
        sprite.fillRect(218, 48, 1, 1, ORANGE);
    } else {
        int scaledBat = (batLevel * 11) / 13;
        sprite.fillRect(216, 44, scaledBat, 4, batColor);
    }

    //bitrate
    sprite.fillRect(160,176,74,16,BLACK);
    sprite.drawRect(160,176,74,16,light);

    //volume bar
    sprite.fillRoundRect(160,140,74,3,2,YELLOW);
    sprite.fillRoundRect(146+((volume*15)/2),137,14,8,2,grays[2]);
    sprite.fillRoundRect(149+((volume*15)/2),139,8,4,2,grays[10]);

    //songplaying frame
    sprite.fillRect(4,212,232,18,BLACK);
    sprite.drawRect(4,212,232,18,light);

    sprite.fillRect(149,20,5,172,grays[11]);

    int sliderPos = 12;
    if (ns > 1) {
        sliderPos = 12 + (chosen * 152) / (ns - 1);
    }
    sprite.fillRect(149,sliderPos+8,5,20,grays[2]);
    sprite.fillRect(151,sliderPos+12,1,12,grays[16]);

    // Split the left header line to leave a gap for "STATIONS"
    sprite.fillRect(4, 7, 24, 3, ORANGE);
    sprite.fillRect(131, 7, 23, 3, ORANGE);
    sprite.fillRect(4, 13, 24, 3, grays[6]);
    sprite.fillRect(131, 13, 23, 3, grays[6]);
   
    // Symmetrical header lines next to WEB
    sprite.fillRect(160, 7, 16, 3, ORANGE);
    sprite.fillRect(219, 7, 16, 3, ORANGE);
    sprite.fillRect(160, 13, 16, 3, grays[6]);
    sprite.fillRect(219, 13, 16, 3, grays[6]);
    sprite.fillRect(160, 194, 74, 1, ORANGE);

    //frame top and bot
    sprite.drawRect(0,0,239,239,light);
    sprite.fillRect(5,234,230,2,grays[13]);

    sprite.setTextColor(grays[1],gray);
    sprite.setTextSize(2);
    sprite.drawString("STATIONS",31,2);
    sprite.drawString("WEB",179,2);
    sprite.setTextSize(1);

    //station list
    int visible_stations = 9;
    int start_idx = chosen - 4;
    if (start_idx < 0) start_idx = 0;
    if (ns > visible_stations && start_idx > ns - visible_stations) {
        start_idx = ns - visible_stations;
    }

    sprite.setTextSize(2); // Large Size 2 (16x16 pixels)
    for(int i = 0; i < visible_stations; i++)
    {
        int station_idx = start_idx + i;
        if (station_idx >= ns) break; // In case ns < visible_stations

        if(station_idx == chosen) {
            sprite.setTextColor(TFT_GREEN,TFT_BLACK);
            String name = station_list[station_idx].name;
            if (name.length() > 11) {
                int numChars = name.length();
                int totalSteps = numChars + 4;
                int step = (millis() / 350) % totalSteps;
                String scrollText = name + "    " + name;
                sprite.drawString(scrollText.substring(step, step + 11), 10, 23 + (i * 19));
            } else {
                sprite.drawString(name, 10, 23 + (i * 19));
            }
        } else {
            sprite.setTextColor(TFT_DARKGREEN,TFT_BLACK);
            sprite.drawString(station_list[station_idx].name.substring(0,11), 10, 23 + (i * 19));
        }
    }

    sprite.setTextSize(1);
    sprite.setTextDatum(1); // Center align
    sprite.setTextColor(grays[0],gray);
    sprite.drawString("INTERNET",197,88); 
    
    sprite.setTextSize(2); // Large Size 2 for RADIO
    sprite.setTextColor(TFT_RED,gray);
    sprite.drawString("RADIO",197,102); 
    sprite.setTextDatum(0); // Reset to Top-Left
    sprite.setTextSize(1); // Reset to Size 1

    sprite.setTextColor(grays[6],gray);
    sprite.drawString("NOW PLAYING",6,200); 
    sprite.drawString("VOLUME",160,124);
    sprite.setTextColor(TFT_GREEN,TFT_BLACK); 
    sprite.drawString("BITRATE "+String(bitrate),164,180); 

    // Calculate remaining sleep time
    unsigned long idleTime = millis() - lastInteraction;
    unsigned long stoppedTime = millis() - lastAudioTime;
    unsigned long remainingMs = 0;
    if (audio.isRunning()) {
        if (IDLE_SLEEP_TIMEOUT > idleTime) {
            remainingMs = IDLE_SLEEP_TIMEOUT - idleTime;
        }
    } else {
        unsigned long remIdle = (IDLE_SLEEP_TIMEOUT > idleTime) ? (IDLE_SLEEP_TIMEOUT - idleTime) : 0;
        unsigned long remStop = (STOPPED_SLEEP_TIMEOUT > stoppedTime) ? (STOPPED_SLEEP_TIMEOUT - stoppedTime) : 0;
        remainingMs = (remIdle < remStop) ? remIdle : remStop;
    }
    unsigned long remainingMins = (remainingMs + 59999) / 60000;
    char sleepBuf[30];
    if (remainingMs == 0) {
        sprintf(sleepBuf, "Sleep: 0m");
    } else {
        sprintf(sleepBuf, "Sleep: %lum", remainingMins);
    }

    sprite.setTextColor(grays[11], gray);
    sprite.setTextDatum(2); // Top-Right aligned
    sprite.drawString(sleepBuf, 234, 200); 
    sprite.setTextDatum(0); // Reset to Top-Left 

    //graph
    for(int i=0;i<13;i++){  
        if(!connected || !audio.isRunning())
            g[i]=0;
        for(int j=0;j<g[i];j++)
            sprite.fillRect(165+(i*5),71-j*4,4,3,grays[4]);
    }

    sprite.setTextColor(grays[16],grays[5]);
    //buttons (size 1 GLCD font centered inside container)
    sprite.setTextSize(1);
    sprite.setTextDatum(4); // Center-Middle
    for(int i=0;i<3;i++)
    {
      int x = 160 + (i * 26);
      int y = 152;
      // 3D Drop Shadow
      sprite.fillRoundRect(x + 1, y + 1, 22, 18, 4, grays[13]);
      // Button Body
      sprite.fillRoundRect(x, y, 22, 18, 4, grays[5]);
      // Top-Left Highlight
      sprite.drawFastHLine(x + 3, y, 16, grays[1]);
      sprite.drawFastVLine(x, y + 3, 12, grays[1]);
      // Bottom-Right Shadow Bevel
      sprite.drawFastHLine(x + 3, y + 17, 16, grays[10]);
      sprite.drawFastVLine(x + 21, y + 3, 12, grays[10]);
      
      sprite.drawString(letters[i], x + 9, y + 9); 
    }
    sprite.setTextDatum(0); // Reset
    sprite.setTextSize(1); // Reset

    uint16_t *buf = (uint16_t*)sprite.getBuffer();
    int total = 240 * 240;

    for (int i = 0; i < total; i++) {
        buf[i] = __builtin_bswap16(buf[i]);
    }

    gfx->draw16bitRGBBitmap(0, 0, buf, 240, 240);

    canDraw=0;
    draw3();
}




void draw3()
{
     songposition--;
     if(songposition<-220) songposition=220;
     sprite2.setFont(&fonts::Font0);
     sprite2.setTextSize(1);
     sprite2.fillSprite(TFT_BLACK);  
     sprite2.drawString(songPlaying,songposition,5);  

    uint16_t *buf2 = (uint16_t*)sprite2.getBuffer();
    int total2 = 230 * 16;

   for (int i = 0; i < total2; i++) {
    buf2[i] = __builtin_bswap16(buf2[i]);
   }

   gfx->draw16bitRGBBitmap(5, 213, buf2, 230, 16);
}

void measureBatt()
{
    uint16_t mv = analogReadMilliVolts(1);  // mV na ADC pinu
    float vbat = (mv / 1000.0) * 3.0;             // stvarni napon baterije

    voltage = vbat;
    char vol_buffer[8];
    sprintf(vol_buffer, "%.2f", voltage);

    // izračun postotka baterije (Li-ion)
    float minV = 3.0;
    float maxV = 4.2;

    float pct = (vbat - minV) / (maxV - minV);
    pct = constrain(pct, 0.0, 1.0);

    batLevel = pct * 13.0;
}

void loop() {



  //measure signal strength
  static unsigned long lastRSSI = 0;
  static unsigned long lastSlide = 0;
  static unsigned long lastStationChangeTime = 0;
  static bool stationChangePending = false;

if (millis() - lastRSSI > 1000) {   // every 1 second
    lastRSSI = millis();
    rssi = WiFi.RSSI();  // očitaj jačinu signala
    measureBatt();
    canDraw=1;

    if (WiFi.status() == WL_CONNECTED)
    {connected=true;}
    else
    {connected=false;
    songPlaying="WIFI NOT CONNECTED";}
}

if (millis() - lastSlide > 30) {   // svakih 1 sekundu
    lastSlide = millis();
    if (!isScreensaver) draw3();
}

static unsigned long lastScrollUpdate = 0;
if (!isScreensaver && station_list[chosen].name.length() > 11 && (millis() - lastScrollUpdate > 350)) {
    lastScrollUpdate = millis();
    canDraw = 1;
}

static unsigned long lastEqUpdate = 0;
if (!isScreensaver && audio.isRunning() && (millis() - lastEqUpdate > 500)) { // 2 FPS
    lastEqUpdate = millis();
    for (int i = 0; i < 13; i++) {
        g[i] = random(1, 5);
    }
    canDraw = 1;
}


  static unsigned long btn5PressTime = 0;
  static bool btn5PressedInScreensaver = false;
  if (digitalRead(5) == LOW) {
      if(deb==0) {
          deb=1;
          btn5PressTime = millis();
          btn5PressedInScreensaver = isScreensaver;
      }
      lastInteraction = millis();
  } else {
      if (deb==1) {
          deb=0;
          if (btn5PressedInScreensaver) {
              // Just dismiss screensaver, do not change station
              isScreensaver = false;
              canDraw = 1;
          } else {
              unsigned long pressDuration = millis() - btn5PressTime;
              if (pressDuration > 500) {
                  chosen--;
                  if(chosen<0) chosen=ns-1;
              } else {
                  chosen++;
                  if(chosen>=ns) chosen=0;
              }
              songPlaying = "Loading...";
              lastStationChangeTime = millis();
              stationChangePending = true;
              canDraw = 1;
          }
      }
  }


  static unsigned long btn4PressTime = 0;
  static bool btn4PressedInScreensaver = false;
  if (digitalRead(4) == LOW) {
      if(deb2==0) {
          deb2=1;
          btn4PressTime = millis();
          btn4PressedInScreensaver = isScreensaver;
      }
      lastInteraction = millis();
  } else {
      if (deb2==1) {
          deb2=0;
          if (btn4PressedInScreensaver) {
              // Just dismiss screensaver, do not change volume
              isScreensaver = false;
              canDraw = 1;
          } else {
              unsigned long pressDuration = millis() - btn4PressTime;
              if (pressDuration > 500) { // Long press: decrease volume
                  volume--;
                  if (volume < 1) volume = 10;
              } else { // Short press: increase volume
                  volume++;
                  if (volume > 10) volume = 1;
              }
              audio.setVolume(volume * 2);
              canDraw = 1;
          }
      }
  }

    if (stationChangePending && (millis() - lastStationChangeTime > 2000)) {
        stationChangePending = false;
        audio.connecttohost(station_list[chosen].url.c_str());
        
        // Save last played station
        preferences.begin("radio", false);
        preferences.putString("last_station", station_list[chosen].name);
        preferences.end();
        
        canDraw = 1;
    }
    
    if (digitalRead(0) == LOW) {
    lastInteraction = millis();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // probudi se kad gumb opet bude LOW
    digitalWrite(PA_CTRL, LOW);
    delay(200);
    esp_deep_sleep_start();
  }

  if (audio.isRunning()) {
      lastAudioTime = millis();
  }

  // Sleep and Dimming Logic
  unsigned long idleTime = millis() - lastInteraction;
  unsigned long stoppedTime = millis() - lastAudioTime;
  
  if (idleTime > IDLE_SLEEP_TIMEOUT || stoppedTime > STOPPED_SLEEP_TIMEOUT) {
      esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Wake up on left button
      digitalWrite(PA_CTRL, LOW); // Turn off audio amp
      analogWrite(GFX_BL, 0); // Turn off screen
      delay(200);
      esp_deep_sleep_start();
  } else if (idleTime > SCREENSAVER_TIMEOUT) {
      isScreensaver = true;
      analogWrite(GFX_BL, SCREEN_BRIGHTNESS_DIM);
  } else if (idleTime > SCREEN_DIM_TIMEOUT) {
      isScreensaver = false;
      analogWrite(GFX_BL, SCREEN_BRIGHTNESS_DIM);
  } else {
      isScreensaver = false;
      analogWrite(GFX_BL, SCREEN_BRIGHTNESS_NORMAL);
  }

  vTaskDelay(1);
  audio.loop();

   if (canDraw) {
       if (isScreensaver) {
           drawScreensaver();
       } else {
           draw2();
       }
   }

}

// optional
void audio_info(const char *info) {
  Serial.print("info        ");
  Serial.println(info);
}
void audio_id3data(const char *info) {  //id3 metadata
  Serial.print("id3data     ");
  Serial.println(info);
}

void audio_showstation(const char *info) {
  Serial.print("station     ");
  curStation=info;
  canDraw=true;
}
void audio_showstreamtitle(const char *info) {
  Serial.print("streamtitle ");
  Serial.println(info);
  songPlaying=info;
  canDraw=1;
}
void audio_bitrate(const char *info) {
  Serial.print("bitrate     ");
  Serial.println(info);
  bitrate=(String(info).toInt()/1000);
  
}
