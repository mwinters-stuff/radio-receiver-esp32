#include <Adafruit_BMP280.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_Sensor.h>
#include <ArduinoOTA.h>
#include <DallasTemperature.h>
#include <LittleFS.h>
#include <OneWire.h>
#include <PubSubClient.h>
#include <RF24.h>
#include <RF24Network.h>
#include <Wire.h>
#include <WiFi.h>
#include <YAMLDuino.h>
#include <driver/gpio.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <ESPmDNS.h>

#include <cstdint>
#include <vector>

#define _USE_SLEEP

#define RADIO_CE_PIN 16
#define RADIO_CSN_PIN 5
#define RADIO_INT_PIN 27
#define ONE_WIRE_PIN 4

#define OTA_PASSWORD "ota"

constexpr uint16_t NO_SENSOR_VALUE = UINT16_MAX;
constexpr uint8_t MESSAGE_SENSOR = 'R';
constexpr const char *CONFIG_PATH = "/config.yaml";

// Network ID for sensors read directly from this board's local peripherals.
constexpr uint16_t LOCAL_SENSOR_NETWORK_ID = 2000;
constexpr uint32_t LOCAL_SENSOR_INTERVAL_MS = 2UL * 60UL * 1000UL;

struct SensorMessage {
  uint16_t location;
  uint16_t temperature_reading;
  uint16_t humidity_reading;
  uint16_t voltage_reading;
  uint16_t pressure_reading;
  uint16_t light_reading;
};

// Sensor location/topic metadata, loaded from CONFIG_PATH on LittleFS.
struct SensorConfig {
  uint16_t networkId = 0;
  String id;
  String name;
  String area;
  std::vector<String> sensors;
  bool enabled = false;
};

RF24 radio(RADIO_CE_PIN, RADIO_CSN_PIN);
RF24Network network(radio);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
uint32_t lastMqttAttempt = 0;

// Set by the RF24 IRQ line; only used to wake the CPU from light sleep promptly.
volatile bool radioIrqFlag = false;

void IRAM_ATTR onRadioIrq() { radioIrqFlag = true; }

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature dallasSensors(&oneWire);
Adafruit_SHT4x sht4;
Adafruit_BMP280 bmp280;
bool dallasReady = false;
bool sht4Ready = false;
bool bmp280Ready = false;
bool localSensorsInitialRead = true;
uint32_t lastLocalReadMillis = 0;

std::vector<SensorConfig> sensorConfigs;
uint8_t radioChannel = 0x21;
uint16_t radioNode = 0;
String wifiSsid;
String wifiPassword;
String hostName;
String mqttHost;
uint16_t mqttPort = 1883;
String mqttUsername;
String mqttPassword;
String mqttBaseTopic = "sensor-net";
String mqttDeviceId = "sensor-net";
String mqttDeviceName = "Sensor Net";

const SensorConfig *findConfig(uint16_t networkId) {
  for (const SensorConfig &config : sensorConfigs) {
    if (config.networkId == networkId) return &config;
  }
  return nullptr;
}

bool hasSensor(const SensorConfig &config, const char *name) {
  for (const String &sensor : config.sensors) {
    if (sensor == name) return true;
  }
  return false;
}

// Splits a "scheme://host:port" style value from the YAML config.
void parseMqttHost(const String &raw, String &host, uint16_t &port) {
  String value = raw;
  int schemeEnd = value.indexOf("://");
  if (schemeEnd >= 0) value = value.substring(schemeEnd + 3);
  int colon = value.indexOf(':');
  if (colon >= 0) {
    host = value.substring(0, colon);
    port = static_cast<uint16_t>(value.substring(colon + 1).toInt());
  } else {
    host = value;
  }
}

