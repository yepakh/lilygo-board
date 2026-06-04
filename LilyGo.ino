#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"
#include "epd_driver.h"
#include "firasans.h"
#include "esp_adc_cal.h"
#include <Preferences.h>

// #define DEBUG_VOLTAGE

const char* ssid = WIFI_SSID;
const char* pass = WIFI_PASS;
const char* time_zone = TIME_ZONE;

const char* station_id_1 = STOP_ID_1;
const char* platform_1 = PLATFORM_1;
const char* station_id_2 = STOP_ID_2;
const char* platform_2 = PLATFORM_2;

const char* ntp_server_1 = "pool.ntp.org";
const char* ntp_server_2 = "time.nist.gov";
const int button_pin = 21;
const int boot_pin = 0;
const unsigned int departures_per_request = 8;
const unsigned int max_request_count = 4;
const unsigned int min_display_items = 6;
const unsigned int max_display_items = 6;

int vref = 1100;
Preferences prefs;

enum Mode {
  MODE_0 = 0,
  MODE_1 = 1,
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
  Departure departures[max_display_items];
  unsigned int count = 0;
  time_t last_ts = 0;
};

typedef enum {
  TURN_ON,
  TURN_OFF,
  CLEAR,
  PRINT_TEXT,
} UI_Action;

void TaskPrintOnScreen(void* pvParameters);
TaskHandle_t display_task_handle;
TaskHandle_t app_task_handle;
QueueHandle_t QueueHandle;
typedef struct {
  UI_Action Action;
  char Text[512];
  int32_t Cursor_X;
  int32_t Cursor_Y;
} message_t;

