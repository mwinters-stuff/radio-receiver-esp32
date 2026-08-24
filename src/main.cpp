// #define USE_ETHERNET

#include <Adafruit_BMP280.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_Sensor.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <DallasTemperature.h>
#ifdef USE_ETHERNET
#include <ETH.h>
#endif
#include <LittleFS.h>
#include <OneWire.h>
#include <PubSubClient.h>
#include <RF24.h>
#include <RF24Network.h>
#include <RemoteDebug.h>
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
#define _DEBUG_DISCOVERY

#define RADIO_CE_PIN 16
#define RADIO_CSN_PIN 5
#define RADIO_INT_PIN 27
#define ONE_WIRE_PIN 4
#define ETHERNET_CS_PIN 17
#define ETHERNET_IRQ_PIN -1
#define ETHERNET_RESET_PIN 33
#define ETHERNET_SCK_PIN 14
#define ETHERNET_MISO_PIN 25
#define ETHERNET_MOSI_PIN 13

#define OTA_PASSWORD "ota"

constexpr uint16_t NO_SENSOR_VALUE = UINT16_MAX;
constexpr uint8_t MESSAGE_SENSOR = 'R';
constexpr const char *CONFIG_PATH = "/config.yaml";

// Network ID for sensors read directly from this board's local peripherals.
constexpr uint16_t LOCAL_SENSOR_NETWORK_ID = 2000;
constexpr uint32_t LOCAL_SENSOR_INTERVAL_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t MQTT_DISCOVERY_INTERVAL_MS = 60UL * 60UL * 1000UL;

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
#ifdef USE_ETHERNET
NetworkClient ethernetClient;
#endif
PubSubClient mqttClient(wifiClient);
RemoteDebug Debug;
uint32_t lastMqttAttempt = 0;
uint32_t lastDiscoveryPublish = 0;
#ifdef USE_ETHERNET
bool ethernetInitialized = false;
bool ethernetReady = false;
SPIClass hspi(HSPI);
#endif

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
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Debug.println("LittleFS mount failed, using build-flag defaults");
    return;
  }

  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    Debug.printf("%s not found, using build-flag defaults\n", CONFIG_PATH);
    return;
  }

  Debug.printf("Loading %s\n", file.name());

  YAMLNode root = YAMLNode::loadStream(file);
  file.close();
  if (root.isNull()) {
    Debug.printf("Failed to parse %s\n", CONFIG_PATH);
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

  Debug.printf("Loaded %s: %u locations\n", CONFIG_PATH, static_cast<unsigned>(sensorConfigs.size()));
}

String valueTopic(const SensorConfig &config, const char *name) {
  return mqttDeviceId + "/" + config.id + "/" + name;
}

void publishValue(const String &topic, const String &value) {
  if (!mqttClient.connected()) return;
  if (!mqttClient.publish(topic.c_str(), value.c_str(), true))
    Debug.printf("MQTT publish failed (%u bytes): %s\n", static_cast<unsigned>(value.length()), topic.c_str());
}

void printPrettyJson(const String &json) {
  JsonDocument document;
  DeserializationError error = deserializeJson(document, json);
  if (error) {
    Debug.printf("JSON logging failed: %s\n", error.c_str());
    return;
  }
  serializeJsonPretty(document, Debug);
  Debug.println();
}

void publishDiscovery(const SensorConfig &config, const char *name,
                      const char *unit, const char *deviceClass) {
  String topic = String("homeassistant/sensor/") + mqttDeviceId + "/" +
                 config.id + "-" + name + "/config";
  JsonDocument document;
  document["unique_id"] = mqttDeviceId + "-" + config.id + "-" + name;
  document["name"] = config.name;
  document["state_topic"] = valueTopic(config, name);
  document["unit_of_measurement"] = unit;
  document["state_class"] = "measurement";
  document["force_update"] = true;
  document["device_class"] = deviceClass;
  JsonObject device = document["device"].to<JsonObject>();
  device["identifiers"] = mqttDeviceId;
  device["suggested_area"] = config.area;
  device["name"] = mqttDeviceName;
  document["availability_topic"] = mqttBaseTopic + "/status";
  document["payload_available"] = "ONLINE";
  document["payload_not_available"] = "OFFLINE";

  String payload;
  serializeJson(document, payload);

  Debug.printf("Discovery: %s\n", topic.c_str());
  #ifdef DEBUG_DISCOVERY
    printPrettyJson(payload);
  #endif

  publishValue(topic, payload);
}