bool parseBool(const char *value, bool fallback) {
  if (!value) return fallback;
  return strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

void loadConfig() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed, using build-flag defaults");
    return;
  }

  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    Serial.printf("%s not found, using build-flag defaults\n", CONFIG_PATH);
    return;
  }

  Serial.printf("Loading %s\n", file);

  YAMLNode root = YAMLNode::loadStream(file);
  file.close();
  if (root.isNull()) {
    Serial.printf("Failed to parse %s\n", CONFIG_PATH);
    return;
  }

  if (const char *channel = root["radio"]["channel"].scalar())
    radioChannel = static_cast<uint8_t>(strtoul(channel, nullptr, 0));
  if (const char *node = root["radio"]["node"].scalar())
    radioNode = static_cast<uint16_t>(strtoul(node, nullptr, 0));

  YAMLNode wifi = root["wifi"];
  if (!wifi.isNull()) {
    if (const char *ssid = wifi["ssid"].scalar()) wifiSsid = ssid;
    if (const char *password = wifi["password"].scalar()) wifiPassword = password;
    if (const char *hostname = wifi["hostname"].scalar()) hostName = hostname;
  }

  YAMLNode mqtt = root["mqtt"];
  if (!mqtt.isNull()) {
    if (const char *host = mqtt["host"].scalar()) parseMqttHost(host, mqttHost, mqttPort);
    if (const char *username = mqtt["username"].scalar()) mqttUsername = username;
    if (const char *password = mqtt["password"].scalar()) mqttPassword = password;
    if (const char *baseTopic = mqtt["base_topic"].scalar()) mqttBaseTopic = baseTopic;
    if (const char *deviceId = mqtt["device_id"].scalar()) mqttDeviceId = deviceId;
    if (const char *deviceName = mqtt["device_name_prefix"].scalar()) mqttDeviceName = deviceName;
  }

  YAMLNode locations = root["locations"];
  if (locations.isSequence()) {
    sensorConfigs.clear();
    for (size_t i = 0; i < locations.size(); i++) {
      YAMLNode entry = locations[static_cast<int>(i)];
      SensorConfig config;
      if (const char *networkId = entry["network_id"].scalar())
        config.networkId = static_cast<uint16_t>(strtoul(networkId, nullptr, 10));
      if (const char *id = entry["id"].scalar()) config.id = id;
      if (const char *name = entry["name"].scalar()) config.name = name;
      if (const char *area = entry["area"].scalar()) config.area = area;
      config.enabled = parseBool(entry["enabled"].scalar(), false);

      YAMLNode sensors = entry["sensors"];
      if (sensors.isSequence()) {
        for (size_t s = 0; s < sensors.size(); s++) {
          if (const char *sensorName = sensors[static_cast<int>(s)].scalar())
            config.sensors.push_back(sensorName);
        }
      }
      sensorConfigs.push_back(config);
    }
  }

  Serial.printf("Loaded %s: %u locations\n", CONFIG_PATH, static_cast<unsigned>(sensorConfigs.size()));
}

String valueTopic(const SensorConfig &config, const char *name) {
  return mqttDeviceId + "/" + config.id + "/" + name;
}

void publishValue(const String &topic, const String &value) {
  if (mqttClient.connected()) mqttClient.publish(topic.c_str(), value.c_str(), true);
}

// Indents a compact JSON string for readable Serial logging.
void printPrettyJson(const String &json) {
  int indent = 0;
  bool inString = false;
  for (size_t i = 0; i < json.length(); i++) {
    char c = json[i];
    if (c == '"' && (i == 0 || json[i - 1] != '\\')) inString = !inString;

    if (!inString && (c == '{' || c == '[')) {
      Serial.println(c);
      indent++;
      for (int j = 0; j < indent; j++) Serial.print("  ");
    } else if (!inString && (c == '}' || c == ']')) {
      Serial.println();
      indent--;
      for (int j = 0; j < indent; j++) Serial.print("  ");
      Serial.print(c);
    } else if (!inString && c == ',') {
      Serial.println(c);
      for (int j = 0; j < indent; j++) Serial.print("  ");
    } else if (!inString && c == ':') {
      Serial.print(": ");
    } else {
      Serial.print(c);
    }
  }
  Serial.println();
}

void publishDiscovery(const SensorConfig &config, const char *name,
                      const char *unit, const char *deviceClass) {
  String topic = String("homeassistant/sensor/") + mqttDeviceId + "/" +
                 config.id + "-" + name + "/config";
  String payload = "{\"unique_id\":\"" + mqttDeviceId + "-" +
                   config.id + "-" + name + "\",\"name\":\"" + config.name +
                   "\",\"state_topic\":\"" + valueTopic(config, name) +
                   "\",\"unit_of_measurement\":\"" + unit +
                   "\",\"state_class\":\"measurement\",\"force_update\":true,\"device_class\":\"" +
                   deviceClass + "\",\"device\":{\"identifiers\":\"" +
                   mqttDeviceId + "\",\"suggested_area\":\"" + config.area +
                   "\",\"name\":\"" + mqttDeviceName + "\"},\"availability_topic\":\"" +
                   mqttBaseTopic + "/status\",\"payload_available\":\"ONLINE\",\"payload_not_available\":\"OFFLINE\"}";

  Serial.printf("Discovery: %s\n", topic.c_str());
  printPrettyJson(payload);

  publishValue(topic, payload);
}