void setup() {
  Serial.begin(115200);
  Serial.printf("setup core: %d\n", xPortGetCoreID());
  QueueHandle = xQueueCreate(10, sizeof(message_t));

  // Check if the queue was successfully created
  if (QueueHandle == NULL) {
    Serial.println("Queue could not be created. Halt.");
    while (1) {
      delay(1000);  // Halt at this point as is not possible to continue
    }
  }

  app_task_handle = xTaskGetCurrentTaskHandle();
  // This variant of task creation can also specify on which core it will be run (only relevant for multi-core ESPs)
  xTaskCreatePinnedToCore(
    TaskPrintOnScreen,
    "Priting the data to display",
    1024 * 8,              // Stack size
    NULL,                  // When no parameter is used, simply pass NULL
    2,                     // Priority
    &display_task_handle,  // With task handle we will be able to manipulate with this task.
    0                      // Core on which the task will run
  );

  pinMode(button_pin, INPUT_PULLUP);

  prefs.begin("app", false);
  epd_init();
  correct_refv();

  Mode mode = (Mode)prefs.getInt("mode", 0);

  uint64_t status = esp_sleep_get_ext1_wakeup_status();
  Serial.printf("Pin wakeup status: %llu\n", status);
  if (status & _BV(button_pin)) {
    mode = switch_mode((Mode)mode);
    prefs.putInt("mode", (int)mode);
  }

  Serial.printf("Processing mode: %i\n", mode);
  switch (mode) {
    case MODE_0:
      process_station_info(station_id_1, platform_1);
      break;
    case MODE_1:
      process_station_info(station_id_2, platform_2);
      break;
  }

  prefs.end();
  esp_sleep_enable_ext1_wakeup(_BV(boot_pin) | _BV(button_pin), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

void TaskPrintOnScreen(void* pvParamters) {
  Serial.printf("display task core: %d\n", xPortGetCoreID());
  message_t message;
  int32_t cursor_x = 0;
  int32_t cursor_y = 0;

  for (;;) {
    if (xQueueReceive(QueueHandle, &message, portMAX_DELAY) == pdTRUE) {
      switch (message.Action) {
        case TURN_ON:
          Serial.println("Handling Screen ON Request.");
          epd_poweron();
          epd_clear();
          break;
        case TURN_OFF:
          Serial.println("Handling Screen Off Request.");
          epd_poweroff_all();
          xTaskNotifyGive(app_task_handle);
          break;
        case PRINT_TEXT:
          Serial.println("Handling Screen Print Request.");
          if (message.Cursor_X >= 0) cursor_x = message.Cursor_X;
          if (message.Cursor_Y >= 0) cursor_y = message.Cursor_Y;
          write_string((GFXfont*)&FiraSans, message.Text, &cursor_x, &cursor_y, NULL);
          break;
      }
    }
  }
}

void correct_refv() {
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
}

void wifi_on() {
  bool is_fast_connect = wifi_begin();

  unsigned long start = millis();
  unsigned long timeout = 3000;

  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
    Serial.print(".");
    delay(50);
  }

  if (WiFi.status() != WL_CONNECTED && is_fast_connect) {
    is_fast_connect = false;
    WiFi.disconnect();
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.begin(ssid, pass);
    Serial.print("Fast connect failed, reconnecting\n");
    start = millis();
    timeout = 10000;

    while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
      Serial.print(".");
      delay(100);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to the WiFi network");
    Serial.print("Local ESP32 IP: ");
    Serial.println(WiFi.localIP());

    if (!is_fast_connect) {
      prefs.putString("bssid", WiFi.BSSIDstr());
      prefs.putInt("channel", WiFi.channel());
      prefs.putString("ip", WiFi.localIP().toString());
      prefs.putString("gateway", WiFi.gatewayIP().toString());
      prefs.putString("subnet", WiFi.subnetMask().toString());
    }

    unsigned long start = millis();
    unsigned long timeout = 5000;
    configTzTime(time_zone, ntp_server_1, ntp_server_2);
    time_t now = 0;
    while (now < 1000000000 && millis() - start < timeout) {
      time(&now);
      delay(50);
    }
  } else if (WiFi.status() == WL_NO_SSID_AVAIL) {
    Serial.println("\nNetwork not found");
  } else {
    Serial.println("\nFailed to connect");
  }
}

bool wifi_begin() {
  String bssid_str = prefs.getString("bssid", "");
  if (bssid_str.length() == 0) {
    Serial.print("Connecting to WiFi\n");
    WiFi.begin(ssid, pass);
    return false;
  }
  // parse "AA:BB:CC:DD:EE:FF" into bytes
  uint8_t bssid_bytes[6];
  sscanf(bssid_str.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &bssid_bytes[0], &bssid_bytes[1], &bssid_bytes[2],
         &bssid_bytes[3], &bssid_bytes[4], &bssid_bytes[5]);

  int channel = prefs.getInt("channel");
  IPAddress ip, gateway, subnet;

  ip.fromString(prefs.getString("ip", ""));
  gateway.fromString(prefs.getString("gateway", ""));
  subnet.fromString(prefs.getString("subnet", ""));

  WiFi.config(ip, gateway, subnet, gateway);

  Serial.print("Fast Connecting to WiFi\n");
  WiFi.begin(ssid, pass, channel, bssid_bytes);
  return true;
}

Mode switch_mode(Mode current) {
  int next = (current + 1) % MODE_COUNT;
  return (Mode)next;
}

void process_station_info(const char* station_id, const char* platform_filter) {
  Serial.println("process_station_info start");

  wifi_on();
  message_t turn_on = { TURN_ON, {}, 0, 0 };
  int ret = xQueueSend(QueueHandle, (void*)&turn_on, 0);
  taskYIELD();

  time_t now;
  time(&now);

  // get first part of request
  String json = request_station_info(station_id, now);
  StationInfo info = parse_station_info(json, platform_filter, max_display_items);
  Serial.println("Initial station info:\n");
  log_station_info(info);

  message_t m_init = { PRINT_TEXT, {}, 10, 40 };
  if (info.name == "") {
    Serial.println("Failed to get station info");
    strncpy(m_init.Text, "Failed to get station info", sizeof(m_init.Text));
    int ret = xQueueSend(QueueHandle, (void*)&m_init, 0);
    taskYIELD();

    WiFi.disconnect(true);
    message_t m_turn_off = { TURN_OFF, {}, 0, 0 };
    ret = xQueueSend(QueueHandle, (void*)&m_turn_off, 0);
    taskYIELD();
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return;
  }

  //print the title
  String station_title = get_station_title(info);
  Serial.println(station_title);
  strncpy(m_init.Text, station_title.c_str(), sizeof(m_init.Text));
  ret = xQueueSend(QueueHandle, (void*)&m_init, 0);
  taskYIELD();

  //print the time
  String curr_time = get_time_string(now);
  Serial.println(curr_time);
  message_t m_time = { PRINT_TEXT, {}, 10, EPD_HEIGHT - 10 };
  strncpy(m_time.Text, curr_time.c_str(), sizeof(m_time.Text));
  ret = xQueueSend(QueueHandle, (void*)&m_time, 0);
  taskYIELD();

  //print the battery
  String bat_pct = get_battery_pct();
  Serial.printf("Battery pct: %s\n", bat_pct.c_str());
  message_t m_bat = { PRINT_TEXT, {}, EPD_WIDTH - 100, EPD_HEIGHT - 10 };
  strncpy(m_bat.Text, bat_pct.c_str(), sizeof(m_bat.Text));
  ret = xQueueSend(QueueHandle, (void*)&m_bat, 0);
  taskYIELD();

  //print first batch of departure
  String departs = get_departures(info, 1, max_display_items);
  Serial.println(departs);
  message_t m_deps = { PRINT_TEXT, {}, 50, 100 };
  strncpy(m_deps.Text, departs.c_str(), sizeof(m_deps.Text));
  ret = xQueueSend(QueueHandle, (void*)&m_deps, 0);
  taskYIELD();

  //fetch extra departures
  int request_counter = 1;
  int items_count = info.count;
  time_t last_ts = info.last_ts;
  if (info.count > 0 && info.last_ts == info.departures[info.count - 1].scheduled_time) {
    last_ts += 60;
  }

  while (items_count < min_display_items && request_counter++ < max_request_count) {
    int items_to_fetch = max_display_items - items_count;
    json = request_station_info(station_id, last_ts);
    info = parse_station_info(json, platform_filter, items_to_fetch);
    Serial.println("Extra station info:\n");
    log_station_info(info);

    String departs = get_departures(info, items_count + 1, items_to_fetch);
    Serial.println(departs);

    message_t m_extra_deps = { PRINT_TEXT, {}, 50, -1 };
    strncpy(m_extra_deps.Text, departs.c_str(), sizeof(m_extra_deps.Text));
    ret = xQueueSend(QueueHandle, (void*)&m_extra_deps, 0);
    taskYIELD();

    items_count += info.count;
    last_ts = info.last_ts;
    if (info.count > 0 && info.last_ts == info.departures[info.count - 1].scheduled_time) {
      last_ts += 60;
    }
  }

  WiFi.disconnect(true);

  message_t m_off = { TURN_OFF, {}, 0, 0 };
  ret = xQueueSend(QueueHandle, (void*)&m_off, 0);
  taskYIELD();

  Serial.println("waiting for display to finish");
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  Serial.println("processButtonPress end");
}

String get_battery_pct() {
  const float max_vol = 4.35;
  const float min_vol = 3.5;

  uint16_t v = analogRead(BATT_PIN);
  float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
  Serial.printf("Battery voltage: %.2f\n", battery_voltage);

  int pct = (battery_voltage - min_vol) * 100 / (max_vol - min_vol);

#ifdef DEBUG_VOLTAGE
  int32_t cursor_x = EPD_WIDTH - 200;
  int32_t cursor_y = EPD_HEIGHT - 90;
  char data[32];
  sprintf(data, "%.2fV", battery_voltage);
  write_string((GFXfont*)&FiraSans, data, &cursor_x, &cursor_y, NULL);

  cursor_x = EPD_WIDTH - 200;
  cursor_y = EPD_HEIGHT - 50;
  sprintf(data, "UP: %i\%", pct);
  write_string((GFXfont*)&FiraSans, data, &cursor_x, &cursor_y, NULL);
#endif

  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return String(pct) + "%";
}

String request_station_info(const char* station_id, time_t timestamp) {
  HTTPClient http;

  char uri[256];
  snprintf(uri, sizeof(uri), "https://webapi.vvo-online.de/dm?stopid=%s&limit=%u&time=%s", station_id, departures_per_request, get_iso_time(timestamp).c_str());
  Serial.printf("Sending request\n%s\n", uri);
  http.begin(uri);
  int httpCode = http.GET();
  Serial.printf("HTTP code: %d\n", httpCode);

  String payload;
  if (httpCode == HTTP_CODE_OK) {
    payload = http.getString();
  }

  http.end();
  return payload;
}

StationInfo parse_station_info(String input, const char* platform_filter, int departures_to_parse) {
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
    if (info.count >= departures_to_parse) {
      break;
    }

    const char* platform_name = departure["Platform"]["Name"];
    const time_t scheduled_time = convert_ms_timestamp(departure["ScheduledTime"]);
    info.last_ts = scheduled_time;
    if (platform_filter && platform_filter[0] != '\0' && platform_name && strcmp(platform_filter, platform_name) != 0) {
      continue;
    }

    Departure dep;

    dep.platform = String(platform_name ? platform_name : "");
    dep.line_name = departure["LineName"].as<String>();
    dep.direction = departure["Direction"].as<String>();
    dep.real_time = convert_ms_timestamp(departure["RealTime"]);
    dep.scheduled_time = scheduled_time;
    dep.state = departure["State"].as<String>();

    info.departures[info.count++] = dep;
  }

  return info;
}

