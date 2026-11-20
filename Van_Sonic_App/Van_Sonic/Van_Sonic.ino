/*******************************************************************************************
 Van Sonic

    ESP32 application to read sensor data from an A02YYUW ultrasonic distance sensor
    and transmit those readings using the ESP-NOW protocol to an ESP32 receiver app.

    The /setup page allows configuration of the Access Point, WiFi network,
    sensor calibration, and firmware updates.  The default AP name is VanSonic_nnnn,
    where nnnn is the last four hex digits of the device's factory MAC address.
    Change the AP credentials using /setup, or recover the defaults by holding the ESP32
    BOOT button for 5 seconds during startup.

    Connect this device and Van_Level to the same 2.4 GHz access point so ESP-NOW follows
    the router's channel automatically. The Receiver MAC address must match the STA MAC
    reported in the Van_Level app setup page. Setup remains available through the SoftAP.

  Over The Air updates are handled by ElegantOTA
  https://github.com/ayushsharma82/ElegantOTA.

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
      it will still create a .bin file from the data folder.  In the debugging window
      you'll see the .littlefs.bin file location.  Probably in
      "C:\Users\<username>\AppData\Local\Temp\".

*******************************************************************************************/
const char* VERSION = "1.0.0b";

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Arduino_JSON.h>
#include <Preferences.h>
#include <ElegantOTA.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_timer.h>

#define DEBUG_SERIAL 0
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

// SoftAP and STA configuration
constexpr char DEFAULT_AP_PASSWORD[] = "12345678";
constexpr char DEFAULT_AP_IP[] = "192.168.4.1";
String esid;
String epass;
String password;
String myIP;
String localIP = "Not Connected";
String wsid;
String wpass;
String wifiStatusText = "Starting up";
const unsigned long staRetryInterval = 10000;
unsigned long lastStaRetryAttempt = 0;
volatile bool wifiApplyPending = false;
bool apConnected = false;

// EEPROM storage for configuration values
Preferences preferences;

// Calibration variables - reasonable defaults here, configurable on setup page
float distanceOffset = 0.0f;  // offset in the selected units to account for sensor mounting position
float obstacleThresholdMm = 50.8f;  // 2" warning distance
// Timer variables
unsigned long bootStart = 0;  // if non-zero, will restart when bootStart + bootDelay = now
const unsigned long bootDelay = 5000;
unsigned long lastPushTime = 0;      // sensor data ESP-NOW push
const unsigned long ESPDelay = 100;

// Webserver pieces
AsyncWebServer server(80);
AsyncEventSource events("/events");

// Three available sensor zones in Van Level app.
const uint8_t SENSOR_ID = 1;  // Behind Driver Side rear wheels

// A02YYUW Ultrasonic Distance Sensor
const uint8_t HEADER_BYTE = 0xFF;
#define RX_PIN D5  // Xiao ESP32S3 pin / White Wire A02YYUW
#define TX_PIN D6  // Xiao ESP32S3 pin / Yellow Wire A02YYUW

// ESP-NOW configuration
enum DistanceUnits : uint8_t {
  UNITS_CM = 0,
  UNITS_INCH = 1,
  UNITS_M = 2,
  UNITS_FT = 3
};

typedef struct {
  uint8_t id;
  uint8_t units;    // DistanceUnits
  int16_t distance; // tenths of the selected unit (e.g. 1234 = 123.4)
  float obstacleThresholdMm;
} EspNowMessage;

static_assert(sizeof(EspNowMessage) == 8, "Unexpected ESP-NOW packet layout");

const char* DISTANCE_UNIT_NAMES[] = {"cm", "in", "m", "ft"};
const float MILLIMETERS_PER_UNIT[] = {10.0f, 25.4f, 1000.0f, 304.8f};
uint8_t distanceUnits = UNITS_CM;

uint8_t receiverMacAddress[6] = {0, 0, 0, 0, 0, 0};
String receiverMacStr = "";

volatile bool lastSendSuccess = false;
unsigned long lastSendTime = 0;

// Latest sensor reading
float latestDistance = 0.0f;

struct SuccessfulReading {
  float distance = 0.0f;
  uint8_t units = UNITS_CM;
  int64_t receivedAt = 0;
  bool received = false;
};

SuccessfulReading lastSuccessfulReading;
portMUX_TYPE readingsMutex = portMUX_INITIALIZER_UNLOCKED;

