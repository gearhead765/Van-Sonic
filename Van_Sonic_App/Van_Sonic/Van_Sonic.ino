/*******************************************************************************************
 Van Sonic

    ESP32 application to read sensor data from an ultrasonic distance sensor and transmit
    those reading using the ESP-NOW protocol to a ESP32 receiver app.

    The /setup page allows configuration of the Access Point, sensor calibration, and
    firmware updates.  Default AP credentials are located in the LittleFS data folder at
    /data/APconfig.json.  You can change these if you want, but the preferred method is to
    change the AP credentials after firmware installation using the /setup page.
    If you forget your AP credentials, you can recover the defaults by holding down
    the boot button on the ESP32 for a 5 seconds during startup.

   Over The Air updates are handled by ElegantOTA https://github.com/ayushsharma82/ElegantOTA.

    Firmware OTA updates:
      Firmware .bin files are created by selecting "Sketch>Export Compiled Binary"
      The generated file will be saved under your project folder inside a Build
      folder then a series of other folders. The file with the .ino.bin extension
      is the one you should upload to your board using the ElegantOTA web page.

    LittleFS OTA updates:
      You will need the programmer connected, but not the board.
        Press [Ctrl] + [Shift] + [P] to open the command palette.
        Select "Upload Little FS to Pico/ESP8266/ESP32".
      You'll get an error because there isn't any ESP32 board connected – don't worry,
      it will still create a .bin file from the data folder.  In the debugging window you'll
      see the .littlefs.bin file location.  Probably in "C:\Users\<username>\AppData\Local\Temp\".

*******************************************************************************************/
const char* VERSION = "1.0.0";

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Arduino_JSON.h>
#include <Preferences.h>
#include <ElegantOTA.h>
#include <esp_now.h>

#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
  #define DBG_PRINT(...) Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(...) do {} while (0)
  #define DBG_PRINTLN(...) do {} while (0)
  #define DBG_PRINTF(...) do {} while (0)
#endif

// Comment out to show plain text passwords in serial monitor
#define HIDE_PASSWORDS

// SoftAP configuration
const char * s_configFile = "/APconfig.json"; // SoftAP configuration defaults
String esid;
String epass;
String password;
String myIP;
bool apConnected = false;
JSONVar configJSON;

// EEPROM storage for configuration values
Preferences preferences;

// Calibration variables - reasonable defaults here, configurable on setup page
float distanceOffset = 0.0f;  // offset in cm to account for sensor mounting position

// Timer variables
unsigned long bootStart = 0;  // if non-zero, will restart when bootStart + bootDelay = now
const unsigned long bootDelay = 5000;
unsigned long lastWebTime = 0;      // sensor data webpage push
const unsigned long ESPDelay = 500; // publish readings every half-second
unsigned long lastSensorReadTime = 0;

// Webserver pieces
AsyncWebServer server(80);
AsyncEventSource events("/events");

// The AP stays up at all times so setup is always reachable.

// A02YYUW Ultrasonic Distance Sensor
const uint8_t HEADER_BYTE = 0xFF;
const uint16_t READ_TIMEOUT_MS = 200;
const int16_t ERROR_DISTANCE = -1;
const uint8_t SENSOR_ID = 1;
#define RX_PIN D7
#define TX_PIN D6

// ESP-NOW configuration
typedef struct {
  int16_t distance;
  uint8_t id;
} EspNowMessage;

uint8_t receiverMacAddress[6] = {0, 0, 0, 0, 0, 0};
String receiverMacStr = "";
volatile bool lastSendSuccess = false;
unsigned long lastSendTime = 0;

// Distance filtering (Exponential Moving Average)
float filteredDistance = 0.0f;
float alpha = 0.1f; // setup configurable - 0.01 (heavy smoothing) and 0.9 (light smoothing)

// Read N bytes from Serial1 with timeout
uint8_t readNBytes(uint8_t *buf, size_t len) {
  size_t offset = 0, left = len;
  uint8_t *buffer = buf;
  long curr = millis();
  while (left) {
    if (Serial1.available()) {
      buffer[offset] = Serial1.read();
      offset++;
      left--;
    }
    if (millis() - curr > READ_TIMEOUT_MS) {
      break;
    }
  }
  return offset;
}

