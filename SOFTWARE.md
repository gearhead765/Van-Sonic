# Software Development

## Introduction

This is an ESP32 application to read sensor data from an ultrasonic rangefinder and transmit is using the ESP-NOW protocol to a listener App. The listener app, [Van_Level](https://github.com/gearhead765/Van-Level), displays the rangefinder information on its HTML-based user interface.

The `/setup` page allows configuration of the access point, Wi-Fi network, ESP-NOW listener, sensor calibration, and firmware updates.

On the Network tab, scan for your router, select its SSID, enter its WPA password (8-63 characters), and choose DHCP or static addressing. Apply saves the settings and starts connecting without a reboot. The Status tab shows the saved network, local IP, connection state, and current channel. The setup AP stays available while the station reconnects automatically, with another attempt every 10 seconds while disconnected.

Connect `Van_Sonic` and `Van_Level` to the same 2.4 GHz access point/channel. ESP-NOW follows the current WiFi channel automatically. A shared SSID alone does not ensure matching channels on mesh or multi-AP networks. Keep the receiver MAC set to the STA MAC shown by Van_Level. Without a router connection, automatic channel alignment between the devices is not guaranteed.

## Setting up the Arduino Environment
There are many great tutorials on the Web guiding your setup of the Arduino/ESP programming enviroment.  Among them are links to Rui & Sara Santos' [Random Nerd Tutorials](https://randomnerdtutorials.com/).  Their pages will give you the necessary background on installing the Arduino IDE, Boards, and Libraries.  If you are not already familiar with these items, I highly recommend giving their tutorials some attention.

### Necessary Libraries
- [Arduino_JSON](https://github.com/arduino-libraries/Arduino_JSON)
- [AsyncTCP](https://github.com/ESP32Async/AsyncTCP)
- [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA)  -  Note: Be sure to enable async mode in `ElegantOTA`.[^1]

[^1]: Set ELEGANTOTA_USE_ASYNC_WEBSERVER to 1 in ElegantOTA.h (or via build flags) before compiling.

- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer)
- [LittleFS](https://github.com/littlefs-project/littlefs)

### IDE Enhancements
See the *Random Nerd Tutorial* above for installation instructions
 - [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload)
 
## The Van Sonic Program
During `setup()`, the firmware mounts LittleFS for web pages and static assets. It uses saved access point settings when present; otherwise, it uses the SSID `VanSonic_nnnn` (the last four hexadecimal digits of the device's factory MAC address), password `12345678`, and IP address `192.168.4.1`.

At this point in the setup process, there is a 3-second pause to allow the `Boot` button on the ESP board to be pressed.  If pressed, this will erase the saved preferences in EEPROM.  Useful if you have customized the AP information and subsequently forgotten it.

Next, we retrieve our AP configuration and our calibration values from the ESP32's non-volatile EEPROM storage.  This storage area is retained across restarts and power loss.  If there are no saved values, default values are substituted.

With the saved AP information retrieved, or default values substituted, we configure our HTML query handlers. The authenticated `/setup` page is stored in the [data/](Van_Sonic_App/Van_Sonic/data/) directory. It configures AP credentials, router access, the ESP-NOW listener, and calibration. The server serves [index.html](Van_Sonic_App/Van_Sonic/data/index.html) from LittleFS at `/` and `/index.html`. Image serving and the `/events` event stream remain registered; the homepage polls `/readings` rather than using sensor events.

The homepage displays the current local time and the ultrasonic sensor's last successful reading, including its calibrated value, units, and reading time. A successful reading is a checksum-valid sensor frame, independent of ESP-NOW delivery. The last successful value remains available if the sensor stops reporting; readings are cleared at restart. Before the first reading, the page displays an empty state. The public, uncached `/readings` endpoint supplies a snapshot and its age using the ESP32's monotonic clock. The browser polls once per second and estimates the reading timestamp from that age and its own local clock, so no NTP connection is needed. During connection loss, the page retains its displayed readings and retries automatically.

The UART is drained continuously with an independent frame buffer. The default sensor output rate is 10 Hz; ESP-NOW transmissions are limited to 150 ms and use the newest complete reading.  [ESPNow Data Structure](<Data/ESPNow Data Structure.txt>) describes the common data packet.

- Sensor ID `1` uses Serial1: sensor TX to ESP32 `D5` (`RX_PIN`), sensor RX to `D6` (`TX_PIN`).
  >pins were reassigned from [defaults](Notes/XIAO_ESP32-S3_front_pinout.png) to make PCB routing easier
  
  The sensor is supplied with +5V directly from the DC-DC converter. Change the pin definitions if your wiring differs. The selected ESP32 board must provide at least one UART in addition to the debug console.
- Set the receiver MAC on `/setup` to match the MAC address of the Van-Level app.
- Each measurement frame consists of 4 bytes:
  >Byte 0 (0xFF): Header/Start byte.  
  >Byte 1 (High Byte): Upper 8 bits of the distance value.  
  >Byte 2 (Low Byte): Lower 8 bits of the distance value.  
  >Byte 3 (Sum Check): Checksum calculated as (Byte 0 + Byte 1 + Byte 2) & 0x00FF.
  
- Communication Protocol: UART (TTL serial, 9600 bps baud rate, 8 data bits, no parity, 1 stop bit).
- See [A02YYUW Datasheet](Notes/A02YYUW-Datasheet.pdf), [Output Info](Notes/A02YYUW_OutputInfo.md), and [sample Arduino code](Notes/ArduinoSampleCode.txt).

Each sensor section also displays the latest ESP-NOW send result and the time of its last successful send. Results distinguish an unconfigured receiver, no sends yet, an initial send in progress, success, and failure. Success is recorded only when the ESP-NOW completion callback reports delivery, not when a packet is merely queued; immediate queue errors count as failures. 

Later failures do not erase the last-success time. Sends are serialized until their completion callbacks arrive. Send timestamps use the same monotonic-age/browser-local-clock calculation as reading timestamps, and send history is cleared at restart. ESP-NOW delivery success is not confirmation that Van_Level processed the reading.

With our handlers in place, we run in AP+STA mode, connect to the saved router, and periodically push distance data to the ESP-NOW listener.
 
## Programming the ESP32
### Initial Programming
The Xiao ESP32-S3 board can be programmed using a regular USB-C cable.  Once the initial program and LittleFS have been installed the first time, you can then use the application setup page and the Elegant OTA features to remote load your updates Over-The-Air.
### Firmware OTA updates:
Using the Arduino IDE, firmware updatess are created by selecting "Sketch>Export Compiled Binary". The generated file will be saved under your project folder inside a Build folder then a series of other folders. The file with the `.ino.bin` extension is the one you should upload to your board using the ElegantOTA web page.

### LittleFS OTA updates:
Again using the Arduino IDE, you will need the programmer connected, but not the board. Press `[Ctrl]` + `[Shift]` + `[P]` to open the command palette. Select `Upload Little FS to Pico/ESP8266/ESP32`. You'll get an error because there isn't any ESP32 board connected – don't worry, it will still create a .bin file from the data folder.  In the debugging window you'll see the `.littlefs.bin` file location.  Probably in "C:\Users\<username>\AppData\Local\Temp\".
  