void publishDiscovery() {
  for (const SensorConfig &config : sensorConfigs) {
    if (!config.enabled) continue;
    if (hasSensor(config, "temperature")) publishDiscovery(config, "temperature", "C", "temperature");
    if (hasSensor(config, "humidity")) publishDiscovery(config, "humidity", "%", "humidity");
    if (hasSensor(config, "battery")) publishDiscovery(config, "battery", "V", "voltage");
    if (hasSensor(config, "pressure")) publishDiscovery(config, "pressure", "hPa", "pressure");
    if (hasSensor(config, "light")) publishDiscovery(config, "light", "lx", "illuminance");
  }
}

void publishReading(const SensorMessage &message) {
  const SensorConfig *config = findConfig(message.location);
  if (!config || !config->enabled || !mqttClient.connected()) return;

  Serial.printf("Sending %s\n", config->name);

  if (message.temperature_reading != NO_SENSOR_VALUE && hasSensor(*config, "temperature"))
  {
    Serial.printf("   Temperature: %0.2f\n", message.temperature_reading / 1000.0f);
    publishValue(valueTopic(*config, "temperature"), String(message.temperature_reading / 1000.0f, 2));
  }
  if (message.humidity_reading != NO_SENSOR_VALUE && hasSensor(*config, "humidity"))
  {
    Serial.printf("      Humidity: %0.2f\n", message.humidity_reading / 100.0f);
    publishValue(valueTopic(*config, "humidity"), String(message.humidity_reading / 100.0f, 2));
  }
  if (message.voltage_reading != NO_SENSOR_VALUE && hasSensor(*config, "battery"))
  {
    Serial.printf("       Battery: %0.2f\n", message.voltage_reading / 1000.0f);
    publishValue(valueTopic(*config, "battery"), String(message.voltage_reading / 1000.0f, 2));
  }
  if (message.pressure_reading != NO_SENSOR_VALUE && hasSensor(*config, "pressure"))
  {
    Serial.printf("      Pressure: %0.2f\n", message.pressure_reading / 10.0f);
    publishValue(valueTopic(*config, "pressure"), String(message.pressure_reading / 10.0f, 2));
  }
  if (message.light_reading != NO_SENSOR_VALUE && hasSensor(*config, "light"))
  {
    Serial.printf("        Light: %d\n", message.light_reading);
    publishValue(valueTopic(*config, "light"), String(message.light_reading));
  }
  Serial.println();
}

void connectWifi() {

  if (WiFi.status() == WL_CONNECTED || wifiSsid.isEmpty()) return;
  Serial.println("Connecting to Wifi");

  WiFi.mode(WIFI_STA);
  if(!hostName.isEmpty()) 
  {
    Serial.printf("Setting hostname to %s\n", hostName);
    WiFi.setHostname(hostName.c_str());
  }
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  while (WiFi.status() != WL_CONNECTED) delay(250);
  Serial.println("Connected to Wifi");
  Serial.printf("IP Address is: %s\n", WiFi.localIP().toString());
  if(!hostName.isEmpty()) 
  {
    MDNS.begin(hostName.c_str());
  }
}

void connectMqtt() {
  if (mqttClient.connected() || millis() - lastMqttAttempt < 5000) return;
  Serial.println("Connecting to MQTT");

  lastMqttAttempt = millis();
  String statusTopic = mqttBaseTopic + "/status";
  bool connected = mqttUsername.isEmpty()
      ? mqttClient.connect(mqttDeviceId.c_str(), statusTopic.c_str(), 1, true, "OFFLINE")
      : mqttClient.connect(mqttDeviceId.c_str(), mqttUsername.c_str(), mqttPassword.c_str(),
                           statusTopic.c_str(), 1, true, "OFFLINE");
  if (connected) {
    Serial.println("Connected to MQTT");
    publishValue(statusTopic, "ONLINE");
    publishDiscovery();
  }else {
    Serial.println("Not Connected to MQTT");

  }
}

