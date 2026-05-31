#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"
#include "epd_driver.h"
#include "firasans.h"
#include "esp_adc_cal.h"

const char* ssid = WIFI_SSID;
const char* pass = WIFI_PASS;
const char* time_zone = TIME_ZONE;

const char* station_id_1 = STOP_ID_1;
const char* platform_1 = PLATFORM_1;
const char* station_id_2 = STOP_ID_2;
const char* platform_2 = PLATFORM_2;

const char* ntp_server_1 = "pool.ntp.org";
const char* ntp_server_2 = "time.nist.gov";
const unsigned int dep_request_limit = 8;
const unsigned int display_limit = 4;
const int BUTTON_PIN = 21;
int vref = 1100;

uint8_t* framebuffer;
time_t last_press_time = 0;
const time_t TIMEOUT = 20;

enum Mode {
  MODE_1 = 0,
  MODE_2 = 1,
  MODE_COUNT
};

struct Departure {
  String line_name;
  String direction;
  String platform;
  time_t scheduled_time;
  time_t real_time;
  String state;
};

struct StationInfo {
  String name;
  String place;
  Departure departure[dep_request_limit];
  unsigned int count = 0;
};

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.printf("Before framebuffer - Heap free: %u, PSRAM free: %u\n",
                heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  epd_init();

  framebuffer = (uint8_t*)heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM);
  if (!framebuffer) {
    for (;;) {
      Serial.println("Failed to allocate framebuffer!");
      delay(100);
    };  // halt
  }
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  log_memory("After frame buffer - ");

  WiFi.begin(ssid, pass);

  log_memory("After Wifi begin - ");

  unsigned long start = millis();
  const unsigned long timeout = 10000;

  Serial.print("Connecting to WiFi\n");

  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
    Serial.print(".");
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to the WiFi network");
    Serial.print("Local ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else if (WiFi.status() == WL_NO_SSID_AVAIL) {
    Serial.println("\nNetwork not found");
  } else {
    Serial.println("\nFailed to connect");
  }

  configTzTime(time_zone, ntp_server_1, ntp_server_2);
  delay(500);

  // Correct the ADC reference voltage
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
    ADC_UNIT_2,
    ADC_ATTEN_DB_12,
    ADC_WIDTH_BIT_12,
    1100,
    &adc_chars);

  if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    Serial.printf("eFuse Vref: %umV\r\n", adc_chars.vref);
    vref = adc_chars.vref;
  }

  processButtonPress(true, station_id_1, platform_1);
}

void loop() {
  static bool last_pressed_state = false;
  static Mode active_mode = MODE_1;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;

  if (pressed && !last_pressed_state) {
    time_t now;
    time(&now);
    Serial.printf("Current time: %u\nLast press time: %u\n", now, last_press_time);
    bool mode_change = now < (last_press_time + TIMEOUT);
    if (mode_change) {
      active_mode = switch_mode(active_mode);
    }

    switch (active_mode) {
      case MODE_1:
        processButtonPress(mode_change, station_id_1, platform_1);
        break;
      case MODE_2:
        processButtonPress(mode_change, station_id_2, platform_2);
        break;
      default:
        break;
    }

    last_press_time = now;
    log_memory("Button pressed!\n");
  }

  last_pressed_state = pressed;
  delay(20);  // crude debounce
}

Mode switch_mode(Mode current) {
  int next = (current + 1) % MODE_COUNT;
  return (Mode)next;
}

void processButtonPress(bool renderStation, const char* station_id, const char* platform_filter) {
  Serial.println("processButtonPress start");

  if (!framebuffer) {
    Serial.println("framebuffer is NULL!");
    return;
  }

  epd_poweron();

  if (renderStation) {
    epd_clear();
  } else {
    Rect_t area = { 0, 50, EPD_WIDTH, EPD_HEIGHT - 50 };
    epd_clear_area(area);
  }

  String json = requestStationInfo(station_id);
  StationInfo info = parse_station_info(json, platform_filter);
  Serial.println("Request sent");
  print_station_info(info);

  int32_t cursor_x;
  int32_t cursor_y;
  if (renderStation) {
    cursor_x = 10;
    cursor_y = 40;
    String station_title = get_station_title(info);
    Serial.println(station_title);
    write_string((GFXfont*)&FiraSans, station_title.c_str(), &cursor_x, &cursor_y, NULL);
  }

  cursor_x = 10;
  cursor_y = EPD_HEIGHT - 10;
  String curr_time = get_current_time();
  Serial.println(curr_time);
  write_string((GFXfont*)&FiraSans, curr_time.c_str(), &cursor_x, &cursor_y, NULL);

  cursor_x = EPD_WIDTH - 100;
  cursor_y = EPD_HEIGHT - 10;
  String bat_pct = get_battery_pct();
  Serial.printf("Battery pct: %s\n", bat_pct.c_str());
  write_string((GFXfont*)&FiraSans, bat_pct.c_str(), &cursor_x, &cursor_y, NULL);

  cursor_x = 50;
  cursor_y = 100;
  String departs = get_departures(info);
  Serial.println(departs);
  write_string((GFXfont*)&FiraSans, departs.c_str(), &cursor_x, &cursor_y, NULL);

  epd_poweroff_all();
  Serial.println("processButtonPress end");
}

