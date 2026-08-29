#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_adc/adc_oneshot.h>

#include "display_bsp.h"
#include "src/app_bsp/lvgl_bsp.h"
#include "wifi_config.h"

// campus-rain：南校未来 2 小时降雨桌面屏
// 数据源：rain.yurikale.top 的 IoT 精简 JSON（约 1.5KB），走 jsDelivr CDN

LV_FONT_DECLARE(lv_font_cn_16);
LV_FONT_DECLARE(lv_font_clock_64);

#define PRIMARY_HOST "cdn.jsdelivr.net"
#define RAIN_PORT 443

struct FetchTarget {
  const char *host;
  const char *path;
};

static const FetchTarget kFetchTargets[] = {
    {"cdn.jsdelivr.net", "/gh/HydroGest/campus-rain@main/data/iot/sysu-south.json"},
    {"fastly.jsdelivr.net", "/gh/HydroGest/campus-rain@main/data/iot/sysu-south.json"},
    {"gcore.jsdelivr.net", "/gh/HydroGest/campus-rain@main/data/iot/sysu-south.json"},
    {"rain.yurikale.top", "/data/iot/sysu-south.json"},
};

#define SCREEN_W 400
#define SCREEN_H 300
#define REFRESH_MS (15UL * 60UL * 1000UL)
#define FETCH_TIMEOUT_MS 120000UL
#define STALL_TIMEOUT_MS 30000UL
#define RAIN_THRESHOLD 0.02f

static DisplayPort RlcdPort(12, 11, 5, 40, 41, SCREEN_W, SCREEN_H);
static WiFiClientSecure rainClient;

struct RainData {
  bool valid = false;
  time_t expiresAt = 0;
  bool rainNow = false;
  int rainStartsInMin = -1;
  int rainEndsInMin = -1;
  int altStartsInMin = -1;
  int rainMinutes2h = 0;
  int probPct = 0;
  float temp = NAN;
  int humidity = -1;
  float nearestKm = NAN;
  char trend[8] = "";
  char nextRainSource[8] = "";
  long long nextRainAtMs = 0;
  bool hasNextRain = false;
  char nextRainAt[8] = "";
  bool hasPrecip10 = false;
  float precip10[12];
  float minutes[120];
  bool hourlyRain[24] = {false};
  int hourlyCount = 0;
};

static RainData rain;
static lv_obj_t *clockDigits[2] = {NULL, NULL};
static lv_obj_t *dateLabel = NULL;
static lv_obj_t *batteryLabel = NULL;
static char statusText[64] = "BOOT";
static unsigned long lastFetchMs = 0;
static bool uiDirty = true;
static int lastSecond = -1;
static unsigned long lastBootPressMs = 0;
static unsigned long lastBatteryMs = 0;
static uint8_t batteryPercent = 0;
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t cali_handle;

// 板载电池 ADC：换算成 0-100% 电量（非天气类新功能）
static void Adc_PortInit() {
  adc_cali_curve_fitting_config_t cali_config = {};
  cali_config.unit_id = ADC_UNIT_1;
  cali_config.atten = ADC_ATTEN_DB_12;
  cali_config.bitwidth = ADC_BITWIDTH_12;
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));

  adc_oneshot_unit_init_cfg_t init_config1 = {};
  init_config1.unit_id = ADC_UNIT_1;
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
  adc_oneshot_chan_cfg_t config = {};
  config.bitwidth = ADC_BITWIDTH_12;
  config.atten = ADC_ATTEN_DB_12;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config));
}

static float Adc_GetBatteryVoltage(int *data) {
  int value = 0;
  int tage = 0;
  float vol = 0;
  esp_err_t err = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &value);
  if (err == ESP_OK) {
    adc_cali_raw_to_voltage(cali_handle, value, &tage);
    vol = 0.001f * tage * 3;
  }
  if (data) {
    *data = value;
  }
  return vol;
}

static uint8_t Adc_GetBatteryLevel() {
  float vol = Adc_GetBatteryVoltage(NULL);
  if (vol < 3.0f) return 0;
  if (vol > 4.12f) return 100;
  return (uint8_t)(((vol - 3.0f) / 1.12f) * 100);
}

