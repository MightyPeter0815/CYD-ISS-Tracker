#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // Download this library https://github.com/bblanchon/ArduinoJson
#include <TFT_eSPI.h> // Download this library https://github.com/Bodmer/TFT_espi
#include <Sgp4.h> // Download this library https://github.com/sparkfun/SparkFun_SGP4_Arduino_Library
#include <time.h>
#include <math.h>

#include "map.h"  // include these two files in the same folder with arduino code
#include "tangoFont.h"

#define CUSTOM_FONT tangoFont

const char* WIFI_SSID = "xxxxx";                        // Enter your wifi name
const char* WIFI_PASS = "xxxxxxxx";                     // Enter your wifi password
const char* N2YO_API_KEY = "xxxxxxxxxxxxxxxxxxxxxxxxx"; // Enter the N2YO API key

const char* TLE_URL =
  "https://api.n2yo.com/rest/v1/satellite/tle/25544?apiKey=";
  
#define TIMEZONE_OFFSET_HOURS    6 // Enter your timezone hour
#define TIMEZONE_OFFSET_MINUTES  0 // Enter your timezone minute

TFT_eSPI tft = TFT_eSPI();

inline uint16_t panelBg()     { return tft.color565(16, 26, 40); }
inline uint16_t panelHilite() { return tft.color565(55, 75, 105); }
inline uint16_t panelBorder() { return tft.color565(85, 105, 135); }

#define BRIGHTNESS_PIN 21

#define SCREEN_W 320
#define SCREEN_H 240

#define HEADER_Y 0
#define HEADER_H 40

#define INFO_Y   (HEADER_Y + HEADER_H)
#define INFO_H   40

#define MAP_W 320
#define MAP_H 160
#define MAP_X 0
#define MAP_Y (SCREEN_H - MAP_H)  
#define MARKER_RADIUS 3 

const unsigned long TLE_INTERVAL   = 3600000UL; // 1 hour (remember rate limit is 1000/hour)
const unsigned long TLE_RETRY      = 300000UL;  // 5 min (after a failed fetch)
const unsigned long POS_INTERVAL   = 1000UL;    // 1 second

unsigned long lastTLEUpdate = 0;
unsigned long lastTLEAttempt = 0;
unsigned long lastPosition  = 0;
unsigned long lastNtpAttempt = 0;

Sgp4 iss;

char tleLine1[130];
char tleLine2[130];

bool tleValid  = false;
bool timeValid = false;

double issLat = 0.0;
double issLon = 0.0;
double issAlt = 0.0;

int previousX = -1;
int previousY = -1;

bool havePreviousMarker = false;


double previousTrailLat = 0.0;
double previousTrailLon = 0.0;

bool havePreviousTrail = false;

#define TRAIL_COLOR TFT_YELLOW
#define TRAIL_WIDTH 2   // px, thickness of the trail dots/line

uint8_t trailBitmap[(MAP_W * MAP_H + 7) / 8];

inline void trailBitSet(int x, int y)
{
  int idx = y * MAP_W + x;
  trailBitmap[idx >> 3] |= (1 << (idx & 7));
}

inline bool trailBitGet(int x, int y)
{
  int idx = y * MAP_W + x;
  return trailBitmap[idx >> 3] & (1 << (idx & 7));
}

inline void trailBitmapClear()
{
  memset(trailBitmap, 0, sizeof(trailBitmap));
}

void trailPlot(int cx, int cy)
{
  int half = TRAIL_WIDTH / 2;
  for (int dy = -half; dy < TRAIL_WIDTH - half; dy++)
  {
    int py = cy + dy;
    if (py < 0 || py >= MAP_H)
      continue;
    for (int dx = -half; dx < TRAIL_WIDTH - half; dx++)
    {
      int px = cx + dx;
      if (px < 0 || px >= MAP_W)
        continue;
      trailBitSet(px, py);
      tft.drawPixel(
        MAP_X + px,
        MAP_Y + py,
        TRAIL_COLOR
      );
    }
  }
}

int lastDay = -1;

TFT_eSprite latSpr  = TFT_eSprite(&tft);
TFT_eSprite lonSpr  = TFT_eSprite(&tft);
TFT_eSprite altSpr  = TFT_eSprite(&tft);
TFT_eSprite timeSpr = TFT_eSprite(&tft);