String get_battery_pct() {
  const float max_vol = 4.2;
  const float min_vol = 3.3;

  uint16_t v = analogRead(BATT_PIN);
  float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
  Serial.printf("Battery voltage: %.2f\n", battery_voltage);

  int pct = (battery_voltage - min_vol) * 100 / (max_vol - min_vol);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return String(pct) + "%";
}

String requestStationInfo(const char* station_id) {
  HTTPClient http;

  char uri[256];  // or bigger if needed
  snprintf(uri, sizeof(uri), "https://webapi.vvo-online.de/dm?stopid=%s&limit=%u", station_id, dep_request_limit);
  http.begin(uri);
  int httpCode = http.GET();

  String payload;
  if (httpCode == HTTP_CODE_OK) {
    payload = http.getString();
  }

  http.end();
  return payload;
}

int intlen(unsigned int n) {
  if (n == 0) return 1;
  int len = 0;
  while (n > 0) {
    len++;
    n /= 10;
  }

  return len;
}

StationInfo parse_station_info(String input, const char* platform_filter) {
  StationInfo info;

  StaticJsonDocument<6144> doc;
  DeserializationError error = deserializeJson(doc, input);

  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return StationInfo{};
  }

  info.name = doc["Name"].as<String>();
  info.place = doc["Place"].as<String>();

  for (JsonObject departure : doc["Departures"].as<JsonArray>()) {
    if (info.count >= dep_request_limit) {
      break;
    }

    const char* platform_name = departure["Platform"]["Name"];
    if (platform_filter && platform_filter[0] != '\0' && platform_name && strcmp(platform_filter, platform_name) != 0) {
      continue;
    }

    Departure dep;

    dep.platform = String(platform_name ? platform_name : "");
    dep.line_name = departure["LineName"].as<String>();
    dep.direction = departure["Direction"].as<String>();
    dep.real_time = convert_ms_timestamp(departure["RealTime"]);
    dep.scheduled_time = convert_ms_timestamp(departure["ScheduledTime"]);
    dep.state = departure["State"].as<String>();

    info.departure[info.count++] = dep;
  }

  return info;
}

String get_station_title(const StationInfo& info) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s - %s", info.place.c_str(), info.name.c_str());
  return buf;
}

String get_departures(const StationInfo& info) {
  // const char* buf = (char*)malloc(1024);
  char buf[1024];
  unsigned int bytes_written = 0;

  for (int i = 0; i < info.count && i < display_limit; i++) {
    bytes_written += snprintf(buf + bytes_written, sizeof(buf) - bytes_written, "%u) %s\n", i + 1, get_departure_info(info.departure[i]).c_str());
  }

  return buf;
}

String get_departure_info(const Departure& dep) {
  String diffStr;
  int diff = (dep.real_time - dep.scheduled_time) / 60;
  if (dep.real_time != 0 && diff != 0) {
    diffStr = String(diff);

    if (diff > 0) {
      diffStr = '+' + diffStr + '\'';
    }
  }

  char buf[256];
  if (diffStr.length() == 0) {
    snprintf(buf, sizeof(buf), "%s - %s: %s", dep.line_name.c_str(), dep.direction.c_str(), get_short_time_string(dep.scheduled_time).c_str());
  } else {
    snprintf(buf, sizeof(buf), "%s - %s: %s(%s)", dep.line_name.c_str(), dep.direction.c_str(), get_short_time_string(dep.scheduled_time).c_str(), diffStr.c_str());
  }
  return buf;
}

void print_station_info(const StationInfo& info) {
  Serial.printf("Station Place: %s\n", info.place.c_str());
  Serial.printf("Station Name: %s\n", info.name.c_str());
  Serial.print("Departures\n");

  for (int i = 0; i < info.count; i++) {
    const Departure& dep = info.departure[i];

    Serial.printf("\tDeparture Line: %s\n", dep.line_name.c_str());
    Serial.printf("\tDeparture Direction: %s\n", dep.direction.c_str());
    Serial.printf("\tDeparture Platform: %s\n", dep.platform.c_str());
    Serial.printf("\tDeparture State: %s\n", dep.state.c_str());
    Serial.printf("\tDeparture Scheduled time: %s\n", get_time_string(dep.scheduled_time).c_str());
    Serial.printf("\tDeparture Real time: %s\n\n", get_time_string(dep.real_time).c_str());
  }
}

time_t convert_ms_timestamp(String ms_timestamp) {
  int start = ms_timestamp.indexOf('(');
  int end = ms_timestamp.indexOf('-');

  if (start < 0 || end < 0 || end <= start + 1) {
    return 0;
  }

  String ms_str = ms_timestamp.substring(start + 1, end);
  const char* c = ms_str.c_str();
  long long ms = strtoll(c, nullptr, 10);
  return ms / 1000;
}

String get_time_string(const time_t& seconds) {
  tm* time = localtime(&seconds);
  if (!time) return "";

  char time_buf[64];
  strftime(time_buf, sizeof(time_buf), "%b %d, %a %R", time);
  return time_buf;
}

String get_short_time_string(const time_t& seconds) {
  tm* time = localtime(&seconds);
  if (!time) return "";

  char time_buf[8];
  strftime(time_buf, sizeof(time_buf), "%R", time);
  return time_buf;
}


String get_current_time() {
  time_t now;
  time(&now);
  return get_time_string(now);
}

void log_memory(String pref) {
  Serial.printf("%sHeap free: %u, PSRAM free: %u\n",
                pref.c_str(),
                heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
