//////// Core Libraries ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <FS.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <Update.h>
#include <EEPROM.h>
#include <Ticker.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include "version.h"

///////// External Libraries///////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <MQTT.h>                 //https://github.com/256dpi/arduino-mqtt
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager/archive/development.zip
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson/releases/download/v6.8.0-beta/ArduinoJson-v6.8.0-beta.zip
#include <WebSocketsServer.h>     //https://github.com/Links2004/arduinoWebSockets

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define HOSTNAME "CeilingLights"

#define MAX_DEVICES 8

char mqtt_server[40] = "192.168.0.xxx";
char mqtt_username[40] = "";
char mqtt_password[40] = "";
char mqtt_port[6] = "1883";

char AP_PASS[32] = "TouchLights"; // AP Password
const byte DNS_PORT = 53;

uint8_t light_pin[MAX_DEVICES] = {16, 4, 32, 33, 5, 14, 27, 26};

const uint8_t light_bri_cycle[] = {15, 95, 175, 255};

const uint8_t switchPin = 22;

#define BUTTON_PRESS_THRESHOLD_MS 1200

// #define HA_AUTO_DISCOVERY

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/*****************  DECLARATIONS  ****************************************/

WebServer server(80);
const char* updateIndex = "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";
WebSocketsServer webSocket = WebSocketsServer(81);
WiFiClient net;
MQTTClient client(512);
Ticker sendStat;
DNSServer dns;

/*****************  GLOBAL VARIABLES  ************************************/

struct LED_LIGHTS
{
  uint8_t pin;
  uint8_t brightness = 64;
  bool state = false;
  uint32_t frequency = 40000;
  uint8_t resolution = 8;
} Light[MAX_DEVICES];

char light_topic_in[100] = "", light_topic_out[100] = "";
char master_topic_in[100] = "", master_topic_out[100] = "";

char mqtt_client_name[100] = HOSTNAME;

bool shouldSaveConfig = false;
bool shouldUpdateLights = false;
bool shouldReboot = false;

unsigned long lastMillis = 0;
char NameChipId[64] = {0}, chipId[9] = {0};

IPAddress apIP(192, 168, 4, 1);

uint8_t brightness_index = 0;
uint8_t light_bri_cycle_max = sizeof(light_bri_cycle)/sizeof(*light_bri_cycle);

volatile bool pressed = false;
unsigned long buttonHold = 0, startPress = 0, cycleTime = 0;
bool lightsOn = false;

// /*****************  Misc Functions *****************************/

void saveConfigCallback()
{
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// /*****************  Read SPIFFs values *****************************/
void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root)
  {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      Serial.print("  DIR : ");
      Serial.print(file.name());
      time_t t = file.getLastWrite();
      struct tm *tmstruct = localtime(&t);
      Serial.printf("  LAST WRITE: %d-%02d-%02d %02d:%02d:%02d\n", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
      if (levels)
      {
        listDir(fs, file.name(), levels - 1);
      }
    }
    else
    {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.print(file.size());
      time_t t = file.getLastWrite();
      struct tm *tmstruct = localtime(&t);
      Serial.printf("  LAST WRITE: %d-%02d-%02d %02d:%02d:%02d\n", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
    }
    file = root.openNextFile();
  }
  Serial.println(F("SPIFFs started"));
  Serial.println(F("---------------------------"));
}

// /*****************  EEPROM to save light state *****************************/

int eeprom_addr = 0;

void writeEEPROM(void)
{
  size_t LED_data_size = sizeof(Light[0]);
  for (uint8_t i = 0; i < MAX_DEVICES; i++)
    EEPROM.put(eeprom_addr + 1 + LED_data_size * i, Light[i]);
  EEPROM.commit();
}

void readEEPROM(void)
{
  char eeprominit = char(EEPROM.read(eeprom_addr));
  if (eeprominit != 'w')
  {
    EEPROM.write(eeprom_addr, 'w');
    writeEEPROM();
    shouldSaveConfig = true;
  }
  else
  {
    size_t LED_data_size = sizeof(Light[0]);
    for (uint8_t i = 0; i < MAX_DEVICES; i++)
      EEPROM.get(eeprom_addr + 1 + LED_data_size * i, Light[i]);
  }
}