void publishDiscovery() {
  for (const SensorConfig &config : sensorConfigs) {
    if (!config.enabled) continue;
    if (hasSensor(config, "temperature")) publishDiscovery(config, "temperature", "°C", "temperature");
    if (hasSensor(config, "humidity")) publishDiscovery(config, "humidity", "%", "humidity");
    if (hasSensor(config, "battery")) publishDiscovery(config, "battery", "V", "voltage");
    if (hasSensor(config, "pressure")) publishDiscovery(config, "pressure", "hPa", "pressure");
    if (hasSensor(config, "light")) publishDiscovery(config, "light", "lx", "illuminance");
  }
}

void publishReading(const SensorMessage &message) {
  const SensorConfig *config = findConfig(message.location);
  if (!config || !config->enabled || !mqttClient.connected()) return;

  Debug.printf("Sending %s\n", config->name);

  if (message.temperature_reading != NO_SENSOR_VALUE && hasSensor(*config, "temperature"))
  {
    Debug.printf("   Temperature: %0.2f\n", message.temperature_reading / 1000.0f);
    publishValue(valueTopic(*config, "temperature"), String(message.temperature_reading / 1000.0f, 2));
  }
  if (message.humidity_reading != NO_SENSOR_VALUE && hasSensor(*config, "humidity"))
  {
    Debug.printf("      Humidity: %0.2f\n", message.humidity_reading / 100.0f);
    publishValue(valueTopic(*config, "humidity"), String(message.humidity_reading / 100.0f, 2));
  }
  if (message.voltage_reading != NO_SENSOR_VALUE && hasSensor(*config, "battery"))
  {
    Debug.printf("       Battery: %0.2f\n", message.voltage_reading / 1000.0f);
    publishValue(valueTopic(*config, "battery"), String(message.voltage_reading / 1000.0f, 2));
  }
  if (message.pressure_reading != NO_SENSOR_VALUE && hasSensor(*config, "pressure"))
  {
    Debug.printf("      Pressure: %0.2f\n", message.pressure_reading / 10.0f);
    publishValue(valueTopic(*config, "pressure"), String(message.pressure_reading / 10.0f, 2));
  }
  if (message.light_reading != NO_SENSOR_VALUE && hasSensor(*config, "light"))
  {
    Debug.printf("        Light: %d\n", message.light_reading);
    publishValue(valueTopic(*config, "light"), String(message.light_reading));
  }
  Debug.println();
}

void connectWifi() {
#ifdef USE_ETHERNET
  if (ETH.linkUp()) return;
  if (ethernetInitialized) return;

  Debug.println("Connecting to Ethernet");
// 1. Force both Chip Selects HIGH so neither chip interferes on startup
  pinMode(ETHERNET_CS_PIN, OUTPUT);
  digitalWrite(ETHERNET_CS_PIN, HIGH);
  
  pinMode(RADIO_CSN_PIN, OUTPUT);
  digitalWrite(RADIO_CSN_PIN, HIGH); 

  // 2. Hardware Reset W5500
  pinMode(ETHERNET_RESET_PIN, OUTPUT);
  digitalWrite(ETHERNET_RESET_PIN, LOW);
  delay(100);
  digitalWrite(ETHERNET_RESET_PIN, HIGH);
  delay(200);

  // 3. Explicitly initialize HSPI with custom pins FIRST
  if(!hspi.begin(ETHERNET_SCK_PIN, ETHERNET_MISO_PIN, ETHERNET_MOSI_PIN, ETHERNET_CS_PIN))
  {
    Debug.println("Initalise HSPI Failed");
    return;
  }

  ethernetInitialized = true;
  if (!ETH.begin(ETH_PHY_W5500, 1, ETHERNET_CS_PIN, ETHERNET_IRQ_PIN,
                 ETHERNET_RESET_PIN, SPI2_HOST, ETHERNET_SCK_PIN,
                 ETHERNET_MISO_PIN, ETHERNET_MOSI_PIN, 14)) {
    Debug.println("Ethernet initialization failed");
    return;
  }
  ETH.setHostname(hostName.isEmpty() ? "sensor-net" : hostName.c_str());
  while (!ETH.linkUp()) delay(250);
  mqttClient.setClient(ethernetClient);
  ethernetReady = true;
  Debug.begin(hostName.isEmpty() ? "sensor-net" : hostName);
  Debug.setSerialEnabled(true);
  Debug.println("Connected to Ethernet");
  Debug.printf("IP Address is: %s\n", ETH.localIP().toString());
  if (!hostName.isEmpty()) MDNS.begin(hostName.c_str());
  return;
#else

  if (WiFi.status() == WL_CONNECTED || wifiSsid.isEmpty()) return;
  Debug.println("Connecting to Wifi");

  WiFi.mode(WIFI_STA);
  if(!hostName.isEmpty()) 
  {
    Debug.printf("Setting hostname to %s\n", hostName);
    WiFi.setHostname(hostName.c_str());
  }
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  while (WiFi.status() != WL_CONNECTED) delay(250);
  Debug.println("Connected to Wifi");
  Debug.begin(hostName.isEmpty() ? "sensor-net" : hostName);
  Debug.setSerialEnabled(true);
  Debug.printf("Connected to Wifi\n");
  Debug.printf("IP Address is: %s\n", WiFi.localIP().toString());
  if(!hostName.isEmpty()) 
  {
    MDNS.begin(hostName.c_str());
  }
#endif
}