String get_station_title(const StationInfo& info) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s - %s", info.place.c_str(), info.name.c_str());
  return buf;
}

String get_departures(const StationInfo& info, unsigned int start_ind, unsigned int max_count) {
  char buf[1024];
  buf[0] = '\0';
  unsigned int bytes_written = 0;

  for (int i = 0; i < info.count && i < max_count; i++) {
    bytes_written += snprintf(buf + bytes_written, sizeof(buf) - bytes_written, "%u) %s\n", i + start_ind, get_departure_info(info.departures[i]).c_str());
  }

  return buf;
}

String get_departure_info(const Departure& dep) {
  String diffStr;
  int diff = (dep.real_time - dep.scheduled_time) / 60;
  if (dep.real_time != 0 && diff != 0) {
    diffStr = String(diff);

    if (diff > 0) {
      diffStr = '+' + diffStr;
    }

    diffStr += '\'';
  }

  char buf[256];
  if (diffStr.length() == 0) {
    snprintf(buf, sizeof(buf), "%s - %s: %s", dep.line_name.c_str(), dep.direction.c_str(), get_short_time_string(dep.scheduled_time).c_str());
  } else {
    snprintf(buf, sizeof(buf), "%s - %s: %s(%s)", dep.line_name.c_str(), dep.direction.c_str(), get_short_time_string(dep.scheduled_time).c_str(), diffStr.c_str());
  }
  return buf;
}

void log_station_info(const StationInfo& info) {
  Serial.printf("Station Place: %s\n", info.place.c_str());
  Serial.printf("Station Name: %s\n", info.name.c_str());
  Serial.printf("Station Last time: %s\n\n", get_time_string(info.last_ts).c_str());
  Serial.print("Departures\n");

  for (int i = 0; i < info.count; i++) {
    const Departure& dep = info.departures[i];

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

String get_iso_time(time_t ts) {
  tm* time = localtime(&ts);
  if (!time) return "";

  char time_buf[64];
  strftime(time_buf, sizeof(time_buf), "%FT%T", time);
  return time_buf;
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

void loop() {
}
