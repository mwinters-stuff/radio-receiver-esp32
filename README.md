# Radio Receiver ESP32

An ESP32 (nodemcu-32s) firmware that acts as the base station for a network of
battery-powered [nRF24L01](https://github.com/nRF24/RF24) sensor nodes. It
receives temperature/humidity/battery/pressure/light readings over an
[RF24Network](https://github.com/nRF24/RF24Network) mesh, republishes them to
MQTT, and auto-registers each sensor with Home Assistant via
[MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery).

The board also reads its own local sensors (DS18B20, SHT4x, BMP280) and
publishes them the same way, so it can double as a sensor node for the room
it's installed in.

## Reference Repo
[sensor-net](https://github.com/mwinters-stuff/sensor-net) has a big history of
code that this is derived from. Look in there for things like a simple battery
powered station.

## Features

- Receives sensor readings from remote RF24 nodes over an RF24Network mesh.
- Reads local sensors: DS18B20 (temperature, OneWire), SHT4x (temperature +
  humidity, I2C), BMP280 (pressure, I2C).
- Publishes readings to MQTT and auto-generates Home Assistant discovery
  configs for each enabled location/sensor combination.
- Per-location configuration (name, area, which sensors to expect) loaded from
  a YAML file on the device's LittleFS filesystem — no rebuild needed to
  add/rename/enable a location.
- Firmware and filesystem updates over USB serial or Wi-Fi (ArduinoOTA).
- Optional wired Ethernet (W5500) support as an alternative to Wi-Fi.
- Automatic light-sleep / CPU frequency scaling to reduce power draw between
  radio packets.
- Telnet/serial debug logging via [RemoteDebug](https://github.com/JoaoLopesF/RemoteDebug).

## Hardware

Built for a `nodemcu-32s` (ESP32) board. Default pin assignments (see
[src/main.cpp](src/main.cpp)):

| Function                | Pin |
| ------------------------ | --- |
| RF24 CE                  | 16  |
| RF24 CSN                 | 5   |
| RF24 IRQ                 | 27  |
| DS18B20 (OneWire)         | 4   |
| W5500 CS (optional)       | 17  |
| W5500 Reset (optional)    | 33  |
| W5500 SCK (optional)      | 14  |
| W5500 MISO (optional)     | 25  |
| W5500 MOSI (optional)     | 13  |

SHT4x and BMP280 use the default I2C bus (`Wire.begin()`). Ethernet support is
disabled by default; uncomment `#define USE_ETHERNET` at the top of
[src/main.cpp](src/main.cpp) to use a W5500 module instead of Wi-Fi.

## Building and flashing

This is a [PlatformIO](https://platformio.org/) project with two environments:

- `nodemcu-32s` — flash firmware and filesystem over USB serial (default).
- `nodemcu-32s-ota` — flash firmware and filesystem over Wi-Fi using
  ArduinoOTA, once the device is already running and on the network.

```sh
# First flash, over USB:
pio run -e nodemcu-32s -t uploadfs   # uploads data/config.yaml to LittleFS
pio run -e nodemcu-32s -t upload

# Subsequent updates, over Wi-Fi (device must already be online):
pio run -e nodemcu-32s-ota -t uploadfs
pio run -e nodemcu-32s-ota -t upload
```

The OTA environment uploads to `sensor-net.local` (mDNS) by default — update
`upload_port` in [platformio.ini](platformio.ini) if you rename the device's
`hostname`. OTA is password-protected; the password is set via `OTA_PASSWORD`
in [src/main.cpp](src/main.cpp) (default `ota`) and must match
`upload_flags = --auth=...` in [platformio.ini](platformio.ini).

## Configuration

All runtime configuration (Wi-Fi credentials, MQTT broker, radio settings, and
sensor locations) lives in `data/config.yaml`, which is uploaded to the
device's LittleFS filesystem — it is **not** compiled into the firmware.

1. Copy the template and fill in your own values:

   ```sh
   cp data/config.yaml.example data/config.yaml
   ```

2. Edit `data/config.yaml` (see reference below).
3. Upload the filesystem image: `pio run -e nodemcu-32s -t uploadfs`.

`data/config.yaml` is listed in [.gitignore](.gitignore) and must never be
committed — it contains your Wi-Fi and MQTT passwords. Keep
`data/config.yaml.example` up to date with placeholder values instead.

### Reference

```yaml
wifi:
  ssid: YOUR_WIFI_SSID
  password: YOUR_WIFI_PASSWORD
  hostname: sensor-net          # mDNS/OTA hostname, and used to derive the MQTT client id

radio:
  channel: 0x47                 # RF24 channel, must match your sensor nodes
  node: 0                       # RF24Network address of this base station (usually 0, the root)

mqtt:
  enabled: true
  host: mqtt://192.168.10.156:1883
  username: YOUR_MQTT_USERNAME
  password: YOUR_MQTT_PASSWORD
  base_topic: sensor-net
  device_id: sensor-net         # used as the MQTT client id and unique_id prefix
  device_name_prefix: Sensor Net

locations:
  - network_id: 1002            # RF24Network address of the remote sensor node
    id: lounge                  # used to build MQTT topics/unique_ids, must be unique
    enabled: true
    name: Lounge                # Home Assistant entity name
    area: Lounge                # Home Assistant suggested area
    sensors:
      - temperature
      - battery
  - network_id: 2000            # reserved network_id for this board's own local sensors
    id: office
    enabled: true
    name: Office
    area: Office
    sensors:
      - temperature
      - humidity
      - pressure
```

- Each entry under `locations` describes one physical sensor location and
  which readings to expect from it. `network_id: 2000` is reserved for the
  base station's own local sensors (DS18B20/SHT4x/BMP280).
- Valid `sensors` values: `temperature`, `humidity`, `battery`, `pressure`,
  `light`. Only sensors listed here get a Home Assistant discovery entity and
  have their readings published.
- Set `enabled: false` to keep a location defined but stop publishing
  discovery/readings for it (e.g. a decommissioned sensor node).

### Home Assistant discovery

On MQTT connect (and every hour after), the firmware publishes a retained
discovery config to `homeassistant/sensor/<device_id>/<location id>-<sensor>/config`
for every enabled sensor, and readings to
`<device_id>/<location id>/<sensor>`. Availability is published as
`ONLINE`/`OFFLINE` retained on `<base_topic>/status`. All locations are
grouped under a single Home Assistant device named after
`device_name_prefix`.

## Debugging

Serial output is available at 115200 baud. The same log stream is also
available over telnet via RemoteDebug once the device is on the network
(default port 23) — connect with `telnet <hostname>.local` and set the log
level with `v`/`vv`/etc.
