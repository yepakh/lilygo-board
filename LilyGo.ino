#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"
#include "epd_driver.h"
#include "firasans.h"

const char* ssid = WIFI_SSID;
const char* pass = WIFI_PASS;
const char* station_id = STOP_ID;
const char* platform_filter = PLATFORM;
const char* time_zone = TIME_ZONE;

const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const unsigned int dep_request_limit = 8;
const unsigned int display_limit = 4;
const int BUTTON_PIN = 21;

uint8_t* framebuffer;

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

  configTzTime(time_zone, ntpServer1, ntpServer2);
  epd_poweroff_all();
}

void loop() {
  static bool last_pressed = false;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;

  if (pressed && !last_pressed) {
    processButtonPress();
    log_memory("Button pressed!\n");
  }

  last_pressed = pressed;
  delay(20);  // crude debounce
}

void processButtonPress() {
  Serial.println("processButtonPress start");

  if (!framebuffer) {
    Serial.println("framebuffer is NULL!");
    return;
  }

  epd_poweron();
  epd_clear();

  String json = requestStationInfo();
  StationInfo info = parse_station_info(json);
  Serial.println("Request sent");
  print_station_info(info);

  int32_t cursor_x = 10;
  int32_t cursor_y = 40;
  String station_title = get_station_title(info);
  Serial.println(station_title);
  write_string((GFXfont*)&FiraSans, station_title.c_str(), &cursor_x, &cursor_y, NULL);

  cursor_x = 10;
  cursor_y = 80;
  String curr_time = get_current_time();
  Serial.println(curr_time);
  write_string((GFXfont*)&FiraSans, curr_time.c_str(), &cursor_x, &cursor_y, NULL);

  cursor_x = 100;
  cursor_y = 200;
  String departs = get_departures(info);
  Serial.println(departs);
  write_string((GFXfont*)&FiraSans, departs.c_str(), &cursor_x, &cursor_y, NULL);

  epd_poweroff_all();
  Serial.println("processButtonPress end");
}

String requestStationInfo() {
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

StationInfo parse_station_info(String input) {
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
    if (platform_filter && platform_name && strcmp(platform_filter, platform_name) != 0) {
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
  String diffStr = "+0'";
  int diff = (dep.real_time - dep.scheduled_time) / 60;
  if (dep.real_time == 0) {
    diffStr = "?";
  } else if (diff == 0) {
    String diffStr = String(diff);

    if (diff > 0) {
      diffStr = '+' + diffStr + '\'';
    }
  }

  char buf[256];
  int bytesWritten = snprintf(buf, sizeof(buf), "%s - %s: %s(%s)", dep.line_name.c_str(), dep.direction.c_str(), get_short_time_string(dep.scheduled_time).c_str(), diffStr.c_str());
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