static void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("[wifi] event: connected");
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[wifi] event: disconnected reason=%d\n", info.wifi_sta_disconnected.reason);
  }
}

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
  uint16_t *buffer = (uint16_t *)color_map;
  for (int y = area->y1; y <= area->y2; y++) {
    for (int x = area->x1; x <= area->x2; x++) {
      uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
      RlcdPort.RLCD_SetPixel(x, y, color);
      buffer++;
    }
  }
  RlcdPort.RLCD_Display();
  lv_disp_flush_ready(drv);
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, int x, int y, lv_align_t align) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_color(label, lv_color_black(), 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_align(label, align, x, y);
  return label;
}

static void setLabelText(lv_obj_t *label, const char *fmt, ...) {
  static char buf[80];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  lv_label_set_text(label, buf);
}

static void setStatus(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(statusText, sizeof(statusText), fmt, args);
  va_end(args);
  uiDirty = true;
}

static bool connectWiFi() {
  // 校园网 WPA2 企业认证（PEAP/MSCHAPv2），账号密码见 wifi_config.h
  setStatus("WIFI: SYSU-SECURE");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, 500);
  delay(200);

  wl_status_t started = WiFi.begin(
    WIFI_SSID, WPA2_AUTH_PEAP, WIFI_IDENTITY, WIFI_USERNAME, WIFI_PASSWORD,
    NULL, NULL, NULL, -1, 0, NULL, true);
  Serial.printf("[wifi] begin status=%d\n", (int)started);
  if (started == WL_CONNECT_FAILED) {
    setStatus("WIFI: CONNECT FAIL");
    return false;
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000UL) {
    delay(500);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[wifi] not connected status=%d\n", (int)WiFi.status());
    int n = WiFi.scanNetworks();
    Serial.printf("[wifi] scan found %d networks\n", n);
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.indexOf("SYSU") >= 0 || ssid.indexOf("SYS") >= 0) {
        Serial.printf("[wifi] nearby: %s rssi=%d auth=%d\n", ssid.c_str(), WiFi.RSSI(i), (int)WiFi.encryptionType(i));
      }
    }
    setStatus("WIFI: AUTH FAIL");
    return false;
  }
  Serial.printf("[wifi] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  IPAddress hostIp;
  if (WiFi.hostByName(PRIMARY_HOST, hostIp)) {
    Serial.printf("[dns] %s -> %s\n", PRIMARY_HOST, hostIp.toString().c_str());
  } else {
    Serial.printf("[dns] %s resolve failed\n", PRIMARY_HOST);
  }
  return true;
}

static bool syncTime() {
  setStatus("SYNC TIME ...");
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
  unsigned long start = millis();
  while (time(nullptr) < 1600000000L && millis() - start < 20000UL) {
    delay(200);
  }
  Serial.printf("[time] epoch=%ld\n", (long)time(nullptr));
  return time(nullptr) >= 1600000000L;
}

static int headerContentLength(const char *headers) {
  const char *p = headers;
  while (p && *p) {
    const char *colon = strchr(p, ':');
    if (!colon) break;
    const char *nl = strstr(colon, "\r\n");
    if (!nl) break;
    size_t nameLen = (size_t)(colon - p);
    if (nameLen == 14) {
      char name[15];
      memcpy(name, p, nameLen);
      name[nameLen] = 0;
      for (size_t i = 0; i < nameLen; i++) {
        name[i] = (char)tolower((unsigned char)name[i]);
      }
      if (strcmp(name, "content-length") == 0) {
        return atoi(colon + 1);
      }
    }
    p = nl + 2;
  }
  return -1;
}

static bool headerHasChunked(const char *headers) {
  const char *p = headers;
  while (p && *p) {
    const char *colon = strchr(p, ':');
    if (!colon) break;
    const char *nl = strstr(colon, "\r\n");
    if (!nl) break;
    size_t nameLen = (size_t)(colon - p);
    if (nameLen == 17) {
      char name[18];
      memcpy(name, p, nameLen);
      name[nameLen] = 0;
      for (size_t i = 0; i < nameLen; i++) {
        name[i] = (char)tolower((unsigned char)name[i]);
      }
      if (strcmp(name, "transfer-encoding") == 0) {
        return strstr(colon + 1, "chunked") != NULL;
      }
    }
    p = nl + 2;
  }
  return false;
}

