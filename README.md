## Introduction
This project utilizes an Adafruit Feather ESP32 V2 development board
and Arduino development environment to publish VOC sensor readings for a Sensirion
SGP40 VOC sensor to an MQTT topic. It utilizes the 'Auto-discovery' feature of the Home Assistant
MQTT server by publishing a JSON payload defining the sensor data to be published. This way, the sensor will automatically appear in the MQTT section of Home Assistant without any further configuration.

Initial commit tested with HAOS 18.3 and Core 2026.10.4.

## Required Software

* `arduino-cli`: Install from your package manager

## Required Libraries

Required libraries (install via Arduino Library Manager or `arduino-cli lib install`):
- `PubSubClient` — MQTT client
- `ArduinoJson` — JSON serialization

These can be installed with `arduino-cli library install <name>` using these exact names above

## Required Hardware
* Sensirion [SGP40 breakout](https://www.digikey.com/en/products/detail/sparkfun-electronics/18345/14565027) from Sparkfun/Digikey
* Adafruit [Feather ESP32 V2](https://www.digikey.com/en/products/detail/adafruit-industries-llc/5400/16129667)

## Setting up WiFi Credentials

I encrypted the WiFi password so that it is not available in plaintext in the firmware binary. To provide your WiFi credentials, the SSID is in plaintext but the password requires encryption. Here is how to encrypt and format your password for the firmware:

```
~$ KEY=$(openssl rand -hex 32)
~$ IV=$(openssl rand -hex 16)
~$ echo -n "yourpasswordhere" | \
  openssl enc -aes-256-cbc \
    -K "$KEY" \
    -iv "$IV" \
    -nosalt \
    -out ciphertext.bin
```

Then in `secrets.h` define using your `KEY` and `IV` from above:
```
#define WIFI_PASSWORD_HEX_KEY "aabbccddeeffaabbccddeeffaabbccddeeffaabbccddeeffaabbccddeeffaabb"
#define WIFI_PASSWORD_CIPHERTEXT { \
  0x0c, 0x91, 0xa3, 0xee, 0xf7, 0xe1, 0x08, 0x02, 0xc1, 0x9a, 0xdd, 0x29, \
  0x12, 0x65, 0xc5, 0xaf, 0x29, 0xaa, 0xa8, 0xc5, 0x11, 0x03, 0xe1, 0x17, \
  0x7d, 0x88, 0xbb, 0x03, 0x0a, 0x05, 0xd5, 0xff \
}
```