int latLabelX, lonLabelX, altLabelX;
int latSprX, lonSprX, altSprX;
int infoRowY;
int timeSprX, timeSprY;
int valH;
int latLonValW, altValW, timeValW;

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("==============================");
  Serial.println("       ESP32 ISS TRACKER");
  Serial.println("==============================");
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  pinMode(BRIGHTNESS_PIN, OUTPUT);
  digitalWrite(BRIGHTNESS_PIN, HIGH);
  tft.fillScreen(TFT_BLACK);
  tft.loadFont(CUSTOM_FONT);
  drawMap();
  trailBitmapClear();
  drawPanel();
  latLabelX = 6;
  latSprX   = 42;
  lonLabelX = 117;
  lonSprX   = 152;
  altLabelX = 227;
  altSprX   = 262;

  tft.loadFont(CUSTOM_FONT);
  drawStaticUI();
  tft.unloadFont();

  infoRowY = INFO_Y + (INFO_H - valH) / 2;

  latSpr.setColorDepth(16);
  latSpr.loadFont(CUSTOM_FONT);
  latSpr.createSprite(latLonValW, valH);

  lonSpr.setColorDepth(16);
  lonSpr.loadFont(CUSTOM_FONT);
  lonSpr.createSprite(latLonValW, valH);

  altSpr.setColorDepth(16);
  altSpr.loadFont(CUSTOM_FONT);
  altSpr.createSprite(altValW, valH);

  timeSpr.setColorDepth(16);
  timeSpr.loadFont(CUSTOM_FONT);
  timeSpr.createSprite(timeValW, valH);

  latSpr.setTextColor(TFT_GREEN, panelBg());
  lonSpr.setTextColor(TFT_GREEN, panelBg());
  altSpr.setTextColor(TFT_GREEN, panelBg());
  timeSpr.setTextColor(TFT_WHITE, panelBg());

  timeSprX = SCREEN_W - timeSpr.width() - 10;
  timeSprY = (HEADER_H - valH) / 2;

  Serial.println("[WIFI] Connecting...");
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if (millis() - wifiStart > 20000)
      break;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("[WIFI] Connected");
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("[WIFI] FAILED");
  }
  Serial.println("[NTP] Starting...");
  tryNTP();
  lastNtpAttempt = millis();

  if (!timeValid)
    Serial.println("[NTP] Not synchronized yet - continuing");
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("[TLE] Downloading...");
    lastTLEAttempt = millis();
    if (downloadTLE())
    {
      tleValid = true;
      lastTLEUpdate = millis();
      Serial.println("[TLE] SUCCESS");
    }
    else
    {
      Serial.println("[TLE] FAILED - will retry in 5 min");
    }
  }
  drawHeader();
  drawInfo();
  Serial.println("[SYSTEM] Ready");
}

void tryNTP()
{
  if (WiFi.status() != WL_CONNECTED)
    return;
  Serial.println("[NTP] Sync attempt...");
  configTime(
    0,
    0,
    "time.google.com",
    "time.cloudflare.com",
    "pool.ntp.org"
  );
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000))
  {
    if (timeinfo.tm_year >= (2020 - 1900))
    {
      timeValid = true;
      lastDay = timeinfo.tm_mday;
      Serial.printf(
        "[NTP] Synchronized UTC: %04d-%02d-%02d %02d:%02d:%02d\n",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      );
      drawHeader();
      return;
    }
  }

  timeValid = false;
  Serial.println("[NTP] No valid response");
}

void loop()
{
  unsigned long now = millis();
  if (!timeValid && now - lastNtpAttempt >= 30000UL)
  {
    lastNtpAttempt = now;
    tryNTP();
  }
  bool tleDue = tleValid
    ? (now - lastTLEUpdate  >= TLE_INTERVAL)   // normal hourly refresh
    : (now - lastTLEAttempt >= TLE_RETRY);      // shorter backoff after a failure

  if (tleDue)
  {
    lastTLEAttempt = now;
    if (downloadTLE())
    {
      tleValid = true;
      lastTLEUpdate = now;
    }
    else
    {
      Serial.println("[TLE] Update failed - will retry in 5 min");
    }
  }
  if (tleValid &&
      timeValid &&
      now - lastPosition >= POS_INTERVAL)
  {
    lastPosition = now;
    calculateISS();
    checkNewDay();
    addTrailPoint();
    updateISSMarker();
    drawHeader();
    drawInfo();
  }
}