static bool httpFetch(const char *host, const char *path, char *body, size_t cap, size_t &bodyLen) {
  // 依次尝试多个 CDN 镜像，兼容 Content-Length 和 chunked 两种响应
  Serial.printf("[rain] connecting %s:%d ...\n", host, RAIN_PORT);
  rainClient.setInsecure();
  rainClient.setTimeout(10);
  if (!rainClient.connect(host, RAIN_PORT)) {
    static char errBuf[128];
    int err = rainClient.lastError(errBuf, sizeof(errBuf));
    Serial.printf("[rain] connect failed err=%d (%s)\n", err, errBuf);
    return false;
  }
  Serial.println("[rain] tls connected");
  rainClient.print("GET ");
  rainClient.print(path);
  rainClient.print(" HTTP/1.1\r\nHost: ");
  rainClient.print(host);
  rainClient.print("\r\nUser-Agent: ESP32-S3-RLCD-RAIN\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");

  static char headerBuf[4096];
  size_t headerLen = 0;
  unsigned long start = millis();
  while (headerLen + 4 < sizeof(headerBuf) && millis() - start < FETCH_TIMEOUT_MS) {
    if (rainClient.available()) {
      headerBuf[headerLen++] = (char)rainClient.read();
      if (headerLen >= 4 && memcmp(headerBuf + headerLen - 4, "\r\n\r\n", 4) == 0) {
        break;
      }
    } else {
      delay(5);
    }
  }
  headerBuf[headerLen] = 0;

  int contentLength = headerContentLength(headerBuf);
  Serial.printf("[rain] headerLen=%d first=%.60s contentLength=%d\n", headerLen, headerBuf, contentLength);
  if (contentLength <= 0 || (size_t)contentLength + 1 > cap) {
    Serial.println("[rain] no content-length, reading until close");
    size_t got = 0;
    unsigned long lastDataMs = millis();
    start = millis();
    if (headerHasChunked(headerBuf)) {
      Serial.println("[rain] chunked transfer");
      while (got + 1 < cap && millis() - start < FETCH_TIMEOUT_MS && millis() - lastDataMs < STALL_TIMEOUT_MS) {
        String sizeLine = rainClient.readStringUntil('\n');
        sizeLine.trim();
        long chunkSize = strtol(sizeLine.c_str(), NULL, 16);
        if (chunkSize <= 0) break;
        size_t want = (size_t)chunkSize;
        if (want > cap - got - 1) want = cap - got - 1;
        size_t have = 0;
        while (have < want && millis() - lastDataMs < STALL_TIMEOUT_MS) {
          if (rainClient.available()) {
            int r = rainClient.read((uint8_t *)body + got + have, want - have);
            if (r > 0) {
              have += (size_t)r;
              lastDataMs = millis();
            }
          } else {
            delay(5);
          }
        }
        got += have;
        if (rainClient.available() >= 2) {
          rainClient.read();
          rainClient.read();
        } else {
          delay(5);
        }
        if (have < want) break;
      }
    } else {
      while (got + 1 < cap && millis() - start < FETCH_TIMEOUT_MS && millis() - lastDataMs < STALL_TIMEOUT_MS) {
        if (rainClient.available()) {
          size_t want = cap - got - 1;
          if (want > 512) want = 512;
          int r = rainClient.read((uint8_t *)body + got, want);
          if (r > 0) {
            got += (size_t)r;
            lastDataMs = millis();
          }
        } else if (!rainClient.connected()) {
          break;
        } else {
          delay(5);
        }
      }
    }
    body[got] = 0;
    bodyLen = got;
    rainClient.stop();
    Serial.printf("[rain] body got=%d\n", (int)got);
    return got > 0;
  }

  size_t got = 0;
  unsigned long lastDataMs = millis();
  start = millis();
  while (got < (size_t)contentLength && millis() - start < FETCH_TIMEOUT_MS && millis() - lastDataMs < STALL_TIMEOUT_MS) {
    if (rainClient.available()) {
      size_t want = (size_t)contentLength - got;
      if (want > 4096) want = 4096;
      int r = rainClient.read((uint8_t *)body + got, want);
      if (r > 0) {
        got += (size_t)r;
        lastDataMs = millis();
      }
    } else {
      delay(5);
    }
  }
  body[got] = 0;
  bodyLen = got;
  rainClient.stop();
  Serial.printf("[rain] body got=%d\n", (int)got);
  return got == (size_t)contentLength;
}