// Get distance data from the A02YYUW sensor
// Returns distance in mm (non-negative) on success, -1 on failure
int16_t getDistance(void) {
  uint8_t data[4] = { 0 };
  uint8_t receivedByte = 0;
  unsigned long startTime = millis();

  while (millis() - startTime < READ_TIMEOUT_MS) {
    if (readNBytes(&receivedByte, 1) == 1 && receivedByte == HEADER_BYTE) {
      data[0] = receivedByte;
      if (readNBytes(&data[1], 3) == 3) {
        uint8_t checksum = data[0] + data[1] + data[2];
        if (checksum == data[3]) {
          uint16_t distance = (data[1] << 8) | data[2];
          DBG_PRINT("Distance: ");
          DBG_PRINT(distance);
          DBG_PRINTLN(" mm");
          return distance;
        }
      }
    }
  }
  DBG_PRINTLN("Failed to get distance");
  return ERROR_DISTANCE;
}

// Function to constantly poll the sensor and apply the EMA filter
void updateSensorData() {
  int16_t rawDistance = getDistance();
  
  if (rawDistance >= 0) {
    // valid reading - apply EMA filter
    filteredDistance = (alpha * rawDistance) + ((1.0f - alpha) * filteredDistance);
  }
  // invalid reading - reject and keep previous filtered value
}

// True only if the whole string is a finite number
bool parseStrictFloat(const String &s, float &out) {
  String t = s;
  t.trim();
  if (t.length() == 0) return false;
  char *end;
  float v = strtof(t.c_str(), &end);
  if (*end != '\0' || !isfinite(v)) return false;
  out = v;
  return true;
}

// Reads a request parameter without dereferencing a missing one
bool getParamStr(AsyncWebServerRequest *request, const char *name, bool post, String &out) {
  if (!request->hasParam(name, post)) return false;
  out = request->getParam(name, post)->value();
  return true;
}

// Setup and configuration endpoints use the AP credentials
#define REQUIRE_AUTH(req) if (!(req)->authenticate(esid.c_str(), epass.c_str())) return (req)->requestAuthentication()

// ESP-NOW send status callback
void onEspNowSend(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  lastSendSuccess = (status == ESP_NOW_SEND_SUCCESS);
  lastSendTime = millis();
  if (lastSendSuccess) {
    DBG_PRINTLN("ESP-NOW: Send successful");
  } else {
    DBG_PRINTLN("ESP-NOW: Send failed");
  }
}

// Parse MAC address string (format: "AA:BB:CC:DD:EE:FF")
bool parseMacAddress(const String &macStr, uint8_t *mac) {
  if (macStr.length() != 17) return false; // AA:BB:CC:DD:EE:FF = 17 chars
  
  int parts[6];
  if (sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", 
             &parts[0], &parts[1], &parts[2], 
             &parts[3], &parts[4], &parts[5]) != 6) {
    return false;
  }
  
  for (int i = 0; i < 6; i++) {
    if (parts[i] < 0 || parts[i] > 255) return false;
    mac[i] = (uint8_t)parts[i];
  }
  return true;
}

// Initialize ESP-NOW
void initEspNow() {
  WiFi.mode(WIFI_AP);
  
  if (esp_now_init() != ESP_OK) {
    DBG_PRINTLN("ESP-NOW initialization failed");
    return;
  }
  
  esp_now_register_send_cb(onEspNowSend);
  
  if (receiverMacStr.length() > 0 && parseMacAddress(receiverMacStr, receiverMacAddress)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMacAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      DBG_PRINTLN("Failed to add ESP-NOW peer");
      return;
    }
    DBG_PRINT("ESP-NOW initialized with receiver: ");
    DBG_PRINTLN(receiverMacStr);
  } else {
    DBG_PRINTLN("ESP-NOW initialized (no receiver MAC configured yet)");
  }
}

// Monitor SoftAP client connections to limit to one connected client
void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  // We only want one SoftAP connection. For basic security, we rely
  // on the AP password to limit who can change our configuration.
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      apConnected = false;
      DBG_PRINTLN("SoftAP client disconnected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      apConnected = true;
      DBG_PRINTLN("SoftAP client connected");
      break;
    default:
      break;
  }
}
  