struct EspNowSendState {
  bool attempted = false;
  bool success = false;
  bool succeeded = false;
  int64_t succeededAt = 0;
};

EspNowSendState sendState;
bool sendPending = false;

// Read the latest distance value from the serial interface
// Process everyting in the serial buffer returning only the most recent reading
//   Each measurement frame consists of 4 bytes:
//     • Byte 0 (0xFF): Header/Start byte.
//     • Byte 1 (High Byte): Upper 8 bits of the distance value.
//     • Byte 2 (Low Byte): Lower 8 bits of the distance value.
//     • Byte 3 (Checksum): (Byte 0 + Byte 1 + Byte 2) & 0x00FF.
bool updateSensorData() {
  static uint8_t frame[4] = {};
  static size_t frameLength = 0;
  bool received = false;

  while (Serial1.available() > 0) {
    // grab the first full frame
    uint8_t nextByte = (uint8_t)Serial1.read();
    if (frameLength == 0 && nextByte != HEADER_BYTE) continue;
    frame[frameLength++] = nextByte;
    if (frameLength < sizeof(frame)) continue;

    // if frame is valid, save the distance value
    uint8_t checksum = (uint8_t)(frame[0] + frame[1] + frame[2]);
    if (frame[0] == HEADER_BYTE && checksum == frame[3]) {
      latestDistance = (frame[1] << 8) | frame[2];
      portENTER_CRITICAL(&readingsMutex);
      lastSuccessfulReading.distance = roundf((latestDistance / MILLIMETERS_PER_UNIT[distanceUnits]
          + distanceOffset) * 10.0f) / 10.0f;
      lastSuccessfulReading.units = distanceUnits;
      lastSuccessfulReading.receivedAt = esp_timer_get_time();
      lastSuccessfulReading.received = true;
      portEXIT_CRITICAL(&readingsMutex);
      received = true;
      frameLength = 0;
    } else {
      memmove(frame, frame + 1, sizeof(frame) - 1);
      frameLength = sizeof(frame) - 1;
    }
  }
  return received;
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

bool hasValidSavedWifiCredentials() {
  return wsid.length() > 0 && wsid != "Not Given" && wpass.length() >= 8;
}

void startStaConnect() {
  WiFi.begin(wsid.c_str(), wpass.c_str());
  lastStaRetryAttempt = millis();
}

void retryWifiStaConnection() {
  if (!hasValidSavedWifiCredentials() || WiFi.status() == WL_CONNECTED) return;
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  if (millis() - lastStaRetryAttempt >= staRetryInterval) {
    wifiStatusText = "AP only - searching for saved WiFi";
    startStaConnect();
  }
}

void applyWifiSettings() {
  WiFi.disconnect(false);
  wsid = preferences.getString("WIFI_SSID", "");
  wpass = preferences.getString("WIFI_PASS", "");
  localIP = "Not Connected";
  if (preferences.getString("WIFI_MODE", "") == "static") {
    IPAddress address, gateway, subnet, primaryDNS, secondaryDNS;
    address.fromString(preferences.getString("WIFI_IP", ""));
    gateway.fromString(preferences.getString("WIFI_GATE", ""));
    subnet.fromString(preferences.getString("WIFI_MASK", ""));
    primaryDNS.fromString(preferences.getString("WIFI_PDNS", ""));
    secondaryDNS.fromString(preferences.getString("WIFI_SDNS", ""));
    WiFi.config(address, gateway, subnet, primaryDNS, secondaryDNS);
  } else {
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
  }
  if (hasValidSavedWifiCredentials()) {
    wifiStatusText = "Connecting to saved WiFi";
    startStaConnect();
  } else {
    wifiStatusText = "AP only - no WiFi configured";
  }
}

// Reads a request parameter without dereferencing a missing one
bool getParamStr(AsyncWebServerRequest *request, const char *name, bool post, String &out) {
  if (!request->hasParam(name, post)) return false;
  out = request->getParam(name, post)->value();
  return true;
}

// Setup and configuration endpoints use the AP credentials
#define REQUIRE_AUTH(req) if (!(req)->authenticate(esid.c_str(), epass.c_str())) return (req)->requestAuthentication()

void completeEspNowSend(bool success) {
  portENTER_CRITICAL(&readingsMutex);
  if (!sendPending) {
    portEXIT_CRITICAL(&readingsMutex);
    return;
  }
  sendState.attempted = true;
  sendState.success = success;
  if (success) {
    sendState.succeeded = true;
    sendState.succeededAt = esp_timer_get_time();
  }
  sendPending = false;
  lastSendSuccess = success;
  lastSendTime = millis();
  portEXIT_CRITICAL(&readingsMutex);
}

// ESP-NOW send status callback
void onEspNowSend(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  bool success = status == ESP_NOW_SEND_SUCCESS;
  completeEspNowSend(success);
  if (success) {
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
  WiFi.setSleep(false);  // power conservation messes up ESP-NOW
  
  if (esp_now_init() != ESP_OK) {
    DBG_PRINTLN("ESP-NOW initialization failed");
    return;
  }
  
  esp_now_register_send_cb(onEspNowSend);
  
  if (receiverMacStr.length() > 0 && parseMacAddress(receiverMacStr, receiverMacAddress)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMacAddress, 6);
    peerInfo.channel = 0;
    peerInfo.ifidx = WIFI_IF_AP;
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
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      localIP = WiFi.localIP().toString();
      wifiStatusText = "Connected to saved WiFi";
      Serial.printf("Connected to saved WiFi at %s on channel: %ld\n", localIP.c_str(), static_cast<long>(WiFi.channel()));
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      localIP = "Not Connected";
      if (hasValidSavedWifiCredentials()) wifiStatusText = "AP only - searching for saved WiFi";
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

  distanceOffset = preferences.getFloat("distanceOffset", distanceOffset);
  distanceUnits = preferences.getUChar("distanceUnits", preferences.getBool("useInches", false) ? UNITS_INCH : UNITS_CM);
  if (distanceUnits != UNITS_CM && distanceUnits != UNITS_INCH) distanceUnits = UNITS_CM;
  obstacleThresholdMm = preferences.getFloat("obstacleMm", obstacleThresholdMm);
  
  DBG_PRINT("Retrieved Distance Offset: ");
  DBG_PRINT(distanceOffset);
  DBG_PRINTLN(DISTANCE_UNIT_NAMES[distanceUnits]);

  // retrieve EEPROM ESP-NOW receiver MAC address
  receiverMacStr = preferences.getString("receiverMac", "");
  if (receiverMacStr.length() > 0) {
    DBG_PRINT("Retrieved Receiver MAC: ");
    DBG_PRINTLN(receiverMacStr);
  } else {
    DBG_PRINTLN("No receiver MAC configured yet");
  }

  // Use saved AP settings when present; defaults are not stored.
  esid = preferences.getString("AP_SSID", "");
  epass = preferences.getString("AP_PASS", DEFAULT_AP_PASSWORD);
  String eap = preferences.getString("AP_IP", DEFAULT_AP_IP);
  if (esid.isEmpty()) {
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
      Serial.println("Failed to read factory MAC for default SoftAP SSID: Halting");
      while (1);
    }
    char defaultSsid[sizeof("VanSonic_0000")];
    snprintf(defaultSsid, sizeof(defaultSsid), "VanSonic_%02X%02X",
             static_cast<unsigned>(mac[4]), static_cast<unsigned>(mac[5]));
    esid = defaultSsid;
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
  WiFi.mode(WIFI_AP_STA);
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

  // HTML+Images server callbacks
  // handlers are evaluated in the order they are attached to the server.
  server.onNotFound([](AsyncWebServerRequest * request) {
    request -> send(404, "text/html", "<h1 align=\"center\">Oops! It's not here.</h1>");
  });

  server.on("/", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    if (LittleFS.exists("/index.html")) request -> send(LittleFS, "/index.html", "text/html");
    else request -> redirect("/setup");
  });

  server.on("/index.html", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    if (LittleFS.exists("/index.html")) request -> send(LittleFS, "/index.html", "text/html");
    else request -> redirect("/setup");
  });

  server.on("/readings", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    portENTER_CRITICAL(&readingsMutex);
    SuccessfulReading reading = lastSuccessfulReading;
    EspNowSendState send = sendState;
    bool pending = sendPending;
    portEXIT_CRITICAL(&readingsMutex);
    JSONVar data = JSON.parse("[]");
    JSONVar item;
    item["id"] = SENSOR_ID;
    item["received"] = reading.received;
    item["send_status"] = receiverMacStr.length() == 0 ? "Not configured"
        : send.attempted ? (send.success ? "Success" : "Failed") : pending ? "Sending" : "Not sent";
    item["send_succeeded"] = send.succeeded;
    if (send.succeeded) {
      item["send_age_ms"] = (double)(esp_timer_get_time() - send.succeededAt) / 1000.0;
    }
    if (reading.received) {
      item["value"] = reading.distance;
      item["units"] = DISTANCE_UNIT_NAMES[reading.units];
      item["age_ms"] = (double)(esp_timer_get_time() - reading.receivedAt) / 1000.0;
    }
    data[0] = item;
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", JSON.stringify(data));
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
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
    data["default_pass"] = (epass == DEFAULT_AP_PASSWORD);
    data["ver"] = String(VERSION);
    data["myIP"] = myIP;
    data["apname"] = esid;
    data["distance_offset"] = String(distanceOffset, 6);
    data["obstacle_threshold"] = String(obstacleThresholdMm / MILLIMETERS_PER_UNIT[distanceUnits], 6);
    data["units"] = DISTANCE_UNIT_NAMES[distanceUnits];
    data["receiver_mac"] = receiverMacStr;
    data["wifi_channel"] = WiFi.channel();
    data["wifi_state"] = wifiStatusText;
    data["localIP"] = localIP;
    data["wsid"] = wsid;
    data["MAC"] = WiFi.softAPmacAddress();
    request -> send(200, "application/json", JSON.stringify(data));
  });

  server.on("/scanWifi", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    int count = WiFi.scanComplete();
    if (count == WIFI_SCAN_FAILED) {
      WiFi.scanNetworks(true);
      request -> send(202, "application/json", "{\"scanning\":true}");
      return;
    }
    if (count == WIFI_SCAN_RUNNING) {
      request -> send(202, "application/json", "{\"scanning\":true}");
      return;
    }
    JSONVar list = JSON.parse("[]");
    for (int index = 0; index < count; ++index) {
      JSONVar network;
      network["rssi"] = WiFi.RSSI(index);
      network["ssid"] = WiFi.SSID(index);
      network["bssid"] = WiFi.BSSIDstr(index);
      network["channel"] = WiFi.channel(index);
      network["secure"] = (int)WiFi.encryptionType(index);
      list[index] = network;
    }
    WiFi.scanDelete();
    request -> send(200, "application/json", JSON.stringify(list));
  });

  server.on("/connectBtnFunction", WebRequestMethod::HTTP_POST, [](AsyncWebServerRequest * request) {
    REQUIRE_AUTH(request);
    String ssid, pass, mode;
    if (!getParamStr(request, "wifi_ssid", true, ssid) || !getParamStr(request, "wifi_pass", true, pass)
        || !getParamStr(request, "wifi_mode", true, mode)) {
      request -> send(400, "text/plain", "missing parameter");
      return;
    }
    if (ssid.length() == 0 || ssid.length() > 32 || pass.length() < 8 || pass.length() > 63
        || (mode != "dhcp" && mode != "static")) {
      request -> send(400, "text/plain", "Invalid SSID, WPA password (8-63 characters), or WiFi mode.");
      return;
    }
    const char* parameters[] = {"txtipadd", "net_m", "g_add", "p_dns", "s_dns"};
    const char* keys[] = {"WIFI_IP", "WIFI_MASK", "WIFI_GATE", "WIFI_PDNS", "WIFI_SDNS"};
    String addresses[5];
    if (mode == "static") {
      for (size_t index = 0; index < 5; ++index) {
        getParamStr(request, parameters[index], true, addresses[index]);
        IPAddress address;
        if ((index < 4 || addresses[index].length() > 0) && !address.fromString(addresses[index])) {
          request -> send(400, "text/plain", "Invalid static network address.");
          return;
        }
      }
      for (size_t index = 0; index < 5; ++index) preferences.putString(keys[index], addresses[index]);
    }
    preferences.putString("WIFI_SSID", ssid);
    preferences.putString("WIFI_PASS", pass);
    preferences.putString("WIFI_MODE", mode);
    wifiApplyPending = true;
    request -> send(200, "text/plain", "ok");
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
    peerInfo.ifidx = WIFI_IF_AP;
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
    String sOffset, sUnits, sThreshold;
    if (!getParamStr(request, "distance_offset", false, sOffset)
        || !getParamStr(request, "units", false, sUnits)
        || !getParamStr(request, "obstacle_threshold", false, sThreshold)) {
      request -> send(400, "text/plain", "missing parameter");
      return;
    }
    uint8_t newUnits = UNITS_CM;
    while (newUnits <= UNITS_INCH && sUnits != DISTANCE_UNIT_NAMES[newUnits]) ++newUnits;
    if (newUnits > UNITS_INCH) {
      request -> send(400, "text/plain", "Units must be cm or in.");
      return;
    }

    float newOffset;
    if (!parseStrictFloat(sOffset, newOffset)) {
      request -> send(400, "text/plain", "Distance offset must be a number.");
      return;
    }
    float newThreshold;
    if (!parseStrictFloat(sThreshold, newThreshold) || newThreshold <= 0
        || !isfinite(newThreshold * MILLIMETERS_PER_UNIT[newUnits])) {
      request -> send(400, "text/plain", "Obstacle threshold must be a positive finite distance.");
      return;
    }

    distanceOffset = newOffset;
    distanceUnits = newUnits;
    obstacleThresholdMm = newThreshold * MILLIMETERS_PER_UNIT[newUnits];
    preferences.putFloat("distanceOffset", distanceOffset);
    preferences.putUChar("distanceUnits", distanceUnits);
    preferences.putFloat("obstacleMm", obstacleThresholdMm);
    
    DBG_PRINT("Calibration set - Distance Offset: ");
    DBG_PRINT(distanceOffset);
    DBG_PRINTLN(DISTANCE_UNIT_NAMES[distanceUnits]);
    
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

  // Only serve the public asset folders here.
  server.serveStatic("/images", LittleFS, "/images");
  events.onConnect([](AsyncEventSourceClient * client) {
    client -> send("hello!", NULL, millis(), 1000);
  });
  server.addHandler(&events);
 
  WiFi.setAutoReconnect(true);
  applyWifiSettings();

  server.begin(); // start the webserver for the Access Point and Station
  
  // start the OTA service using our SoftAP credentials
  ElegantOTA.begin(&server, esid.c_str(), epass.c_str());

  Serial.println("Startup complete");
}

//####################################  BEGIN LOOP  ##################################
void loop() {
  ElegantOTA.loop();
  
  if (wifiApplyPending) {
    wifiApplyPending = false;
    applyWifiSettings();
  }
  retryWifiStaConnection();
  
  if (bootStart != 0) {
    // received a reboot request from the setup page
    if ((millis() - bootStart) > bootDelay) {
      ESP.restart();
    }
  }
  
  updateSensorData();
  
  portENTER_CRITICAL(&readingsMutex);
  bool pending = sendPending;
  portEXIT_CRITICAL(&readingsMutex);
  if (!pending && (millis() - lastPushTime) >= ESPDelay) {
    lastPushTime = millis();
    
    if (receiverMacStr.length() > 0) {
      // sensor reports mm; send tenths of the selected unit
      const float mmPerUnit = MILLIMETERS_PER_UNIT[distanceUnits];
      int16_t finalDistance = (int16_t)lroundf((latestDistance / mmPerUnit + distanceOffset) * 10.0f);
      
      EspNowMessage msg;
      msg.distance = finalDistance;
      msg.id = SENSOR_ID;
      msg.units = distanceUnits;
      msg.obstacleThresholdMm = obstacleThresholdMm;
      
      portENTER_CRITICAL(&readingsMutex);
      sendPending = true;
      portEXIT_CRITICAL(&readingsMutex);
      esp_err_t result = esp_now_send(receiverMacAddress, (uint8_t *)&msg, sizeof(msg));
      if (result != ESP_OK) {
        completeEspNowSend(false);
        DBG_PRINTF("ESP-NOW send failed: %s (0x%X)\n", esp_err_to_name(result), (unsigned int)result);
      } else {
        DBG_PRINT("ESP_NOW: ");
        DBG_PRINT(finalDistance / 10.0f);
        DBG_PRINT(" ");
        DBG_PRINT(DISTANCE_UNIT_NAMES[msg.units]);
        DBG_PRINTLN("");
      }
    }
  }
}