void setupOTA() {
  Serial.println("Setting up OTA.");
  ArduinoOTA.setHostname(mqttDeviceId.c_str());
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("OTA update starting"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA update complete"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA error: %u\n", error); });
  ArduinoOTA.begin();
}

void setupRadio() {
  if (!radio.begin()) {
    Serial.println("RF24 chip not detected");
    while (true) delay(1000);
  }
  Serial.println("RF25 chip detected, setting up radio network.");
  network.begin(radioChannel, radioNode);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_MAX);
  radio.setRetries(15, 15);
  radio.setAutoAck(true);

  // RF24 IRQ is active-low, open-drain; only wake on RX_DR (data ready).
  pinMode(RADIO_INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RADIO_INT_PIN), onRadioIrq, FALLING);
  radio.maskIRQ(true, true, false);
}

// Lets the CPU (and Wi-Fi modem) automatically light-sleep whenever idle,
// waking immediately when data arrives on the RF24 IRQ line.
void setupPowerSaving() {
  #ifdef USE_SLEEP
  gpio_wakeup_enable(static_cast<gpio_num_t>(RADIO_INT_PIN), GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  esp_pm_config_t pmConfig = {};
  pmConfig.max_freq_mhz = 240;
  pmConfig.min_freq_mhz = 80;
  pmConfig.light_sleep_enable = true;
  if (esp_pm_configure(&pmConfig) != ESP_OK) {
    Serial.println("Failed to enable automatic light sleep");
  }

  WiFi.setSleep(true);
  #endif
}

void processRadio() {
  network.update();
  while (network.available()) {
    RF24NetworkHeader header;
    network.peek(header);
    if (header.type == MESSAGE_SENSOR) {
      SensorMessage message;
      network.read(header, &message, sizeof(message));
      publishReading(message);
    } else {
      network.read(header, nullptr, 0);
    }
  }
}

void setupLocalSensors() {
  Wire.begin();

  dallasSensors.begin();
  dallasReady = dallasSensors.getDeviceCount() > 0;
  Serial.println(dallasReady ? "DS18B20 detected" : "DS18B20 not detected");

  sht4Ready = sht4.begin(&Wire);
  if (sht4Ready) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
  }
  Serial.println(sht4Ready ? "SHT4x detected" : "SHT4x not detected");

  bmp280Ready = bmp280.begin(BMP280_ADDRESS_ALT) || bmp280.begin(BMP280_ADDRESS);
  if (bmp280Ready) {
    bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
                       Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
                       Adafruit_BMP280::STANDBY_MS_500);
  }
  Serial.println(bmp280Ready ? "BMP280 detected" : "BMP280 not detected");
}

void readLocalSensors() {
  SensorMessage message{};
  message.location = LOCAL_SENSOR_NETWORK_ID;

  if (dallasReady) {
    dallasSensors.requestTemperatures();
    float tempC = dallasSensors.getTempCByIndex(0);
    if (tempC != DEVICE_DISCONNECTED_C) {
      message.temperature_reading = static_cast<uint16_t>(tempC * 1000.0f + 0.5f);
    }
  }

  if (sht4Ready) {
    sensors_event_t humidityEvent, tempEvent;
    if (sht4.getEvent(&humidityEvent, &tempEvent)) {
      message.humidity_reading = static_cast<uint16_t>(humidityEvent.relative_humidity * 100.0f + 0.5f);
    }
  }

  if (bmp280Ready) {
    float pressureHpa = bmp280.readPressure() / 100.0f;
    if (!isnan(pressureHpa)) {
      message.pressure_reading = static_cast<uint16_t>(pressureHpa * 10.0f + 0.5f);
    }
  }

  publishReading(message);
}

void setup() {
  Serial.begin(115200);
  delayMicroseconds(3000);
  Serial.println("Starting!...");
  loadConfig();
  connectWifi();
  setupOTA();
  mqttClient.setServer(mqttHost.c_str(), mqttPort);
  setupRadio();
  setupLocalSensors();
  setupPowerSaving();
}

void loop() {
  radioIrqFlag = false;
  connectWifi();
  ArduinoOTA.handle();
  connectMqtt();
  mqttClient.loop();
  processRadio();

  if (localSensorsInitialRead || millis() - lastLocalReadMillis >= LOCAL_SENSOR_INTERVAL_MS) {
    localSensorsInitialRead = false;
    lastLocalReadMillis = millis();
    readLocalSensors();
  }

  delay(1);
}