static bool parseRain(const char *body) {
  // 解析精简 IoT JSON：now（温度/降雨/概率/下一场雨）+ hourly24（逐小时）
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    return false;
  }

  JsonObject now = doc["now"];
  if (now.isNull()) {
    return false;
  }

  RainData d;
  d.temp = now["temp"].as<float>();
  d.humidity = now["humidity"].is<int>() ? now["humidity"].as<int>() : -1;
  d.nearestKm = now["nearestKm"].as<float>();
  d.rainNow = now["rainNow"].as<bool>();
  d.rainStartsInMin = now["rainStartsInMin"].is<int>() ? now["rainStartsInMin"].as<int>() : -1;
  d.rainEndsInMin = now["rainEndsInMin"].is<int>() ? now["rainEndsInMin"].as<int>() : -1;
  d.altStartsInMin = now["startsInAlt"].is<int>() ? now["startsInAlt"].as<int>() : -1;
  d.probPct = now["probPct"].as<int>();
  d.rainMinutes2h = now["rainMinutes2h"].as<int>();
  const char *trend = now["trend"] | "";
  strncpy(d.trend, trend, sizeof(d.trend) - 1);
  d.trend[sizeof(d.trend) - 1] = 0;
  const char *src = now["nextRainSource"] | "";
  strncpy(d.nextRainSource, src, sizeof(d.nextRainSource) - 1);
  d.nextRainSource[sizeof(d.nextRainSource) - 1] = 0;
  d.nextRainAtMs = now["nextRainAt"].as<long long>();
  long long expiresMs = now["expiresAt"].as<long long>();
  if (expiresMs > 0) {
    d.expiresAt = (time_t)(expiresMs / 1000);
  }

  JsonArray p10 = now["precipitation10"];
  if (!p10.isNull()) {
    d.hasPrecip10 = true;
    int idx = 0;
    for (JsonVariant v : p10) {
      if (idx >= 12) break;
      d.precip10[idx++] = v.as<float>();
    }
    for (int i = 0; i < 12; i++) {
      if (d.precip10[i] < 0) d.precip10[i] = 0;
    }
  }

  for (int i = 0; i < 120; i++) {
    d.minutes[i] = 0;
  }
  if (d.hasPrecip10) {
    for (int i = 0; i < 120; i++) {
      d.minutes[i] = d.precip10[i / 10];
    }
  } else if (d.rainStartsInMin >= 0 || d.rainNow) {
    int s = d.rainStartsInMin >= 0 ? d.rainStartsInMin : 0;
    int e = d.rainEndsInMin >= 0 ? d.rainEndsInMin : s;
    for (int i = s; i <= e && i < 120; i++) {
      d.minutes[i] = 1.0f;
    }
  }

  JsonArray hourly = doc["hourly24"];
  for (JsonVariant h : hourly) {
    int hh = h["h"].as<int>();
    if (hh >= 0 && hh < 24) {
      d.hourlyRain[hh] = h["rain"].as<bool>();
      d.hourlyCount++;
    }
  }

  long long nowMsLL = (long long)time(nullptr) * 1000;
  if (d.nextRainAtMs > nowMsLL) {
    time_t t = (time_t)(d.nextRainAtMs / 1000);
    struct tm nextTm;
    if (localtime_r(&t, &nextTm)) {
      snprintf(d.nextRainAt, sizeof(d.nextRainAt), "%02d:%02d", nextTm.tm_hour, nextTm.tm_min);
      d.hasNextRain = true;
    }
  }
  if (!d.hasNextRain) {
    time_t nowT2 = time(nullptr);
    struct tm tm2;
    int curHour = 0;
    if (localtime_r(&nowT2, &tm2)) {
      curHour = tm2.tm_hour;
    }
    for (int hh = curHour + 1; hh < 24; hh++) {
      if (d.hourlyRain[hh]) {
        snprintf(d.nextRainAt, sizeof(d.nextRainAt), "%02d:00", hh);
        d.hasNextRain = true;
        break;
      }
    }
    if (!d.hasNextRain) {
      for (int hh = 0; hh <= curHour; hh++) {
        if (d.hourlyRain[hh]) {
          snprintf(d.nextRainAt, sizeof(d.nextRainAt), "%02d:00", hh);
          d.hasNextRain = true;
          break;
        }
      }
    }
  }

  rain = d;
  return true;
}