void drawMap()
{
  tft.pushImage(
    MAP_X,
    MAP_Y,
    MAP_W,
    MAP_H,
    worldMap
  );
}
void drawPanel()
{
  tft.fillRect(
    0,
    0,
    SCREEN_W,
    INFO_Y + INFO_H,
    panelBg()
  );
  tft.drawFastHLine(0, 0, SCREEN_W, panelHilite());
  tft.drawFastHLine(0, 1, SCREEN_W, panelHilite());
  tft.drawFastHLine(0, HEADER_H, SCREEN_W, panelBorder());
  tft.drawFastHLine(
    0,
    INFO_Y + INFO_H - 1,
    SCREEN_W,
    panelBorder()
  );
}

bool downloadTLE()
{
  if (WiFi.status() != WL_CONNECTED)
    return false;
  String url = String(TLE_URL) + N2YO_API_KEY;
  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    Serial.printf("[TLE] HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }
  String data = http.getString();
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data);
  if (err)
  {
    Serial.printf("[TLE] JSON parse failed: %s\n", err.c_str());
    return false;
  }
  const char* tleField = doc["tle"];
  if (tleField == nullptr)
  {
    Serial.println("[TLE] No 'tle' field in response");
    return false;
  }
  String tleText = String(tleField);
  int splitAt = tleText.indexOf("\r\n");
  int splitLen = 2;
  if (splitAt < 0)
  {
    splitAt = tleText.indexOf('\n');
    splitLen = 1;
  }
  if (splitAt < 0)
    return false;
  String line1 = tleText.substring(0, splitAt);
  String line2 = tleText.substring(splitAt + splitLen);

  line1.trim();
  line2.trim();

  if (!line1.startsWith("1 "))
    return false;
  if (!line2.startsWith("2 "))
    return false;
  line1.toCharArray(
    tleLine1,
    sizeof(tleLine1)
  );
  line2.toCharArray(
    tleLine2,
    sizeof(tleLine2)
  );
  char name[] = "ISS";
  if (!iss.init(
        name,
        tleLine1,
        tleLine2
      ))
  {
    return false;
  }
  return true;
}
void calculateISS()
{
  time_t now;
  time(&now);
  double jd =
    ((double)now / 86400.0)
    + 2440587.5;
  iss.findsat(jd);
  issLat = iss.satLat;
  issLon = iss.satLon;
  issAlt = iss.satAlt;

  Serial.print("[ISS] ");
  Serial.print(issLat, 3);
  Serial.print(" / ");
  Serial.print(issLon, 3);
  Serial.print(" / ");
  Serial.print(issAlt, 1);
  Serial.println(" km");
}

int longitudeToX(double longitude)
{
  int x =
    (int)(
      (longitude + 180.0)
      * MAP_W
      / 360.0
    );
  return constrain(
    x,
    0,
    MAP_W - 1
  );
}

int latitudeToY(double latitude)
{
  int y =
    (int)(
      (90.0 - latitude)
      * MAP_H
      / 180.0
    );
  return constrain(
    y,
    0,
    MAP_H - 1
  );
}

void drawTrailLine(int x0, int y0, int x1, int y1)
{
  int dx = abs(x1 - x0);
  int sx = (x0 < x1) ? 1 : -1;

  int dy = -abs(y1 - y0);
  int sy = (y0 < y1) ? 1 : -1;

  int err = dx + dy;

  while (true)
  {
    trailPlot(x0, y0);

    if (x0 == x1 && y0 == y1)
      break;

    int e2 = 2 * err;

    if (e2 >= dy)
    {
      err += dy;
      x0 += sx;
    }

    if (e2 <= dx)
    {
      err += dx;
      y0 += sy;
    }
  }
}

void clearTrail()
{
  drawMap();
  trailBitmapClear();
}

void addTrailPoint()
{
  int x = longitudeToX(issLon);
  int y = latitudeToY(issLat);

  if (!havePreviousTrail)
  {
    previousTrailLat = issLat;
    previousTrailLon = issLon;
    havePreviousTrail = true;
    trailPlot(x, y);
    return;
  }

  int oldX = longitudeToX(previousTrailLon);
  int oldY = latitudeToY(previousTrailLat);
  if (fabs(issLon - previousTrailLon) < 180.0)
  {
    drawTrailLine(oldX, oldY, x, y);
  }
  else
  {
    clearTrail();
    trailPlot(x, y);
  }
  previousTrailLat = issLat;
  previousTrailLon = issLon;
}

