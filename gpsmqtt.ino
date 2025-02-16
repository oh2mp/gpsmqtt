#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <FS.h>
#include <PubSubClient.h>
#include "TinyGPSPlus.h"

#define APREQUEST 2
#define APTIMEOUT 180000
#define GPSBAUD 9600  // Some modules use 4800. Check the datasheet.

SoftwareSerial GPSserial(13, 9);  // GPIO9 isn't used and "free"
TinyGPSPlus gps;
ESP8266WiFiMulti WiFiMulti;
WiFiClient wificlient;
PubSubClient mqttclient(wificlient);
File file;

ESP8266WebServer server(80);
uint32_t portal_timer = 0;

// SmartBeacon configuration
char low_speed_str[8], low_rate_str[8], high_speed_str[8], high_rate_str[8];
char turn_time_str[8], turn_min_str[8], turn_slope_str[8];
// default settings for Smartbeaconing
int low_speed = 3;
int low_rate = 1800;
int high_speed = 100;
int high_rate = 60;
int turn_time = 15;
int turn_min = 10;
int turn_slope = 240;
unsigned long lastBeaconMillis;
bool send_now = true;
int prev_heading;

// placeholder values
char myhostname[24] = "gps2mqtt-";  // Maxlen in ESP8266WiFi class is 24
char topic[256] = "gps2mqtt";       // MQTT topic
char mqtt_user[64] = "foo";
char mqtt_pass[64] = "bar";
char mqtt_host[64] = "192.168.36.99";
int mqtt_port = 1883;

/* ------------------------------------------------------------------------------- */
void setup() {
  pinMode(APREQUEST, INPUT_PULLUP);

  uint8_t mymac[6];
  wifi_get_macaddr(STATION_IF, mymac);
  char mac_end[8];
  sprintf(mac_end, "%02x%02x%02x", mymac[3], mymac[4], mymac[5]);
  strcat(myhostname, mac_end);

  Serial.begin(115200);
  Serial.println("");
  Serial.println("GPS2MQTT - OH2MP 2024");
  Serial.printf("Default hostname: %s\n", myhostname);

  GPSserial.begin(GPSBAUD);

  SPIFFS.begin();
  loadWifis();
  delay(1000);
  WiFi.mode(WIFI_STA);
  delay(1000);
  loadMQTT();
  loadSmartbeacon();

  Serial.println(WIFI_STA);
  Serial.println(WIFI_AP);
  Serial.println(WiFi.getMode());

  if (strlen(myhostname) > 0) WiFi.hostname(myhostname);
  mqttclient.setServer(mqtt_host, mqtt_port);
}