static void buildUI() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_white(), 0);

  lv_obj_t *title = makeLabel(screen, &lv_font_cn_16, 16, 6, LV_ALIGN_TOP_LEFT);
  lv_label_set_text(title, "南校 RAIN");

  lv_obj_t *statusTop = makeLabel(screen, &lv_font_montserrat_14, -16, 8, LV_ALIGN_TOP_RIGHT);
  lv_label_set_text(statusTop, statusText);

  // Win10 风格时钟磁贴：HH 和 MM 各一个大黑块，去掉冒号
  const int tileH = 96;
  const int tileW = 130;
  const int tileGap = 24;
  const int totalW = 2 * tileW + tileGap;
  int tileX = (SCREEN_W - totalW) / 2;
  for (int i = 0; i < 2; i++) {
    lv_obj_t *tile = lv_obj_create(screen);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, tileW, tileH);
    lv_obj_set_pos(tile, tileX, 28);
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_t *lbl = lv_label_create(tile);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_clock_64, 0);
    lv_obj_center(lbl);
    lv_label_set_text(lbl, "--");
    clockDigits[i] = lbl;
    tileX += tileW + tileGap;
  }

  dateLabel = makeLabel(screen, &lv_font_montserrat_14, 0, 132, LV_ALIGN_TOP_MID);
  lv_label_set_text(dateLabel, "");

  lv_obj_t *verdict = makeLabel(screen, &lv_font_montserrat_28, 16, 150, LV_ALIGN_TOP_LEFT);
  lv_obj_t *prob = makeLabel(screen, &lv_font_montserrat_20, -16, 156, LV_ALIGN_TOP_RIGHT);
  lv_obj_t *sub = makeLabel(screen, &lv_font_cn_16, 16, 182, LV_ALIGN_TOP_LEFT);

  time_t nowT = time(nullptr);
  bool nowcastExpired = !rain.valid || (rain.expiresAt > 0 && nowT >= rain.expiresAt);

  if (!rain.valid) {
    lv_label_set_text(verdict, "NO DATA");
    lv_label_set_text(prob, "");
    lv_label_set_text(sub, "数据已过期，等待自动刷新");
  } else if (nowcastExpired) {
    lv_label_set_text(verdict, "NO RADAR");
    lv_label_set_text(prob, "");
    if (rain.altStartsInMin >= 0 && rain.altStartsInMin < 120) {
      setLabelText(sub, "MODEL: RAIN IN %d MIN", rain.altStartsInMin);
    } else if (rain.hasNextRain) {
      if (strcmp(rain.nextRainSource, "radar") == 0) {
        setLabelText(sub, "下一场雨 %s", rain.nextRainAt);
      } else {
        setLabelText(sub, "下一场雨约 %s (HOURLY)", rain.nextRainAt);
      }
    } else {
      lv_label_set_text(sub, "数据已过期，等待自动刷新");
    }
  } else if (rain.rainNow) {
    lv_label_set_text(verdict, "RAINING NOW");
    setLabelText(prob, "P %d%%  %d MIN", rain.probPct, rain.rainMinutes2h);
    if (rain.rainEndsInMin >= 0) {
      setLabelText(sub, "正在下雨，约 %d 分钟后停", rain.rainEndsInMin);
    } else {
      lv_label_set_text(sub, "正在下雨，未来两小时持续有雨");
    }
  } else if (rain.rainStartsInMin >= 0) {
    int duration = (rain.rainEndsInMin >= rain.rainStartsInMin)
                       ? rain.rainEndsInMin - rain.rainStartsInMin + 1
                       : rain.rainMinutes2h;
    setLabelText(verdict, "RAIN IN %d MIN", rain.rainStartsInMin);
    setLabelText(prob, "P %d%%  %d MIN", rain.probPct, rain.rainMinutes2h);
    setLabelText(sub, "预计 %d 分钟后开始，持续约 %d 分钟", rain.rainStartsInMin, duration);
  } else if (rain.altStartsInMin >= 0 && rain.altStartsInMin < 120) {
    setLabelText(verdict, "MODEL RAIN");
    setLabelText(prob, "P %d%%", rain.probPct);
    setLabelText(sub, "MODEL: RAIN IN %d MIN (UMBRELLA)", rain.altStartsInMin);
  } else {
    lv_label_set_text(verdict, "DRY FOR 2H");
    setLabelText(prob, "P %d%%", rain.probPct);
    if (rain.hasNextRain) {
      if (strcmp(rain.nextRainSource, "radar") == 0) {
        setLabelText(sub, "未来两小时无雨，下一场雨 %s", rain.nextRainAt);
      } else {
        setLabelText(sub, "未来两小时无雨，下一场雨约 %s (HOURLY)", rain.nextRainAt);
      }
    } else {
      lv_label_set_text(sub, "未来两小时无雨，放心出门");
    }
  }

  {
    // 未来 2 小时降雨柱状图：每根柱子代表 10 分钟
    int chartBottom = 266;
    int barAreaH = 72;
    const int barCount = 12;
    const int barW = 26;
    const int step = 31;

    float maxI = 0;
    for (int i = 0; i < 12; i++) {
      if (rain.minutes[i * 10] > maxI) maxI = rain.minutes[i * 10];
    }
    if (maxI <= 0) maxI = 1;

    for (int i = 0; i < barCount; i++) {
      float v = rain.minutes[i * 10];
      bool rainy = v >= RAIN_THRESHOLD;
      int h = rainy ? (int)(v / maxI * barAreaH) : 6;
      if (h < 6) h = rainy ? 8 : 6;
      lv_obj_t *bar = lv_obj_create(screen);
      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, barW, h);
      lv_obj_set_pos(bar, 16 + i * step, chartBottom - h);
      lv_obj_set_style_bg_color(bar, rainy ? lv_color_black() : lv_color_white(), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(bar, 1, 0);
      lv_obj_set_style_border_color(bar, lv_color_black(), 0);
    }

    lv_obj_t *nowLbl = makeLabel(screen, &lv_font_montserrat_14, 16, chartBottom + 6, LV_ALIGN_TOP_LEFT);
    lv_label_set_text(nowLbl, "NOW");
    lv_obj_t *midLbl = makeLabel(screen, &lv_font_montserrat_14, 16 + 5 * step, chartBottom + 6, LV_ALIGN_TOP_LEFT);
    lv_label_set_text(midLbl, "+1H");
    lv_obj_t *endLbl = makeLabel(screen, &lv_font_montserrat_14, -16, chartBottom + 6, LV_ALIGN_TOP_RIGHT);
    lv_label_set_text(endLbl, "+2H");
  }

  lv_obj_t *bottom = makeLabel(screen, &lv_font_montserrat_14, 16, 286, LV_ALIGN_TOP_LEFT);
  if (!isnan(rain.temp)) {
    const char *trendText = "FLAT";
    if (strcmp(rain.trend, "up") == 0) trendText = "UP";
    else if (strcmp(rain.trend, "down") == 0) trendText = "DOWN";
    if (!isnan(rain.nearestKm) && rain.nearestKm > 0) {
      setLabelText(bottom, "TEMP %.1fC  HUM %d%%  %s  %.0fKM", rain.temp, rain.humidity, trendText, rain.nearestKm);
    } else {
      setLabelText(bottom, "TEMP %.1fC  HUM %d%%  %s", rain.temp, rain.humidity, trendText);
    }
  } else {
    lv_label_set_text(bottom, "RAIN.YURIKALE.TOP");
  }
  batteryLabel = makeLabel(screen, &lv_font_montserrat_14, -16, 286, LV_ALIGN_TOP_RIGHT);
  setLabelText(batteryLabel, "BAT %d%%", batteryPercent);
}