//####################################  BEGIN SETUP  ##################################
void setup() {
  Serial.begin(115200);  // USB CDC for debugging and logging
  
  // Initialize A02YYUW ultrasonic distance sensor on Serial1
  Serial1.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  // connect to EEPROM storage
  if (!preferences.begin("config", false)) {
    Serial.println("Failed to initialize Preferences: Halting");
    while (1);
  }
  DBG_PRINTLN("Preferences initialized");

  // mount our file server for our static data and web pages
  if (!LittleFS.begin()) {
    Serial.println("An error has occurred while mounting LittleFS: Halting");
    while (1);
  }
  DBG_PRINTLN("LittleFS successfully mounted");

  // get default SoftAP configuration from file
  File dataFile = LittleFS.open(s_configFile, FILE_READ); //Open File for reading
  if (!dataFile) {
    Serial.println("Config file open failed on read: Halting");
    while (1);
  } else {
    String configContents;
    DBG_PRINTLN("Reading configuration data from file:");
    if (dataFile.available()) configContents = dataFile.readString();
    dataFile.close();
    #ifdef HIDE_PASSWORDS
      DBG_PRINTLN( "*Hidden*");
    #else
      DBG_PRINTLN(configContents);
    #endif

    configJSON = JSON.parse(configContents);
    if (JSON.typeof(configJSON) == "undefined") {
      Serial.println("Parsing configuration file data failed: Halting");
      while (1);
    }
  }

  // reset board to defaults if necessary (i.e. lost AP password)
  //  click EN/RESET button, then press and hold BOOT/GPIO0 button for 5 seconds
  //  careful with Adafruit Huzzah ESP32 breakout board, it does not come with 
  //  pull-up on GPIO0 like the Devkit V1 boards - you need to add it, or use INPUT_PULLUP.
  //  select the INPUT version below if you have an external pullup resistor.
  pinMode( 0, INPUT_PULLUP);
  //pinMode( 0, INPUT);
 
  Serial.println( "Hold \"BOOT\" button to reset board to defaults");
  delay(3000);
  if (digitalRead( 0) == LOW) { // button is pulled high on hardware, pressing it makes it low
    Serial.println( "Resetting, release \"BOOT\" button now");
    preferences.clear();
  }

  // retrieve EEPROM sensor filter and offset values
  alpha = preferences.getFloat("alpha", alpha);
  distanceOffset = preferences.getFloat("distanceOffset", distanceOffset);
  
  DBG_PRINT("Retrieved Filter Alpha: ");
  DBG_PRINT(alpha);
  DBG_PRINT(" Distance Offset: ");
  DBG_PRINTLN(distanceOffset);

  // retrieve EEPROM ESP-NOW receiver MAC address
  receiverMacStr = preferences.getString("receiverMac", "");
  if (receiverMacStr.length() > 0) {
    DBG_PRINT("Retrieved Receiver MAC: ");
    DBG_PRINTLN(receiverMacStr);
  } else {
    DBG_PRINTLN("No receiver MAC configured yet");
  }

  // retrieve EEPROM AP configuration
  esid = preferences.getString("AP_SSID", "");
  epass = preferences.getString("AP_PASS", "");
  String eap = preferences.getString("AP_IP", "");

  // if EEPROM info is missing, use defaults from config file
  if ((esid == "") && (epass == "")) {
    DBG_PRINTLN("\nInitial SoftAP user/pass configuration");
    esid = String(configJSON["AP_name"]);
    epass = String(configJSON["AP_pass"]);
    preferences.putString("AP_SSID", esid);
    preferences.putString("AP_PASS", epass);
  }
  if (eap == "") {
    DBG_PRINTLN("\nInitial SoftAP address configuration");
    eap = String(configJSON["AP_IP"]);
    preferences.putString("AP_IP", eap);
  }

  DBG_PRINT("Access point SSID: ");
  DBG_PRINTLN(esid);
  DBG_PRINT("Access point PASSWORD: ");
  #ifdef HIDE_PASSWORDS
    DBG_PRINTLN( "*Hidden*");
  #else
    DBG_PRINTLN(epass);
  #endif
  DBG_PRINT("Access point ADDRESS: ");
  DBG_PRINTLN(eap);

  // start our SoftAP with one allowed connection
  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_AP);
  WiFi.softAPsetHostname(esid.c_str());
  if (!WiFi.softAP(esid.c_str(), epass.c_str(), 1, 0, 1))
  {
    Serial.println("SoftAP creation failed: Halting");
    while (1);
  }
  delay(100);

  IPAddress apIP;
  apIP.fromString(eap);
  IPAddress NMask(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, NMask);
  myIP = WiFi.softAPIP().toString();
  DBG_PRINT("SoftAP server started at: ");
  DBG_PRINTLN(myIP);

  // Initialize ESP-NOW for distance data transmission
  initEspNow();

  // HTML+JS+CSS+Images server callbacks
  // handlers are evaluated in the order they are attached to the server.
  server.onNotFound([](AsyncWebServerRequest * request) {
    request -> send(404, "text/html", "<h1 align=\"center\">Oops! It's not here.</h1>");
  });

  server.on("/setup", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    if(!request->authenticate(esid.c_str(), epass.c_str()))
      return request->requestAuthentication();
    request -> send(LittleFS, "/setup.html", "text/html");
  });

  server.on("/setup.html", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    if(!request->authenticate(esid.c_str(), epass.c_str()))
      return request->requestAuthentication();
    request -> send(LittleFS, "/setup.html", "text/html");
  });

  // server callback button handlers
  server.on("/setupData", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    JSONVar data;
    data["default_pass"] = (epass == String(configJSON["AP_pass"]));
    data["ver"] = String(VERSION);
    data["myIP"] = myIP;
    data["apname"] = esid;
    data["alpha"] = String(alpha, 3);
    data["distance_offset"] = String(distanceOffset, 1);
    data["receiver_mac"] = receiverMacStr;
    data["MAC"] = WiFi.softAPmacAddress();
    request -> send(200, "application/json", JSON.stringify(data));
  });

  server.on("/espNowStatus", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    JSONVar data;
    unsigned long elapsed = millis() - lastSendTime;
    if (elapsed > 2147483647UL) elapsed = 0;  // handle millis() overflow
    data["lastSendTime"] = elapsed;
    data["lastSendSuccess"] = lastSendSuccess;
    request -> send(200, "application/json", JSON.stringify(data));
  });

  server.on("/applyBtnFunction", WebRequestMethod::HTTP_POST, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    DBG_PRINTLN( "Assigning AP");
    String txtssid, txtpass, txtaplan;
    if (!getParamStr(request, "txtssid", true, txtssid) || !getParamStr(request, "txtpass", true, txtpass)
        || !getParamStr(request, "txtaplan", true, txtaplan)) {
      request -> send(400, "text/plain", "missing parameter");
      return;
    }
    if (txtssid.length() > 0) {
      preferences.putString("AP_SSID", txtssid);
      DBG_PRINT("Writing AP SSID :: ");
      DBG_PRINTLN(txtssid);
    }
    // WPA2 passwords must be 8 or more characters. SoftAP will fail if fewer.
    if (txtpass.length() > 7) {
      preferences.putString("AP_PASS", txtpass);
      DBG_PRINT("Writing AP PASSWORD :: ");
      #ifdef HIDE_PASSWORDS
        DBG_PRINTLN( "*Hidden*");
      #else
        DBG_PRINTLN(txtpass);
      #endif
    } else {
      DBG_PRINT("New AP Password, \"");
      DBG_PRINT(txtpass);
      DBG_PRINTLN("\", too short, not saved");
    }
    if (txtaplan.length() > 0) {
      preferences.putString("AP_IP", txtaplan);
      DBG_PRINT("Writing AP IP :: ");
      DBG_PRINTLN(txtaplan);
    }
    request -> send(200, "text/plain", "ok");
  });

  server.on("/setReceiverMac", WebRequestMethod::HTTP_POST, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    DBG_PRINTLN("Setting ESP-NOW receiver MAC address");
    String newMacStr;
    if (!getParamStr(request, "receiver_mac", true, newMacStr)) {
      request -> send(400, "text/plain", "missing receiver_mac parameter");
      return;
    }

    uint8_t tempMac[6];
    if (!parseMacAddress(newMacStr, tempMac)) {
      request -> send(400, "text/plain", "Invalid MAC address format. Use: AA:BB:CC:DD:EE:FF");
      return;
    }

    // Try to remove old peer if it exists
    if (receiverMacStr.length() > 0) {
      esp_now_del_peer(receiverMacAddress);
    }

    receiverMacStr = newMacStr;
    memcpy(receiverMacAddress, tempMac, 6);
    preferences.putString("receiverMac", receiverMacStr);

    // Add new peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMacAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      request -> send(500, "text/plain", "Failed to add ESP-NOW peer");
      return;
    }

    DBG_PRINT("ESP-NOW receiver MAC set to: ");
    DBG_PRINTLN(receiverMacStr);
    request -> send(200, "text/plain", "ok");
  });

  server.on("/applyOffsetFunction", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    DBG_PRINTLN("Assigning Calibration values");
    String sAlpha, sOffset;
    if (!getParamStr(request, "alpha", false, sAlpha) || !getParamStr(request, "distance_offset", false, sOffset)) {
      request -> send(400, "text/plain", "missing parameter");
      return;
    }

    float newAlpha, newOffset;
    if (!parseStrictFloat(sAlpha, newAlpha) || newAlpha <= 0.0f || newAlpha >= 1.0f) {
      request -> send(400, "text/plain", "Filter alpha must be a number greater than 0 and less than 1.");
      return;
    }
    if (!parseStrictFloat(sOffset, newOffset)) {
      request -> send(400, "text/plain", "Distance offset must be a number.");
      return;
    }

    alpha = newAlpha;
    distanceOffset = newOffset;
    preferences.putFloat("alpha", alpha);
    preferences.putFloat("distanceOffset", distanceOffset);
    
    DBG_PRINT("Calibration set - Alpha: ");
    DBG_PRINT(alpha);
    DBG_PRINT(" Distance Offset (cm): ");
    DBG_PRINTLN(distanceOffset);
    
    request -> send(200, "text/plain", "ok");
  });

  server.on("/rebootBtnFunction", WebRequestMethod::HTTP_POST, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    DBG_PRINTLN( "Requesting Restart");
    if (request->hasParam("reboot_btn", true) && request -> getParam("reboot_btn", true) -> value() == "reboot_device") {
      DBG_PRINTLN("restarting device");
      request -> send(200, "text/plain", "ok");
      // handle rebooting in the loop() - watchdog gets upset if we block with Delay() here.
      bootStart = millis();
    } else {
      request -> send(400, "text/plain", "bad request");
    }
  });

  server.on("/resetBtnFunction", WebRequestMethod::HTTP_POST, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    DBG_PRINTLN( "Requesting Reset");
    if (request->hasParam("reset_btn", true) && request -> getParam("reset_btn", true) -> value() == "reset_device") {
      preferences.clear();
      DBG_PRINTLN("EEPROM cleared");
      request -> send(200, "text/plain", "ok");
    } else {
      request -> send(400, "text/plain", "bad request");
    }
  });

  // only these folders are public; APconfig.json and the HTML pages are not served from here
  server.serveStatic("/images", LittleFS, "/images");
  server.serveStatic("/js", LittleFS, "/js");

  // setup our WiFi for AP mode only
  WiFi.mode(WIFI_AP);

  server.begin(); // start the webserver for the Access Point and Station
  
  // start the OTA service using our SoftAP credentials
  ElegantOTA.begin(&server, esid.c_str(), epass.c_str());

  Serial.println("Startup complete");
}

//####################################  BEGIN LOOP  ##################################
void loop() {
  ElegantOTA.loop();
  
  if (bootStart != 0) {
    // received a reboot request from the setup page
    if ((millis() - bootStart) > bootDelay) {
      ESP.restart();
    }
  }
  
  // poll Ultrasonic sensor at the sensor bandwidth rate (~10Hz)
  if ((millis() - lastSensorReadTime) >= READ_TIMEOUT_MS) {
    updateSensorData();
    lastSensorReadTime = millis();
  }
  
  // Send distance and ID via ESP-NOW every 500ms
  if ((millis() - lastWebTime) >= ESPDelay) {
    lastWebTime = millis();
    
    if (receiverMacStr.length() > 0) {
      int16_t finalDistance = (int16_t)filteredDistance;
      finalDistance = finalDistance + (int16_t)(distanceOffset * 10.0f);
      
      EspNowMessage msg;
      msg.distance = finalDistance;
      msg.id = SENSOR_ID;
      
      esp_err_t result = esp_now_send(receiverMacAddress, (uint8_t *)&msg, sizeof(msg));
      if (result != ESP_OK) {
        DBG_PRINTLN("ESP-NOW send failed");
      }
    }
  }
}