/* ------------------------------------------------------------------------------- */
void loop() {
  char c = 0;
  int cur_speed, cur_heading, beacon_rate, turn_threshold, heading_change_since_beacon = 0;
  unsigned long currentMillis = millis(), secs_since_beacon = (currentMillis - lastBeaconMillis) / 1000;

  if (GPSserial.available() && WiFi.getMode() == WIFI_STA) {
    if (gps.encode(GPSserial.read())) {
      // We definitely are not in Gulf of Guinea.
      if (gps.location.lat() != 0 && gps.location.lng() != 0) {

        const char* mqttmsg = mqtt_message();

        if (mqttmsg[0] != '\0') {
          cur_speed = gps.speed.kmph();
          cur_heading = gps.course.deg();
          //
          // SmartBeacon (http://www.hamhud.net/hh2/smartbeacon.html)
          //
          // Slow Speed = Speed below which I consider myself "stopped" 10 m.p.h.
          // Slow Rate = Beacon rate while speed below stopped threshold (1750s = ~29mins)
          // Fast Speed = Speed that I send out beacons at fast rate 100 m.p.h.
          // Fast Rate = Beacon rate at fastest interval (175s ~ 3 mins)
          // Any speed between these limits, the beacon rate is proportional.
          // Min Turn Time = don't beacon any faster than this interval in a turn (40sec)
          // Min Turn Angle = Minimum turn angle to consider beaconing. (20 degrees)
          // Turn Slope = Number when divided by current speed creates an angle that is added to Min Turn Angle to trigger a beacon.

          // Stopped - slow rate beacon
          if (cur_speed < low_speed) {
            beacon_rate = low_rate;
          } else {
            // Adjust beacon rate according to speed
            if (cur_speed > high_speed) {
              beacon_rate = high_rate;
            } else {
              beacon_rate = high_rate * high_speed / cur_speed;
              if (beacon_rate > low_rate) {
                beacon_rate = low_rate;
              }
              if (beacon_rate < high_rate) {
                beacon_rate = high_rate;
              }
            }
            // Corner pegging - ALWAYS occurs if not "stopped"
            // - turn threshold is speed-dependent
            turn_threshold = turn_min + turn_slope / cur_speed;
            if (prev_heading > cur_heading) {
              heading_change_since_beacon = ((prev_heading - cur_heading + 360) % 360);
            } else {
              heading_change_since_beacon = ((cur_heading - prev_heading + 360) % 360);
            }
            if ((heading_change_since_beacon > turn_threshold) && (secs_since_beacon > turn_time)) {
              send_now = true;
            }
          }

          // Send beacon if SmartBeacon interval (beacon_rate) is reached
          if (secs_since_beacon > beacon_rate || send_now) {
            lastBeaconMillis = currentMillis;
            if (WiFiMulti.run() == WL_CONNECTED) {
              if (mqttclient.connect(myhostname, mqtt_user, mqtt_pass)) {
                mqttclient.publish(topic, mqttmsg);
                Serial.printf("%s %s\n", topic, mqttmsg);
              } else {
                Serial.printf("Failed to connect MQTT broker, state=%d\n", mqttclient.state());
              }
            } else {
              Serial.printf("Failed to connect WiFi, status=%d\n", WiFi.status());
            }
            send_now = false;
          }
        }
      } else {
        Serial.println("Waiting for fix");
      }
    }
  }
  if (digitalRead(APREQUEST) == LOW && portal_timer == 0) {
    portal_timer = millis();
    startPortal();
  }
  if (WiFi.getMode() == WIFI_AP) {
    server.handleClient();
  }
  if (millis() - portal_timer > APTIMEOUT && WiFi.getMode() == WIFI_AP) {
    Serial.println("Portal timeout. Booting.");
    delay(1000);
    ESP.restart();
  }
}

/* ------------------------------------------------------------------------------- */
char* mqtt_message() {
  static char msg[256];
  memset(msg, '\0', sizeof(msg));

  sprintf(msg, "{\"la\":%.6f,\"lo\":%.6f,\"kmh\":%.0f,\"cou\":%.0f,\"alt\":%.0f}",
          gps.location.lat(), gps.location.lng(), gps.speed.kmph(),
          gps.course.deg(), gps.altitude.meters());

  return (msg);
}

/* ------------------------------------------------------------------------------- */
void loadWifis() {
  if (SPIFFS.exists("/known_wifis.txt")) {
    char ssid[33];
    char pass[65];

    file = SPIFFS.open("/known_wifis.txt", "r");
    while (file.available()) {
      memset(ssid, '\0', sizeof(ssid));
      memset(pass, '\0', sizeof(pass));
      file.readBytesUntil('\t', ssid, 32);
      file.readBytesUntil('\n', pass, 64);
      WiFiMulti.addAP(ssid, pass);
      Serial.printf("wifi loaded: %s / %s\n", ssid, pass);
    }
    file.close();
  } else {
    Serial.println("no wifis configured");
  }
}

/* ------------------------------------------------------------------------------- */
void loadMQTT() {
  if (SPIFFS.exists("/mqtt.txt")) {
    char tmpstr[8];
    memset(tmpstr, 0, sizeof(tmpstr));
    memset(mqtt_host, 0, sizeof(mqtt_host));
    memset(mqtt_user, 0, sizeof(mqtt_user));
    memset(mqtt_pass, 0, sizeof(mqtt_pass));
    memset(topic, 0, sizeof(topic));
    memset(myhostname, 0, sizeof(myhostname));

    file = SPIFFS.open("/mqtt.txt", "r");
    while (file.available()) {
      file.readBytesUntil(':', mqtt_host, sizeof(mqtt_host));
      file.readBytesUntil('\n', tmpstr, sizeof(tmpstr));
      mqtt_port = atoi(tmpstr);
      if (mqtt_port < 1 || mqtt_port > 65535) mqtt_port = 1883;  // default
      file.readBytesUntil(':', mqtt_user, sizeof(mqtt_user));
      file.readBytesUntil('\n', mqtt_pass, sizeof(mqtt_pass));
      file.readBytesUntil('\n', topic, sizeof(topic));
      file.readBytesUntil('\n', myhostname, sizeof(myhostname));
    }
    file.close();
    Serial.printf("MQTT broker: %s:%d\nTopic: %s\n", mqtt_host, mqtt_port, topic);
    Serial.printf("My hostname: %s\n", myhostname);
  } else {
    Serial.println("No MQTT settings");
  }
}