static void refreshUI() {
  if (Lvgl_lock(-1)) {
    buildUI();
    Lvgl_unlock();
  }
  uiDirty = false;
}

static void refreshRain() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi()) {
      setStatus("WIFI FAIL, RETRY 60S");
      refreshUI();
      lastFetchMs = millis();
      return;
    }
  }
  if (!syncTime()) {
    setStatus("TIME FAIL");
  }
  setStatus("FETCH RAIN ...");

  char *body = (char *)heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
  if (!body) {
    setStatus("NO PSRAM MEMORY");
    refreshUI();
    lastFetchMs = millis();
    return;
  }

  size_t bodyLen = 0;
  bool fetched = false;
  bool parsed = false;
  for (const FetchTarget &target : kFetchTargets) {
    bodyLen = 0;
    fetched = httpFetch(target.host, target.path, body, 64 * 1024, bodyLen);
    parsed = fetched && parseRain(body);
    Serial.printf("[rain] %s fetched=%d parsed=%d bytes=%d\n", target.host, (int)fetched, (int)parsed, (int)bodyLen);
    if (fetched && parsed) {
      break;
    }
  }
  if (fetched && parsed) {
    rain.valid = true;
    time_t nowT = time(nullptr);
    struct tm nowTm;
    if (localtime_r(&nowT, &nowTm)) {
      setStatus("OK, UPD %02d:%02d", nowTm.tm_hour, nowTm.tm_min);
    } else {
      setStatus("OK");
    }
  } else {
    setStatus("FETCH FAIL");
  }

  time_t nowDbg = time(nullptr);
  bool expiredDbg = rain.valid && rain.expiresAt > 0 && nowDbg >= rain.expiresAt;
  Serial.printf("[ui] valid=%d expired=%d startMin=%d endMin=%d altMin=%d prob=%d%% nextRain=%s(%s) temp=%.1f hum=%d km=%.0f\n",
                (int)rain.valid, (int)expiredDbg, rain.rainStartsInMin, rain.rainEndsInMin,
                rain.altStartsInMin, rain.probPct, rain.nextRainAt, rain.nextRainSource,
                rain.temp, rain.humidity, rain.nearestKm);

  heap_caps_free(body);
  lastFetchMs = millis();
  refreshUI();
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("[campus-rain] boot");
  WiFi.onEvent(onWiFiEvent);
  pinMode(0, INPUT_PULLUP);
  Adc_PortInit();

  RlcdPort.RLCD_Init();
  Lvgl_PortInit(SCREEN_W, SCREEN_H, Lvgl_FlushCallback);

  setStatus("WIFI CONNECTING ...");
  refreshUI();
  refreshRain();
}

