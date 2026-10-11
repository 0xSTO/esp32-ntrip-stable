# esp32-ntrip-stable
Stable dual-caster NTRIP base station firmware for ESP32 (fork of [esp32-ntrip-DUO](https://github.com/incarvr6/esp32-ntrip-DUO), imported at upstream commit `28ef71b`).

This fork fixes the dropped streams (rtk2go "restarts") caused by blocking socket writes in the shared event
loop, plus a set of WiFi, watchdog and memory-safety fixes. See [STABILITY_CHANGES.md](STABILITY_CHANGES.md).

Target: classic ESP32 (e.g. ESP32-WROOM-32U), root `main/` tree, ESP-IDF v5.4.2:

```sh
idf.py build                 # firmware
test/host/run_tests.sh       # host unit tests
```

The prebuilt `.bin` files and the `esp32s3test` tree below are upstream's and do not include these fixes.

---

## Upstream: ESP32 NTRIP Duo
This is modified version of ESP32 xbee.

Main difference is that this have two NTRIP servers that can be running at the same time to be able to feed Onocoy and RTK Direct!!!

ESP32 NTRIP Duo is made with [ESP-IDF](https://github.com/espressif/esp-idf). Its main function is to forward the UART of the ESP32 to a variety of protocols over WiFi.

In this version Installation is simplified, with just a single bin file. You can use ESPHome web flasher https://web.esphome.io/ just connect your ESP32 dev board to PC, select connect, cho0se correct COM port, connect and install. On popup choose bin file and click install.

This software can run on ESP32 WROOM type. Now added ESP32S3, ESP32C3, ESP32S2  Just choose correct bin file!
For ESP32S2 if ESPHome is not connecting (it didn't with S2mini) You can use https://tasmota.github.io/install/ to install with single bin file.
## Features
- WiFi Station
- WiFi Hotspot
- Web Interface
- UART configuration
- Two NTRIP Servers


## Help
Now It can be compiled using ESP-IDF 5.4. (with some depreciation comments)

To install the latest firmware use ESPHome web Flasher https://web.esphome.io/

Here is installation video https://youtu.be/33Mu5EV7fOE?si=J6kwCt6bbmIu7HnS

It is still work in progress!!!

## Pinout
By default it is set for UART0 TX gpio1, RX gpio3 including ESP32S3

LED with common positive and low output:

ESP32: gpio21 Red, gpio22 Green, gpio23 Blue

ESP32S3: gpio4 Red, gpio5 Green, gpio6 Blue
![IMG_20250212_000825](https://github.com/user-attachments/assets/f17d28dc-4bc7-4647-8311-7a1c44526d17)