/* ------------------------------------------------------------------------------- */
void loadSmartbeacon() {
  if (SPIFFS.exists("/smartbeacon.txt")) {
    file = SPIFFS.open("/smartbeacon.txt", "r");

    file.readBytesUntil('\n', low_speed_str, 8);
    if (low_speed_str[strlen(low_speed_str) - 1] == 13) low_speed_str[strlen(low_speed_str) - 1] = 0;
    low_speed = atoi(low_speed_str);

    file.readBytesUntil('\n', low_rate_str, 8);
    if (low_rate_str[strlen(low_rate_str) - 1] == 13) low_rate_str[strlen(low_rate_str) - 1] = 0;
    low_rate = atoi(low_rate_str);

    file.readBytesUntil('\n', high_speed_str, 8);
    if (high_speed_str[strlen(high_speed_str) - 1] == 13) high_speed_str[strlen(high_speed_str) - 1] = 0;
    high_speed = atoi(high_speed_str);

    file.readBytesUntil('\n', high_rate_str, 8);
    if (high_rate_str[strlen(high_rate_str) - 1] == 13) high_rate_str[strlen(high_rate_str) - 1] = 0;
    high_rate = atoi(high_rate_str);

    file.readBytesUntil('\n', turn_time_str, 8);
    if (turn_time_str[strlen(turn_time_str) - 1] == 13) turn_time_str[strlen(turn_time_str) - 1] = 0;
    turn_time = atoi(turn_time_str);

    file.readBytesUntil('\n', turn_min_str, 8);
    if (turn_min_str[strlen(turn_min_str) - 1] == 13) turn_min_str[strlen(turn_min_str) - 1] = 0;
    turn_min = atoi(turn_min_str);

    file.readBytesUntil('\n', turn_slope_str, 8);
    if (turn_slope_str[strlen(turn_slope_str) - 1] == 13) turn_slope_str[strlen(turn_slope_str) - 1] = 0;
    turn_slope = atoi(turn_slope_str);

    file.close();
    Serial.println("Smartbeacon(tm) settings loaded");
  } else {
    Serial.println("No Smartbeacon(tm) settings");
  }
}

/* ------------------------------------------------------------------------------- */
/*  Portal code begins here

     Yeah, I know that String objects are pure evil 😈, but this is meant to be
     rebooted immediately after saving all parameters, so it is quite likely that
     the heap will not fragmentate yet.
*/
/* ------------------------------------------------------------------------------- */
void startPortal() {

  Serial.print("Starting portal...");
  portal_timer = millis();

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP8266 GPS2MQTT");
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));

  server.on("/", httpRoot);
  server.on("/style.css", httpStyle);
  server.on("/wifis.html", httpWifi);
  server.on("/savewifi", httpSaveWifi);
  server.on("/mqtt.html", httpMQTT);
  server.on("/savemqtt", httpSaveMQTT);
  server.on("/smartbeacon.html", httpSmartBeacon);
  server.on("/savesmartbeacon", httpSaveSmartBeacon);
  server.on("/boot", httpBoot);

  server.onNotFound([]() {
    server.sendHeader("Refresh", "1;url=/");
    server.send(404, "text/plain", "QSD QSY");
  });
  server.begin();
  Serial.println("Portal running.");
}
/* ------------------------------------------------------------------------------- */

void httpRoot() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/index.html", "r");
  html = file.readString();
  file.close();

  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */
void httpStyle() {
  portal_timer = millis();
  String css;

  file = SPIFFS.open("/style.css", "r");
  css = file.readString();
  file.close();
  server.send(200, "text/css", css);
}

