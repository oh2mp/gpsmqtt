# gpsmqtt
An ESP8266 based GPS tracker with MQTT

A Web-configurable device with GPS receiver which sends location data to a MQTT broker. In my own configuration I have Mosquitto as a broker and InfluxDB + Telegraf with MQTT plugin. See an [example configurations](https://github.com/oh2mp/esp32_ble2mqtt/blob/main/CONFIG_EXAMPLES.md) from my another repository.

The data is sent as JSON of this format:

`{"la":%.6f,"lo":%.6f,"kmh":%.0f,"cou":%.0f,"alt":%.0f}`

Where la = latitude, lo = longitude, kmh = kilometers per hour, cou = course as degrees and alt = altitude in meters

## Smart beaconing
The device uses SmartBeaconing™ algorithm by Tony Arnerich KD7TA and Steve Bragg KA9MVA to choose when the packets are sent. See https://www.w8wjb.com/qth/QTHHelp/English.lproj/adv-smartbeaconing.html Thanks to [OH2TH](https://github.com/oh2th) for the SmartBeaconing code.

## Extra Libraries needed
Here are the versions I had installed when I compiled this in Arduino IDE 2.3.4

- EspSoftwareSerial 8.1.0
- PubSUbCLient 2.8

I have included here a local copy of Mikal Hart's TinyGPSPlus

## Filesystem
You can use the ESP8266 filesystem uploader tool to upload the contents of data directory. It contains the html pages for the configuring portal and base config files. Or you can just upload the provided image with upload.py:

`/path/to/esp8266/tools/upload.py --chip esp8266 --port /dev/ttyUSB0 --baud 115200 write_flash 0x100000 data.spiffs`