// /*****************  Light's States *****************************/

void initLights(void)
{
  for (uint8_t i = 0; i < MAX_DEVICES; i++)
  {
    Light[i].pin = light_pin[i];
    ledcSetup(i, Light[i].frequency, Light[i].resolution);
    ledcAttachPin(Light[i].pin, i);
  }
}

void setLights(uint8_t single_pin = 99)
{
  uint8_t begin_i = 0, end_i = MAX_DEVICES;
  if (single_pin != 99)
  {
    begin_i = single_pin;
    end_i = single_pin + 1;
  }
  for (uint8_t i = begin_i; i < end_i; i++)
  {
    if(Light[i].state)
      ledcWrite(i, Light[i].brightness);
    else
      ledcWrite(i, 0);
  }
  writeEEPROM();
  shouldUpdateLights = false;
}

void setAllOn(void)
{
  for (uint8_t i = 0; i < MAX_DEVICES; i++)
    Light[i].state = true;
  //setLights();
}

void setAllOff(void)
{
  for (uint8_t i = 0; i < MAX_DEVICES; i++)
    Light[i].state = false;
  //setLights();
}

// /*****************  MQTT Stuff *****************************/

String statusMsg(void)
{
  /*
  Will send out something like this:
  {
    "light1":"off",
    "light2":"off",
    "light3":"off",
    "light4":"off",
    "light5":"off",
    "light6":"off",
    "light7":"off",
    "light8":"off",
    "light1b": 255,
    "light2b": 255,
    "light3b": 255,
    "light4b": 255,
    "light5b": 255,
    "light6b": 255,
    "light7b": 255,
    "light8b": 255
  }
  */

  DynamicJsonDocument json(JSON_OBJECT_SIZE(MAX_DEVICES*2) + 600);
  for (uint8_t i = 0; i < MAX_DEVICES; i++)
  {
    String l_name = "light" + String(i + 1);
    json[l_name] = (Light[i].state) ? "on" : "off";
    String l_bri = l_name + "b";
    json[l_bri] = Light[i].brightness;
  }
  String msg_str;
  serializeJson(json, msg_str);
  return msg_str;
}

void sendMQTTStatusMsg(void)
{
  Serial.print(F("Sending ["));
  Serial.print(light_topic_out);
  Serial.print(F("] >> "));
  Serial.println(statusMsg());
  client.publish(light_topic_out, statusMsg(), true, 0);
  sendStat.detach();
}

void sendMQTTMasterStatusMsg(void)
{
  Serial.print(F("Sending ["));
  Serial.print(master_topic_out);
  Serial.print(F("] >> "));

  char master_status_msg[32] ="";
  if (lightsOn) 
    sprintf(master_status_msg, "%s", "{'master':'ON'}");
  else
    sprintf(master_status_msg, "%s", "{'master':'OFF'}");

  Serial.println(master_status_msg);
  client.publish(light_topic_out, master_status_msg, true, 0);
}