void connectMqtt() {
#ifdef USE_ETHERNET
  if (!ethernetReady) return;
#endif
  if (mqttClient.connected() || millis() - lastMqttAttempt < 5000) return;
  Debug.println("Connecting to MQTT");

  lastMqttAttempt = millis();
  String statusTopic = mqttBaseTopic + "/status";
  bool connected = mqttUsername.isEmpty()
      ? mqttClient.connect(mqttDeviceId.c_str(), statusTopic.c_str(), 1, true, "OFFLINE")
      : mqttClient.connect(mqttDeviceId.c_str(), mqttUsername.c_str(), mqttPassword.c_str(),
                           statusTopic.c_str(), 1, true, "OFFLINE");
  if (connected) {
    Debug.println("Connected to MQTT");
    publishValue(statusTopic, "ONLINE");
    publishDiscovery();
    lastDiscoveryPublish = millis();
  }else {
    Debug.println("Not Connected to MQTT");

  }
}

void setupOTA() {
  Debug.println("Setting up OTA.");
  ArduinoOTA.setHostname(mqttDeviceId.c_str());
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Debug.println("OTA update starting"); });
  ArduinoOTA.onEnd([]() { Debug.println("OTA update complete"); });
  ArduinoOTA.onError([](ota_error_t error) { Debug.printf("OTA error: %u\n", error); });
  ArduinoOTA.begin();
}

void setupRadio() {
  if (!radio.begin()) {
    Debug.println("RF24 chip not detected");
    while (true) delay(1000);
  }
  Debug.println("RF25 chip detected, setting up radio network.");
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
    Debug.println("Failed to enable automatic light sleep");
  }

#ifndef USE_ETHERNET
  WiFi.setSleep(true);
#endif
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
  Debug.println(dallasReady ? "DS18B20 detected" : "DS18B20 not detected");

  sht4Ready = sht4.begin(&Wire);
  if (sht4Ready) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
  }
  Debug.println(sht4Ready ? "SHT4x detected" : "SHT4x not detected");

  bmp280Ready = bmp280.begin(BMP280_ADDRESS_ALT) || bmp280.begin(BMP280_ADDRESS);
  if (bmp280Ready) {
    bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
                       Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
                       Adafruit_BMP280::STANDBY_MS_500);
  }
  Debug.println(bmp280Ready ? "BMP280 detected" : "BMP280 not detected");
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
  Debug.setSerialEnabled(true);
  Debug.setResetCmdEnabled(true);
  Debug.println("Starting!");
  loadConfig();
  connectWifi();
  setupOTA();
  mqttClient.setBufferSize(1024); // HA discovery payloads exceed the 256-byte default
  mqttClient.setServer(mqttHost.c_str(), mqttPort);
  setupRadio();
  setupLocalSensors();
  setupPowerSaving();
}

void loop() {
  radioIrqFlag = false;
  connectWifi();
  Debug.handle();
  ArduinoOTA.handle();
  connectMqtt();
  mqttClient.loop();
  if (mqttClient.connected() && millis() - lastDiscoveryPublish >= MQTT_DISCOVERY_INTERVAL_MS) {
    publishDiscovery();
    lastDiscoveryPublish = millis();
  }
  processRadio();

  if (localSensorsInitialRead || millis() - lastLocalReadMillis >= LOCAL_SENSOR_INTERVAL_MS) {
    localSensorsInitialRead = false;
    lastLocalReadMillis = millis();
    readLocalSensors();
  }

  delay(1);
}