void loop() {
  struct tm now;
  time_t t = time(nullptr);
  if (t >= 1600000000L && localtime_r(&t, &now)) {
    if (now.tm_sec != lastSecond) {
      lastSecond = now.tm_sec;
      if (Lvgl_lock(-1)) {
        // 每秒刷新时钟磁贴数字
        if (clockDigits[0]) setLabelText(clockDigits[0], "%02d", now.tm_hour);
        if (clockDigits[1]) setLabelText(clockDigits[1], "%02d", now.tm_min);
        if (dateLabel) {
          static const char *wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
          setLabelText(dateLabel, "%02d/%02d %s", now.tm_mon + 1, now.tm_mday, wd[now.tm_wday]);
        }
        Lvgl_unlock();
      }
    }
  }

  if (millis() - lastBatteryMs > 10000UL) {
    lastBatteryMs = millis();
    batteryPercent = Adc_GetBatteryLevel();
    if (batteryLabel && Lvgl_lock(-1)) {
      setLabelText(batteryLabel, "BAT %d%%", batteryPercent);
      Lvgl_unlock();
    }
  }

  if (uiDirty) {
    refreshUI();
  }

  if (digitalRead(0) == LOW && millis() - lastBootPressMs > 2000UL) {
    lastBootPressMs = millis();
    setStatus("MANUAL REFRESH");
    refreshUI();
    refreshRain();
  }

  time_t nowT = time(nullptr);
  bool dataStale = !rain.valid || (rain.expiresAt > 0 && nowT >= rain.expiresAt);
  unsigned long refreshInterval = (WiFi.status() == WL_CONNECTED)
                                      ? (dataStale ? 300000UL : REFRESH_MS)
                                      : 60000UL;
  if (millis() - lastFetchMs > refreshInterval) {
    refreshRain();
  }

  delay(100);
}