void processJson(String &payload)
{
  /*
  incoming message template:
  {
    "light": 1,
    "state": "ON",
    "brightness": 255
  }
  */

  DynamicJsonDocument jsonBuffer(JSON_OBJECT_SIZE(3) + 100);
  DeserializationError error = deserializeJson(jsonBuffer, payload);
  if (error)
  {
    Serial.print(F("parseObject() failed: "));
    Serial.println(error.c_str());
  }
  JsonObject root = jsonBuffer.as<JsonObject>();

  if (root.containsKey("light"))
  {
    uint8_t index = jsonBuffer["light"];
    index--;
    if (index >= MAX_DEVICES)
      return;

    if(root.containsKey("state"))
    {
      String stateValue = jsonBuffer["state"];
      if (stateValue == "ON" or stateValue == "on")
      {
        Light[index].state = true;
        if(!root.containsKey("brightness"))
          Light[index].brightness = 255;
        shouldUpdateLights = true;
        sendMQTTStatusMsg();
        webSocket.broadcastTXT(statusMsg().c_str());
      }
      else if (stateValue == "OFF" or stateValue == "off")
      {
        Light[index].state = false;
        shouldUpdateLights = true;
        sendMQTTStatusMsg();
        webSocket.broadcastTXT(statusMsg().c_str());
      }
    }

    if(root.containsKey("brightness"))
    {
      Light[index].brightness = (uint8_t) jsonBuffer["brightness"];
      if(!Light[index].brightness) // When brightness is 0, turn off light
        Light[index].state = false;
      shouldUpdateLights = true;
      sendMQTTStatusMsg();
      webSocket.broadcastTXT(statusMsg().c_str());
    }
  }

  if (root.containsKey("master"))
  {
    String stateValue = jsonBuffer["master"];
    if (stateValue == "ON" or stateValue == "on")
    {
      setAllOn();
      lightsOn = true;
      shouldUpdateLights = true;
      sendMQTTMasterStatusMsg();
      sendMQTTStatusMsg();
      webSocket.broadcastTXT(statusMsg().c_str());
    }
    else if (stateValue == "OFF" or stateValue == "off")
    {
      setAllOff();
      lightsOn = false;
      shouldUpdateLights = true;
      sendMQTTMasterStatusMsg();
      sendMQTTStatusMsg();
      webSocket.broadcastTXT(statusMsg().c_str());
    }
  }
}

void messageReceived(String &topic, String &payload)
{
  Serial.println("Incoming: [" + topic + "] << " + payload);
  processJson(payload);
}

#ifdef HA_AUTO_DISCOVERY
void sendAutoDiscoverySingle(String index, String &discovery_topic)
{
  /*
  "discovery topic" >> "homeassistant/light/XXXXXXXXXXXXXXXX/config"

  Sending data that looks like this >>
  {
    "name":"lights1",
    "schema":"template",
    "state_topic": "ceiling/aabbccddeeff/out",
    "command_topic": "ceiling/aabbccddeeff/in",
    "brightness_template": "{{value_json.light1b}}",
    "command_on_template":"{'light':1,'state':'ON'{%- if brightness is defined -%},'brightness':{{ brightness|d }}{%- endif -%}}",
    "command_off_template":"{'light':1,'state':'OFF'}",
    "state_template": "{{value_json.light1}}",
    "optimistic": false,
    "qos": 0
  }
  */

  const size_t capacity = JSON_OBJECT_SIZE(10) + 700;
  DynamicJsonDocument json(capacity);

  json["name"] = String(HOSTNAME) + " " + index;
  json["schema"] = "template";
  json["state_topic"] = light_topic_out;
  json["command_topic"] = light_topic_in;
  json["brightness_template"] = "{{value_json.light" + index + "b}}";
  json["command_on_template"] = "{'light':" + index + ",'state':'ON'{%- if brightness is defined -%},'brightness':{{ brightness|d }}{%- endif -%}}";
  json["command_off_template"] = "{'light':" + index + ",'state':'OFF'}";
  json["state_template"] = "{{value_json.light" + index + "}}";
  json["optimistic"] = false;
  json["qos"] = 0;

  String msg_str;
  Serial.print(F("Sending AD MQTT ["));
  Serial.print(discovery_topic);
  Serial.print(F("] >> "));
  serializeJson(json, Serial);
  serializeJson(json, msg_str);
  client.publish(discovery_topic, msg_str, true, 0);
  Serial.println();
}