void updateISSMarker()
{
  int x = longitudeToX(issLon);
  int y = latitudeToY(issLat);

  
  if (havePreviousMarker)
  {
    for (int dy = -MARKER_RADIUS; dy <= MARKER_RADIUS; dy++)
    {
      int py = previousY + dy;

      if (py < 0 || py >= MAP_H)
        continue;
      for (int dx = -MARKER_RADIUS; dx <= MARKER_RADIUS; dx++)
      {
        int px = previousX + dx;
        if (px < 0 || px >= MAP_W)
          continue;
        if (trailBitGet(px, py))
        {
          tft.drawPixel(
            MAP_X + px,
            MAP_Y + py,
            TRAIL_COLOR
          );
        }
        else
        {
          uint16_t pixel;
          memcpy_P(
            &pixel,
            &worldMap[py * MAP_W + px],
            sizeof(pixel)
          );

          tft.pushImage(
            MAP_X + px,
            MAP_Y + py,
            1,
            1,
            &pixel
          );
        }
      }
    }
  }

  tft.fillCircle(
    MAP_X + x,
    MAP_Y + y,
    MARKER_RADIUS,
    TFT_RED
  );
  previousX = x;
  previousY = y;
  havePreviousMarker = true;
}

void checkNewDay()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10))
    return;
  if (lastDay == -1)
  {
    lastDay = timeinfo.tm_mday;
    return;
  }
  if (timeinfo.tm_mday != lastDay)
  {
    Serial.println("[TRAIL] New UTC day");
    drawMap();
    trailBitmapClear();
    havePreviousTrail = false;
    havePreviousMarker = false;
    lastDay = timeinfo.tm_mday;
  }
}

void drawStaticUI()
{
  valH       = tft.fontHeight() + 4;
  latLonValW = tft.textWidth("-180.00") + 4;
  altValW    = tft.textWidth("99999 km") + 4;
  timeValW   = tft.textWidth("88:88:88") + 4;
  tft.loadFont(CUSTOM_FONT);
  tft.setTextColor(TFT_YELLOW, panelBg());
  int titleH = tft.fontHeight();
  tft.setCursor(6, (HEADER_H - titleH) / 2);
  tft.print("Real-Time ISS Tracker");
  tft.setTextColor(TFT_WHITE, panelBg());
  int labelH = tft.fontHeight();
  int labelY = INFO_Y + (INFO_H - labelH) / 2;
  tft.setCursor(latLabelX, labelY);
  tft.print("LAT :");
  tft.setCursor(lonLabelX, labelY);
  tft.print("LON :");
  tft.setCursor(altLabelX, labelY);
  tft.print("ALT :");
}

void drawHeader()
{
  if (!timeValid)
    return;
  time_t utc = time(nullptr);
  utc += (TIMEZONE_OFFSET_HOURS * 3600L)
       + (TIMEZONE_OFFSET_MINUTES * 60L);
  struct tm localTime;
  gmtime_r(
    &utc,
    &localTime
  );
  char clockText[12];
  sprintf(
    clockText,
    "%02d:%02d:%02d",
    localTime.tm_hour,
    localTime.tm_min,
    localTime.tm_sec
  );
  timeSpr.fillSprite(panelBg());
  timeSpr.drawString(clockText, 2, 2);
  timeSpr.pushSprite(timeSprX, timeSprY);
}
void drawInfo()
{
  char buf[16];
  dtostrf(issLat, 0, 2, buf);
  latSpr.fillSprite(panelBg());
  latSpr.drawString(buf, 2, 2);
  latSpr.pushSprite(latSprX, infoRowY);

  dtostrf(issLon, 0, 2, buf);
  lonSpr.fillSprite(panelBg());
  lonSpr.drawString(buf, 2, 2);
  lonSpr.pushSprite(lonSprX, infoRowY);

  sprintf(buf, "%.0f km", issAlt);
  altSpr.fillSprite(panelBg());
  altSpr.drawString(buf, 2, 2);
  altSpr.pushSprite(altSprX, infoRowY);
}