/* ------------------------------------------------------------------------------- */
void httpWifi() {
  String html;
  char tablerows[1024];
  char rowbuf[256];
  char ssid[33];
  char pass[64];
  int counter = 0;
  portal_timer = millis();

  memset(tablerows, '\0', sizeof(tablerows));

  file = SPIFFS.open("/wifis.html", "r");
  html = file.readString();
  file.close();

  if (SPIFFS.exists("/known_wifis.txt")) {
    file = SPIFFS.open("/known_wifis.txt", "r");
    while (file.available()) {
      memset(rowbuf, '\0', sizeof(rowbuf));
      memset(ssid, '\0', sizeof(ssid));
      memset(pass, '\0', sizeof(pass));
      file.readBytesUntil('\t', ssid, 33);
      file.readBytesUntil('\n', pass, 33);
      sprintf(rowbuf, "<tr><td>SSID</td><td><input type=\"text\" name=\"ssid%d\" maxlength=\"32\" value=\"%s\"></td></tr>", counter, ssid);
      strcat(tablerows, rowbuf);
      sprintf(rowbuf, "<tr><td>PASS</td><td><input type=\"text\" name=\"pass%d\" maxlength=\"63\" value=\"%s\"></td></tr>", counter, pass);
      strcat(tablerows, rowbuf);
      counter++;
    }
    file.close();
  }

  html.replace("###TABLEROWS###", tablerows);
  html.replace("###COUNTER###", String(counter));

  if (counter > 3) {
    html.replace("table-row", "none");
  }

  server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveWifi() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/known_wifis.txt", "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
  }

  for (int i = 0; i < server.arg("counter").toInt(); i++) {
    if (server.arg("ssid" + String(i)).length() > 0) {
      file.print(server.arg("ssid" + String(i)));
      file.print("\t");
      file.print(server.arg("pass" + String(i)));
      file.print("\n");
    }
  }
  // Add new
  if (server.arg("ssid").length() > 0) {
    file.print(server.arg("ssid"));
    file.print("\t");
    file.print(server.arg("pass"));
    file.print("\n");
  }
  file.close();

  file = SPIFFS.open("/ok.html", "r");
  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "2;url=/");
  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */
void httpMQTT() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/mqtt.html", "r");
  html = file.readString();
  file.close();

  html.replace("###HOSTPORT###", String(mqtt_host) + ":" + String(mqtt_port));
  html.replace("###USERPASS###", String(mqtt_user) + ":" + String(mqtt_pass));
  html.replace("###TOPIC###", String(topic));
  html.replace("###MYHOSTNAME###", String(myhostname));

  server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */
void httpSaveMQTT() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/mqtt.txt", "w");
  file.printf("%s\n", server.arg("hostport").c_str());
  file.printf("%s\n", server.arg("userpass").c_str());
  file.printf("%s\n", server.arg("topic").c_str());
  file.printf("%s\n", server.arg("myhostname").c_str());
  file.close();
  loadMQTT();  // reread

  file = SPIFFS.open("/ok.html", "r");
  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "2;url=/");
  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */
void httpSmartBeacon() {
  portal_timer = millis();
  String html;
  String symtab;

  file = SPIFFS.open("/smartbeacon.html", "r");
  html = file.readString();
  file.close();

  html.replace("###LOWSPEED###", String(low_speed));
  html.replace("###LOWRATE###", String(low_rate));
  html.replace("###HIGHSPEED###", String(high_speed));
  html.replace("###HIGHRATE###", String(high_rate));
  html.replace("###TURNMIN###", String(turn_min));
  html.replace("###TURNSLOPE###", String(turn_slope));
  html.replace("###TURNTIME###", String(turn_time));

  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */
void httpSaveSmartBeacon() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/smartbeacon.txt", "w");
  file.println(server.arg("low_speed"));
  file.println(server.arg("low_rate"));
  file.println(server.arg("high_speed"));
  file.println(server.arg("high_rate"));
  file.println(server.arg("turn_min"));
  file.println(server.arg("turn_slope"));
  file.println(server.arg("turn_time"));
  file.close();

  // reread config from file
  loadSmartbeacon();

  file = SPIFFS.open("/ok.html", "r");
  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "3;url=/");
  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */
void httpBoot() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/ok.html", "r");
  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "2;url=about:blank");
  server.send(200, "text/html; charset=UTF-8", html);
  delay(1000);

  ESP.restart();
}
/* ------------------------------------------------------------------------------- */