void sendAutoDiscoverySwitch(String &discovery_topic)
{
  /*
  "discovery topic" >> "homeassistant/switch/XXXXXXXXXXXXXXXX/config"

  Sending data that looks like this >>
  {
    "name":"Ceiling Lights Master Switch",
    "state_topic": "ceiling/aabbccddeeff_master/out",
    "command_topic": "ceiling/aabbccddeeff_master/in",
    "payload_on":"{'master':'ON'}",
    "payload_off":"{'master':'OFF'}",
    "value_template": "{{ value_json.master }}",
    "state_on": "ON",
    "state_off": "OFF",
    "optimistic": false,
    "qos": 0,
    "retain": true
  }
  */

  const size_t capacity = JSON_OBJECT_SIZE(11) + 500;
  DynamicJsonDocument json(capacity);

  json["name"] = String(HOSTNAME)+" Master Switch";
  json["state_topic"] = master_topic_out;
  json["command_topic"] = master_topic_in;
  json["payload_on"] = "{'master':'ON'}";
  json["payload_off"] = "{'master':'OFF'}";
  json["value_template"] = "{{value_json.master}}";
  json["state_on"] = "ON";
  json["state_off"] = "OFF";
  json["optimistic"] = false;
  json["qos"] = 0;
  json["retain"] = true;

  String msg_str;
  Serial.print(F("Sending AD MQTT ["));
  Serial.print(discovery_topic);
  Serial.print(F("] >> "));
  serializeJson(json, Serial);
  serializeJson(json, msg_str);
  client.publish(discovery_topic, msg_str, true, 0);
  Serial.println();
}

void sendAutoDiscovery(void)
{
  for (uint8_t i = 0; i < MAX_DEVICES; i++)
    sendAutoDiscoverySingle(String(i + 1), "homeassistant/light/" + String(HOSTNAME) + String(i + 1) + "/config");

  sendAutoDiscoverySwitch("homeassistant/switch/" + String(HOSTNAME) + "/config");
}
#endif

void connect_mqtt(void)
{
  // Make sure the WiFi is connected, otherwise we know the MQTT won't connect
  Serial.print(F("Checking wifi "));
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin();
    // Not connected
    Serial.println(F("not connected!"));
    return;
  }

  // Attempt to connect once
  Serial.println(F("connected!"));
  Serial.print(F("Attempting MQTT connection..."));
  if (!client.connect(mqtt_client_name, mqtt_username, mqtt_password)) {
    // Failed to connect
    Serial.println(F("not connected!"));
    return;
  }

  Serial.println(F("connected!"));

  client.subscribe(light_topic_in);      //subscribe to incoming topic
  client.subscribe(master_topic_in);     //subscribe to master switch topic
#ifdef HA_AUTO_DISCOVERY
  sendAutoDiscovery();                   //send auto-discovery topics
#endif
  sendStat.attach(2, sendMQTTStatusMsg); //send status of switches
}

// /****************************  Read/Write MQTT Settings from SPIFFs ****************************************/

bool readConfigFS()
{
  //if (resetsettings) { SPIFFS.begin(); SPIFFS.remove("/config.json"); SPIFFS.format(); delay(1000);}
  if (SPIFFS.exists("/config.json"))
  {
    Serial.print(F("Read cfg: "));
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile)
    {
      size_t size = configFile.size(); // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);
      configFile.readBytes(buf.get(), size);
      StaticJsonDocument<200> jsonBuffer;
      DeserializationError error = deserializeJson(jsonBuffer, buf.get());
      if (!error)
      {
        JsonObject json = jsonBuffer.as<JsonObject>();
        serializeJson(json, Serial);
        strcpy(mqtt_server, json["mqtt_server"]);
        strcpy(mqtt_port, json["mqtt_port"]);
        strcpy(mqtt_username, json["mqtt_username"]);
        strcpy(mqtt_password, json["mqtt_password"]);
        return true;
      }
      else
        Serial.println(F("Failed to parse JSON!"));
    }
    else
      Serial.println(F("Failed to open \"/config.json\""));
  }
  else
    Serial.println(F("Couldn't find \"/config.json\""));
  return false;
}

bool writeConfigFS()
{
  Serial.print(F("Saving /config.json: "));
  StaticJsonDocument<200> jsonBuffer;
  JsonObject json = jsonBuffer.to<JsonObject>();
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_username"] = mqtt_username;
  json["mqtt_password"] = mqtt_password;
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile)
  {
    Serial.println(F("failed to open config file for writing"));
    return false;
  }
  serializeJson(json, Serial);
  serializeJson(json, configFile);
  configFile.close();
  Serial.println(F("ok!"));
  return true;
}

// /**********************  WebSocket & WebServer ******************************/

