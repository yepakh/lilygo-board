#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"

const char* ssid = WIFI_SSID;
const char* pass = WIFI_PASS;
const char* station_id = STOP_ID;
const char* platform_filter = PLATFORM;
const char* time_zone = TIME_ZONE;

const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const unsigned int dep_limit = 8;
const int BUTTON_PIN = 21;

struct Departure {
  String line_name;
  String direction;
  String platform;
  String scheduled_time;
  String real_time;
  String state;
};

struct StationInfo {
  String name;
  String place;
  Departure departure[dep_limit];
  unsigned int count = 0;
};

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.begin(ssid, pass);
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
  String json = requestStationInfo();
  StationInfo info = parse_station_info(json);
  print_station_info(info);
}

void loop() {
  static bool last_pressed = false;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;

  if (pressed && !last_pressed) {
    String json = requestStationInfo();
    StationInfo info = parse_station_info(json);
    print_station_info(info);
  }

  last_pressed = pressed;
  delay(20);  // crude debounce
}

String requestStationInfo() {
  HTTPClient http;

  char uri[256];  // or bigger if needed
  snprintf(uri, sizeof(uri), "https://webapi.vvo-online.de/dm?stopid=%s&limit=%u", station_id, dep_limit);
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

  info.name = String(doc["Name"]);
  info.place = String(doc["Place"]);

  for (JsonObject departure : doc["Departures"].as<JsonArray>()) {
    if (info.count >= dep_limit) {
      break;
    }

    const char* platform_name = departure["Platform"]["Name"];
    if (platform_filter && platform_name && strcmp(platform_filter, platform_name) != 0) {
      continue;
    }

    Departure dep;

    dep.platform = String(platform_name ? platform_name : "");
    dep.line_name = String(departure["LineName"]);
    dep.direction = String(departure["Direction"]);
    dep.real_time = String(departure["RealTime"]);
    dep.scheduled_time = String(departure["ScheduledTime"]);
    dep.state = String(departure["State"]);

    info.departure[info.count++] = dep;
  }

  return info;
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
    Serial.printf("\tDeparture Scheduled time: %s\n", convert_ms_timestamp(dep.scheduled_time).c_str());
    Serial.printf("\tDeparture Real time: %s\n\n", convert_ms_timestamp(dep.real_time).c_str());
  }
}

String convert_ms_timestamp(const String& ms_timestamp) {
  int start_ind = ms_timestamp.indexOf('(');
  int end_ind = ms_timestamp.indexOf('-');

  if (start_ind < 0 || end_ind < 0 || end_ind <= start_ind + 1) {
    return "";
  }

  String ms_str = ms_timestamp.substring(start_ind + 1, end_ind);
  const char* c = ms_str.c_str();
  long long ms = strtoll(c, nullptr, 10);
  time_t seconds = ms / 1000;

  tm* time = localtime(&seconds);
  if (!time) return "";

  char time_buf[64];
  strftime(time_buf, sizeof(time_buf), "%b %d, %a %R", time);
  return String(time_buf);
}
