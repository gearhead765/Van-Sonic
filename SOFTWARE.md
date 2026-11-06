# Software Development

## Introduction

This is an ESP32 application to read sensor data from an ultrasonic rangefinder and transmit is using the ESP-NOW protocol to a listener App.
The listener app, Van_Level, displays the rangefinder information on its HTML-based user interface.

The `/setup` page allows configuration of the Access Point (default values are located in [data/APconfig.json](Van_Sonic_App/Van_Sonic/data/APconfig.json), ESP-NOW listener setup, sensor calibration, and firmware updates.

## Setting up the Arduino Environment
There are many great tutorials on the Web guiding your setup of the Arduino/ESP programming enviroment.  Among them are links to Rui & Sara Santos' [Random Nerd Tutorials](https://randomnerdtutorials.com/).  Their pages will give you the necessary background on installing the Arduino IDE, Boards, and Libraries.  If you are not already familiar with these items, I highly recommend giving their tutorials some attention.

### Necessary Libraries
 - [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer)
 - [AsyncTCP](https://github.com/ESP32Async/AsyncTCP)
 - [Arduino_JSON](https://github.com/arduino-libraries/Arduino_JSON)
 - [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA)
 - [LittleFS[(https://github.com/littlefs-project/littlefs)
 
### IDE Enhancements
See the *Random Nerd Tutorial* above for installation instructions
 - [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload)
 
## The Van Sonic Program
Our program sets up a local Access Point with the default SSID `VAN_SONIC` that we will use to configure the AP access.  We have a single event listener to watch for Access Point connection requests and limit to a single connection.

The program actually begins in the `setup()` function where we start by setting up our local file system using LittleFS, and open the file [data/APconfig.json](Van_Level_App/Van_Level/data/APconfig.json) containing our default AP configuration information used to log into the captive Access Point.

At this point in the setup process, there is a 3-second pause to allow the `Boot` button on the ESP board to be pressed.  If pressed, this will erase the saved preferences in EEPROM.  Useful if you have customized the AP information and subsequently forgotten it.

Next, we retrieve our AP configuration and our calibration values from the ESP32's non-volatile EEPROM storage.  This storage area is retained across restarts and power loss.  If there are no saved values, default values are substituted.

With the saved AP information retrieved, or default values substituted, we proceed to configure our HTML query handlers.  Our Web server has only a single configuration page that it delivers to the user; `/Setup`; stored in the [data/](Van_Sonic_App/Van_Sonic/data/) directory. There is a redirect configured to sent `/` requests to `/Setup`.  `Setup` is the page where we can modify the default AP SSID and password, configure the ESP-NO listener, and adjust our calibration values.

With our handlers in place, we establish our AP server, and periodically push our distance data to the ESP-NOW listener.
 
## Programming the ESP32
### Initial Programming
The Xiao ESP32-S3 board can be programmed using a regular USB-C cable.  Once the initial program and LittleFS have been installed the first time, you can then use the application setup page and the Elegant OTA features to remote load your updates Over-The-Air.
### Firmware OTA updates:
Using the Arduino IDE, firmware updatess are created by selecting "Sketch>Export Compiled Binary". The generated file will be saved under your project folder inside a Build folder then a series of other folders. The file with the `.ino.bin` extension is the one you should upload to your board using the ElegantOTA web page.

### LittleFS OTA updates:
Again using the Arduino IDE, you will need the programmer connected, but not the board. Press `[Ctrl]` + `[Shift]` + `[P]` to open the command palette. Select `Upload Little FS to Pico/ESP8266/ESP32`. You'll get an error because there isn't any ESP32 board connected – don't worry, it will still create a .bin file from the data folder.  In the debugging window you'll see the `.littlefs.bin` file location.  Probably in "C:\Users\<username>\AppData\Local\Temp\".
  