#include "indexhtml.h" //contains gzipped minimized index.html

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{

  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.printf("[%u] Disconnected!\n", num);
    break;
  case WStype_CONNECTED:
  {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
    Serial.printf("WS [%u] << %s\n", num, payload);
    webSocket.sendTXT(num, statusMsg().c_str());
  }
  break;
  case WStype_TEXT:
    Serial.printf("WS [%u] << %s\n", num, payload);
    String msg = String((char *)payload);
    processJson(msg);
    webSocket.sendTXT(num, "OK");
    break;
  }
}

void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

// /****************************  Button Press  ***********************************/

void checkSwitch()
{
  if (digitalRead(switchPin) == LOW && pressed == false)
  {
    startPress = millis();
    cycleTime = millis();
    pressed = true;
    Serial.println("button pressed start");
  }

  if (pressed == true && (millis() - cycleTime) >= BUTTON_PRESS_THRESHOLD_MS) {
    cycleTime = millis();
    if (brightness_index >= light_bri_cycle_max) {
      brightness_index = 0;
    }

    for(uint8_t i=0; i<MAX_DEVICES; i++)
    {
      Light[i].state = true;
      Light[i].brightness = light_bri_cycle[brightness_index];
    }
    setLights();
    sendMQTTStatusMsg();
    webSocket.broadcastTXT(statusMsg().c_str());
    brightness_index++;
  }

  if (digitalRead(switchPin) == HIGH && pressed == true)
  {
    buttonHold = millis() - startPress;
    Serial.println("button pressed end");
    if (buttonHold > 50
        && buttonHold < BUTTON_PRESS_THRESHOLD_MS)
    {
      Serial.println("normal press");
      if (lightsOn == false)
      {
        for(uint8_t i=0; i<MAX_DEVICES; i++)
        {
          Light[i].state = true;
          Light[i].brightness = 255;
        }
        setLights();
        sendMQTTStatusMsg();
        webSocket.broadcastTXT(statusMsg().c_str());
        brightness_index = 0;
        lightsOn = true;
      }
      else if (lightsOn == true)
      {
        for(uint8_t i=0; i<MAX_DEVICES; i++)
        {
          Light[i].state = false;
          Light[i].brightness = 0;
        }
        setLights();
        sendMQTTStatusMsg();
        webSocket.broadcastTXT(statusMsg().c_str());
        brightness_index = 0;
        lightsOn = false;
      }
    }
    pressed = false;
  }
}

// /****************************  SETUP  ****************************************/

void setup()
{
  EEPROM.begin(512);
  Serial.begin(115200);
  delay(10);

  initLights();
  readEEPROM();
  setLights();

  pinMode(switchPin, INPUT_PULLUP);

  Serial.println(F("---------------------------"));
  Serial.println(F("Starting SPIFFs"));
  if (SPIFFS.begin(true)) //format SPIFFS if needed
  {
    listDir(SPIFFS, "/", 0);
  }

  if (readConfigFS())
    Serial.println(F(" yay!"));

  WiFi.mode(WIFI_AP_STA); // Make sure you're in station mode

  WiFi.setHostname(HOSTNAME);
  snprintf(chipId, sizeof(chipId), "%08x", (uint32_t)ESP.getEfuseMac());
  snprintf(NameChipId, sizeof(NameChipId), "%s_%08x", HOSTNAME, (uint32_t)ESP.getEfuseMac());
  WiFi.setHostname(const_cast<char *>(NameChipId));

  WiFiManager wifiManager;

  WiFiManagerParameter custom_mqtt_server("mqtt_server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_username("mqtt_username", "mqtt username", mqtt_username, 40, " maxlength=31");
  WiFiManagerParameter custom_mqtt_password("mqtt_password", "mqtt password", mqtt_password, 40, " maxlength=31 type='password'");
  WiFiManagerParameter custom_mqtt_port("mqtt_port", "mqtt port", mqtt_port, 6);

  wifiManager.setSaveConfigCallback(saveConfigCallback);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_username);
  wifiManager.addParameter(&custom_mqtt_password);

  wifiManager.setConnectTimeout(60);
  if (!wifiManager.autoConnect(HOSTNAME))
  {
    Serial.println(F("Failed to connect and hit timeout"));
    // delay(3000);
    // ESP.restart();
    // delay(5000);
  }
  else
  {
    Serial.println(F("---------------------------------------"));
    Serial.print(F("Router IP: "));
    Serial.println(WiFi.localIP());
  }

  Serial.println(F("---------------------------------------"));

  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(NameChipId, AP_PASS);
  Serial.print(F("HotSpt IP: "));
  Serial.println(WiFi.softAPIP());
  dns.start(DNS_PORT, "*", WiFi.softAPIP());

  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_username, custom_mqtt_username.getValue());
  strcpy(mqtt_password, custom_mqtt_password.getValue());

  if (shouldSaveConfig)
  {
    writeConfigFS();
    shouldSaveConfig = false;
  }

  byte mac[6];
  WiFi.macAddress(mac);
  sprintf(mqtt_client_name, "%02X%02X%02X%02X%02X%02X%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], HOSTNAME);
  sprintf(light_topic_in, "ceiling/%02X%02X%02X%02X%02X%02X%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], "/in");
  sprintf(light_topic_out, "ceiling/%02X%02X%02X%02X%02X%02X%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], "/out");
  sprintf(master_topic_in, "ceiling/%02X%02X%02X%02X%02X%02X%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], "_master/in");
  sprintf(master_topic_out, "ceiling/%02X%02X%02X%02X%02X%02X%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], "_master/out");

  client.begin(mqtt_server, atoi(mqtt_port), net);
  client.onMessage(messageReceived);

  ArduinoOTA.setHostname(NameChipId);
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.print(F(" HTTP server starting "));
  server.on("/", HTTP_GET, [&] {
    server.sendHeader("Content-Encoding", "gzip", true);
    server.send_P(200, PSTR("text/html"), index_htm_gz, index_htm_gz_len);
  });
  server.on("/status", HTTP_GET, [&] {
    server.send(200, "application/json", statusMsg());
  });
  server.on("/version", HTTP_GET, [&] {
    server.send(200, "application/json", SKETCH_VERSION);
  });
  server.on("/restart", HTTP_GET, [&]() {
    Serial.println(F("/restart"));
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/html", "<META http-equiv='refresh' content='15;URL=/'><body align=center><H4>Restarting...</H4></body>");
    shouldReboot = true;
  });
  server.on("/reset_wifi", HTTP_GET, [&]() {
    Serial.println(F("/reset_wlan"));
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/html", "<META http-equiv='refresh' content='15;URL=/'><body align=center><H4>Resetting WLAN and restarting...</H4></body>");
    WiFiManager wm;
    wm.resetSettings();
    shouldReboot = true;
  });
  server.on("/update", HTTP_GET, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/html", updateIndex);
  });
  server.on("/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "<META http-equiv='refresh' content='15;URL=/'><body align=center><H4>Update: FAILED, refreshing in 15s.</H4></body>": "<META http-equiv='refresh' content='15;URL=/'><body align=center><H4>Update: OK, refreshing in 15s.</H4></body>");
      shouldReboot = true;
    }, []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.setDebugOutput(true);
        Serial.printf("Update: %s\n", upload.filename.c_str());
        if (!Update.begin()) { //start with max available size
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) { //true to set the size to the current progress
          Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
        Serial.setDebugOutput(false);
      }
  });
  server.begin();
  Serial.print(F(" done!\n"));
}

// /*****************  MAIN LOOP  ****************************************/

void loop()
{
  if (shouldUpdateLights)
    setLights();

  ArduinoOTA.handle();
  server.handleClient();
  webSocket.loop();
  checkSwitch();

  if (shouldReboot)
  {
    Serial.println(F("Rebooting..."));
    delay(100);
    ESP.restart();
  }

  if (!client.connected()) {
    if (millis() - lastMillis > 5000 or !lastMillis)
    {
      lastMillis = millis();
      connect_mqtt();
    }
  } else {
    client.loop();
  }
}
