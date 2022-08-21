#define FASTLED_ESP32_I2S true // https://github.com/FastLED/FastLED/issues/1220

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <AsyncTCP.h>
#include <FastLED.h>
#include "SPIFFS.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include "esp_adc_cal.h"

#define VERSION "1.0.1"

#define LED_PIN_FRONT     19
#define LED_PIN_BACK     18
#define NUM_LEDS    8
#define LED_TYPE    WS2811
#define COLOR_ORDER GRB

#define VBUS_MON_PIN 34
#define CHARGING_PIN 13
#define BATTERY_VOLTAGE_CHANNEL ADC1_CHANNEL_7 /* GPIO35 = CH7, see ESP32-WROVER datasheet */

#define BUTTON_PIN  25
#define BUTTON_LONG_PRESS_MSEC 500
#define BUTTON_RESET_PRESS_MSEC 10000

#define EEPROM_SIZE 98
#define EEPROM_ADDRESS_BRIGHTNESS    0  // 1 byte
#define EEPROM_ADDRESS_LIGHT_INDEX   1  // 1 byte
#define EEPROM_ADDRESS_VMIX_PORT     2  // 4 bytes
#define EEPROM_ADDRESS_VMIX_HOST     8  // 56 bytes
#define EEPROM_ADDRESS_NAME          64 // 32 bytes
#define EEPROM_ADDRESS_PREVIEW_FRONT 96 // 1 byte
#define EEPROM_ADDRESS_PREVIEW_BACK  97 // 1 byte
// #define EEPROM_ADDRESS_FREE 98  // 54 bytes

#define STATE_STARTUP 0
#define STATE_WLAN_CONNECTED 1
#define STATE_NOT_CONNECTED 2
#define STATE_CONNECTED 3
#define STATE_CONNECTING 4
#define STATE_WLAN_CONFIGURATION 5
#define STATE_PRE_RESET 6

#define VMIX_TIMEOUT_MSEC 10000
#define VMIX_RECONNECT_INTERVAL_MSEC 10000
#define VMIX_POLL_INTERVAL_MSEC 5000
#define VMIX_TALLY_REQUEST "TALLY\n"
#define VMIX_TALLY_SUBSCRIBE "SUBSCRIBE TALLY\n"
#define VMIX_TALLY_OK "TALLY OK "

void(* resetFunc) (void) = 0;//declare reset function at address 0

struct LED_STATE {
  unsigned long nextSwitch = 0;
  boolean flashOn = false;
  byte iterateCurrentLed = 0;
};

// Configuration
String name;
String vmixHost;
uint32_t vmixPort = 0;
byte lightIndex = 0;

// Persistet state
byte brightness = 255;
bool showPreviewOnFront = false;
bool showPreviewOnBack = true;

// Temp state
short brightnessDirection = 1;
time_t eepromCommitDueAt = 0;
byte lightValue = 0;
time_t preResetStartedAt = 0;
int state = STATE_STARTUP;
LED_STATE *ledStateFront = new LED_STATE();
LED_STATE *ledStateBack = new LED_STATE();
float smoothBatteryVoltage = 0;
float showBatteryStateUntil = 0;

// Wifi
AsyncWiFiManager* wifiManager;
AsyncWebServer server(80);
DNSServer dns;

// VMix
AsyncClient *vmix = new AsyncClient;
time_t vmixLastDataReceived = 0;
time_t vmixNextConnectTry = 0;
time_t vmixLastDataRequested = 0;

// LED
CRGB ledsFront[NUM_LEDS];
CRGB ledsBack[NUM_LEDS];
CRGB colorBlue = CRGB(0, 0, 255);
CRGB colorGreen = CRGB(0, 255, 0);
CRGB colorRed = CRGB(255, 0, 0);
CRGB colorBlack = CRGB(0, 0, 0);
CRGB colorWhite = CRGB(255, 255, 255);
CRGB colorYellow = CRGB(255, 255, 0);

TaskHandle_t  displayTaskHnd;

unsigned long readBatteryVoltageDue = 0;

// Calibration of ADC
esp_adc_cal_characteristics_t adc_chars;



float ReadCalibratedVoltage(adc1_channel_t channel)
{
   uint32_t reading =  adc1_get_raw(channel);
   uint32_t voltage = esp_adc_cal_raw_to_voltage(reading, &adc_chars);
   return ((float)voltage) / 1000.0;
}

float readBatteryVoltage() {
  if (readBatteryVoltageDue < millis()) {
    readBatteryVoltageDue = millis() + 1000;
    float voltage = ReadCalibratedVoltage(BATTERY_VOLTAGE_CHANNEL) * 2.0; //Multiply by 2 because of voltage divider on board
    if (smoothBatteryVoltage == 0) {
      smoothBatteryVoltage = voltage;
    } else {
      smoothBatteryVoltage = smoothBatteryVoltage * 0.9 + voltage * 0.1;
    }
  }
  return smoothBatteryVoltage;
}

int getBatteryPercentage() {
  float voltage = readBatteryVoltage();
  double percentage = 0.4179*pow(voltage,2.0) - 2.1277*voltage + 2.7;
  return  100.0*max(0.0, min(1.0, percentage));
}

int8_t readWifiRssi() {
  return WiFi.RSSI();
}

bool isUSBPowered() {
  return ( analogRead(VBUS_MON_PIN) > 2048 );
}

bool isCharging() {
  return (isUSBPowered() && !digitalRead(CHARGING_PIN));
}

void writeStateToEeprom() {
  EEPROM.writeByte(EEPROM_ADDRESS_LIGHT_INDEX, lightIndex);
  EEPROM.writeUInt(EEPROM_ADDRESS_VMIX_PORT, vmixPort);
  EEPROM.writeString(EEPROM_ADDRESS_VMIX_HOST, vmixHost);
  EEPROM.writeString(EEPROM_ADDRESS_NAME, name);
  EEPROM.writeByte(EEPROM_ADDRESS_PREVIEW_FRONT, showPreviewOnFront);
  EEPROM.writeByte(EEPROM_ADDRESS_PREVIEW_BACK, showPreviewOnBack);
  eepromCommitDueAt = millis() + 100; // Write almost immediately
}

void loadStateFromEeprom() {
  brightness = EEPROM.readByte(EEPROM_ADDRESS_BRIGHTNESS);
  lightIndex = EEPROM.readByte(EEPROM_ADDRESS_LIGHT_INDEX);
  vmixPort = EEPROM.readUInt(EEPROM_ADDRESS_VMIX_PORT);
  char buffer[57];
  size_t read = EEPROM.readString(EEPROM_ADDRESS_VMIX_HOST, buffer, 56);
  if (read < 56) {
    buffer[read + 1] = 0;
    vmixHost = buffer;
  }
  read = EEPROM.readString(EEPROM_ADDRESS_NAME, buffer, 32);
  if (read < 32) {
    buffer[read + 1] = 0;
    name = buffer;
  }
  showPreviewOnFront = EEPROM.readByte(EEPROM_ADDRESS_PREVIEW_FRONT);
  showPreviewOnBack = EEPROM.readByte(EEPROM_ADDRESS_PREVIEW_BACK);
  Serial.println("Loaded from EEPROM");
  if (vmixPort > 65535) {
    Serial.println("Not yet initialized. Setting default values.");
    vmixPort = 8099;
    vmixHost = "";
    name = "New Tally Light";
    lightIndex = 1;
    brightness = 255;
    showPreviewOnFront = false;
    showPreviewOnBack = true;
  }
  Serial.print("Name: ");
  Serial.println(name);
  Serial.print("VMix Port: ");
  Serial.print(vmixHost);
  Serial.print(":");
  Serial.println(vmixPort);
  Serial.print("Light Index: ");
  Serial.println(lightIndex);
  Serial.print("Brightness: ");
  Serial.println(brightness);
}

String dataBuffer = "";

void onData(void *arg, AsyncClient *client, void *data, size_t len)
{
  vmixLastDataReceived = millis();
  String inData = String((char*)data).substring(0, len);
	//Serial.print("In : ");
  //Serial.println(inData);
  dataBuffer += inData;
  int newLinePos = dataBuffer.indexOf("\n");
  while (newLinePos > -1) {
    String line = dataBuffer.substring(0, newLinePos);
    line.trim();
    //Serial.printf("Process line : %s\n", line.c_str());
    String matchString = VMIX_TALLY_OK;
    if (line.startsWith(matchString)) {
      if (line.length() <= matchString.length() + (lightIndex - 1)) {
        //Serial.printf("Requested light index [%d] not found.\n", lightIndex);
      } else {
        byte newLightValue = String(line.charAt(matchString.length() + (lightIndex - 1))).toInt();
        if (lightValue != newLightValue) {
          lightValue = newLightValue;
          //Serial.printf("Light updated to: %d\n", lightValue);
        }
      }
    }
    if (dataBuffer.length() > newLinePos) {
      dataBuffer = dataBuffer.substring(newLinePos + 1, -1);
    } else {
      dataBuffer = "";
    }

    newLinePos = dataBuffer.indexOf("\n");
  }
}

bool vmixSend(String payload) {
  if (vmix->space() > strlen(payload.c_str()) && vmix->canSend())
	{
    //Serial.print("Out: ");
    //Serial.println(payload);
		vmix->add(payload.c_str(), strlen(payload.c_str()));
		vmix->send();
    vmixLastDataRequested = millis();
    return true;
	}
  return false;
}

void requestLightState() {
  if (vmixLastDataRequested + VMIX_POLL_INTERVAL_MSEC > millis()) {
    return;
  }
  if (vmixSend(VMIX_TALLY_REQUEST))
	{
    vmixLastDataRequested = millis();
	}
}

void onConnect(void *arg, AsyncClient *client)
{
	//Serial.printf("VMix client connected to %s:%d\n", vmixHost.c_str(), vmixPort);
  vmixLastDataReceived = millis();
  state = STATE_CONNECTED;
  vmixSend(VMIX_TALLY_SUBSCRIBE);
  requestLightState();
}

void onDisconnect(void *arg, AsyncClient *client) {
  state = STATE_NOT_CONNECTED;
}

void handleEeprom() {
  if (eepromCommitDueAt > 0 && eepromCommitDueAt < millis()) {
    Serial.println("EEPROM commit.");
    EEPROM.commit();
    eepromCommitDueAt = 0;
  }
}

void reset() {
  Serial.println("Execute reset.");
  wifiManager->resetSettings();
  resetFunc();
}

void handleButtonReleased() {
  if (state == STATE_PRE_RESET) {
    reset();
  } else {
    showBatteryStateUntil = millis() + 5000;
  }
}

void writeBrightnessToEeprom() {
  EEPROM.writeByte(EEPROM_ADDRESS_BRIGHTNESS, brightness);
  eepromCommitDueAt = millis() + 15000;
}

time_t lastButtonRead = 0;
int lastButtonState = 0;
time_t buttonPressedTime = 0;

void handleButton() {
  if (lastButtonRead + 10 > millis()) {
    return;
  }
  lastButtonRead = millis();
  int buttonState = digitalRead(BUTTON_PIN);
  if (!buttonState) {
    if (lastButtonState == 0) {
      lastButtonState = 1;
    } else if (lastButtonState == 1) {
      lastButtonState = 2;
      Serial.println("Button pressed");
      buttonPressedTime = millis();
    } else if (lastButtonState == 2) {
      if (millis() - buttonPressedTime > BUTTON_LONG_PRESS_MSEC) {
        if (brightnessDirection > 0 && brightness < 255) {
          brightness+=1;
        } else if (brightnessDirection < 0 && brightness > 10) {
          brightness-=1;
        }
      }
      if (millis() - buttonPressedTime > BUTTON_RESET_PRESS_MSEC) {
        lastButtonState = 3;
        preResetStartedAt = millis();
        state = STATE_PRE_RESET;
      }
    }
  } else {
    if (lastButtonState == 2) {
      Serial.println("Button released");
      if (millis() - buttonPressedTime < BUTTON_LONG_PRESS_MSEC) {
        handleButtonReleased();
      } else {
        brightnessDirection *= -1;
        writeBrightnessToEeprom();
      }
    }
    lastButtonState = 0;
  }
}

void handleVmix() {
  if (state == STATE_PRE_RESET) {
    return;
  }
  if (state == STATE_CONNECTED) {
    if (vmixLastDataReceived + VMIX_POLL_INTERVAL_MSEC < millis()) {
      requestLightState();
    }
    if (vmixLastDataReceived + VMIX_TIMEOUT_MSEC > millis()) {
      return;
    }
    Serial.printf("Did not receive any data from VMix for %d msec. Trying to reconnect.\n", VMIX_TIMEOUT_MSEC);
    state = STATE_NOT_CONNECTED;
  }
  if (vmixNextConnectTry > millis()) {
    return;
  }
  Serial.printf("Connecting to VMix %s:%d\n", vmixHost.c_str(), vmixPort);
  vmixNextConnectTry = millis() + VMIX_RECONNECT_INTERVAL_MSEC;
  state = STATE_CONNECTING;
  vmix->close();
  vmix->connect(vmixHost.c_str(), vmixPort);
}

void setLed(CRGB *led, CRGB color) {
    led->r = ((int)color.r * brightness) / 255;
    led->g = ((int)color.g * brightness) / 255;
    led->b = ((int)color.b * brightness) / 255;
}

void setLeds(CRGB* leds, CRGB color) {
  for (int i = 0; i < NUM_LEDS; i++) {
    setLed(leds + i, color);
  }
}

void flashLeds(CRGB* leds, CRGB color, unsigned long intervalMsec, LED_STATE *ledState) {
  if (ledState->nextSwitch < millis()) {
    ledState->flashOn = !ledState->flashOn;
    ledState->nextSwitch = millis() + intervalMsec;
  }
  if (ledState->flashOn) {
    setLeds(leds, color);
  } else {
    setLeds(leds, colorBlack);
  }
}

void iterateLED(CRGB* leds, CRGB color, unsigned long intervalMsec, LED_STATE *ledState) {
  if (ledState->nextSwitch < millis()) {
    ledState->iterateCurrentLed = (ledState->iterateCurrentLed + 1) % NUM_LEDS;
    ledState->nextSwitch = millis() + intervalMsec;
  }
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i == ledState->iterateCurrentLed) {
      setLed(leds + i, color);
    } else {
      setLed(leds + i, colorBlack);
    }
  }
}

void aliveLeds(CRGB* leds, CRGB color, unsigned long intervalOnMsec, unsigned long intervalOffMsec, LED_STATE *ledState) {
  if (ledState->nextSwitch < millis()) {
    ledState->flashOn = !ledState->flashOn;
    if (ledState->flashOn) {
      ledState->nextSwitch = millis() + intervalOnMsec;
      // Flash only inner 2 LEDs
      setLeds(leds, colorBlack);
      setLed(leds+3, color);
      setLed(leds+4, color);
    } else {
      ledState->nextSwitch = millis() + intervalOffMsec;
      setLeds(leds, colorBlack);
    }
  }
}


void showBatteryState(CRGB* leds, LED_STATE *ledState) {
  if (ledState->nextSwitch < millis()) {
    ledState->flashOn = !ledState->flashOn;
    ledState->nextSwitch = millis() + 1000;
  }
  int percentage = isCharging() || !isUSBPowered() ? min(getBatteryPercentage(),99) : 100;
  int flashingLed = min(percentage * NUM_LEDS / 100, NUM_LEDS);
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i < flashingLed) {
      setLed(leds + i, colorYellow);
    } else if (i == flashingLed) {
      if (ledState->flashOn && isCharging()) {
        setLed(leds + i, colorYellow);
      } else {
        setLed(leds + i, colorBlack);
      }
    } else {
        setLed(leds + i, colorBlack);
    }
  }
}


unsigned long nextLedUpdate = 0;

void updateLeds() {
  if (nextLedUpdate > millis()) {
    return;
  }
  nextLedUpdate = millis() + 20;
  switch(state) {
    case STATE_STARTUP:
      setLeds(ledsFront, colorBlack);
      iterateLED(ledsBack, colorBlue, 100, ledStateBack);
      break;
    case STATE_WLAN_CONNECTED:
      setLeds(ledsFront, colorBlack);
      iterateLED(ledsBack, colorBlue, 200, ledStateBack);
      break;
    case STATE_NOT_CONNECTED:
      setLeds(ledsFront, colorBlack);
      flashLeds(ledsBack, colorBlue, 500, ledStateBack);
      break;
    case STATE_CONNECTING:
      setLeds(ledsFront, colorBlack);
      iterateLED(ledsBack, colorBlue, 500, ledStateBack);
      break;
    case STATE_WLAN_CONFIGURATION:
      setLeds(ledsFront, colorBlack);
      flashLeds(ledsBack, colorBlue, 2000, ledStateBack);
      break;
    case STATE_PRE_RESET:
      setLeds(ledsFront, colorBlack);
      flashLeds(ledsBack, colorRed, 50, ledStateBack);
      break;
    case STATE_CONNECTED:
      switch (lightValue) {
        case 0:
          setLeds(ledsFront, colorBlack);
          aliveLeds(ledsBack, colorWhite, 100, 10000, ledStateBack); // Indicate that the system is still on
          break;
        case 1:
          setLeds(ledsFront, colorRed);
          setLeds(ledsBack, colorRed);
          break;
        case 2:
          if (showPreviewOnFront) {
            setLeds(ledsFront, colorGreen);
          } else {
            setLeds(ledsFront, colorBlack);
          }
          if (showPreviewOnBack) {
            setLeds(ledsBack, colorGreen);
          } else {
            setLeds(ledsBack, colorBlack);
          }
          break;
        default:
          setLeds(ledsFront, colorBlack);
          flashLeds(ledsBack, colorRed, 200, ledStateBack);
          break;
      }
      break;
  }
  if (millis() > 200 && (isUSBPowered() || (showBatteryStateUntil > millis()))) {
    showBatteryState(ledsFront, ledStateFront);
  }
  FastLED.show();
}

void displayLoop( void * parameter) {
  while(1) {
    handleButton();
    updateLeds();
    yield();
  }
}

void configureRoutes(char* root) {
  server.serveStatic(root, SPIFFS, "/").setDefaultFile("config.html");

    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      DynamicJsonDocument doc(1000);
      doc["name"] = name;
      doc["vmixHost"] = vmixHost;
      doc["vmixPort"] = vmixPort;
      doc["lightIndex"] = lightIndex;
      doc["showPreviewOnFront"] = showPreviewOnFront;
      doc["showPreviewOnBack"] = showPreviewOnBack;
      serializeJson(doc, *response);
      request->send(response);
    });

    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *request) {
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      DynamicJsonDocument doc(1000);
      doc["batteryVoltage"] = readBatteryVoltage();
      doc["batteryPercentage"] = getBatteryPercentage();
      doc["isCharging"] = isCharging();
      doc["usbPowered"] = isUSBPowered();
      doc["wifiRssi"] = readWifiRssi();
      doc["brightness"] = brightness;
      doc["appVersion"] = VERSION;
      serializeJson(doc, *response);
      request->send(response);
    });

    AsyncCallbackJsonWebHandler *handler = new AsyncCallbackJsonWebHandler("/api/settings", [](AsyncWebServerRequest *request, JsonVariant &json) {
      JsonObject obj = json.as<JsonObject>();
      if (obj.containsKey("name")) {
        const char *val = obj["name"];
        name = val;
      }
      if (obj.containsKey("vmixHost")) {
        const char *val = obj["vmixHost"];
        vmixHost = val;
      }
      if (obj.containsKey("vmixPort")) {
        vmixPort = obj["vmixPort"];
      }
      if (obj.containsKey("lightIndex")) {
        lightIndex = obj["lightIndex"];
      }
      if (obj.containsKey("showPreviewOnFront")) {
        showPreviewOnFront = obj["showPreviewOnFront"];
      }
      if (obj.containsKey("showPreviewOnBack")) {
        showPreviewOnBack = obj["showPreviewOnBack"];
      }

      state = STATE_NOT_CONNECTED;
      writeStateToEeprom();
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      DynamicJsonDocument doc(100);
      doc["success"] = true;
      serializeJson(doc, *response);
      request->send(response);
    }, 1000);
    server.addHandler(handler);

    handler = new AsyncCallbackJsonWebHandler("/api/state", [](AsyncWebServerRequest *request, JsonVariant &json) {
      JsonObject obj = json.as<JsonObject>();
      if (obj.containsKey("brightness")) {
        brightness = obj["brightness"];
        writeBrightnessToEeprom();
      }
      state = STATE_NOT_CONNECTED;
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      DynamicJsonDocument doc(100);
      doc["success"] = true;
      serializeJson(doc, *response);
      request->send(response);
    }, 1000);
    server.addHandler(handler);
}

void configModeCallback(AsyncWiFiManager* wifiManager) {
  //Serial.println("Entered config mode");
  state = STATE_WLAN_CONFIGURATION;
}

void setup() {
  Serial.begin(9600);

  if(!SPIFFS.begin()){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // Setup button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(VBUS_MON_PIN, INPUT);

  // Setup charge pin
  pinMode(CHARGING_PIN, INPUT_PULLUP);

  // ADC for Battery measurement
  adc_power_acquire();
  adc1_config_width(ADC_WIDTH_12Bit);
  adc1_config_channel_atten(BATTERY_VOLTAGE_CHANNEL, ADC_ATTEN_DB_11);
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_11db, ADC_WIDTH_12Bit, ESP_ADC_CAL_VAL_DEFAULT_VREF, &adc_chars);

  // Setup LED
  FastLED.addLeds<LED_TYPE, LED_PIN_FRONT, COLOR_ORDER>(ledsFront, NUM_LEDS);
  FastLED.addLeds<LED_TYPE, LED_PIN_BACK, COLOR_ORDER>(ledsBack, NUM_LEDS);

  // Setup EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadStateFromEeprom();

  // Setup display thread
  xTaskCreatePinnedToCore(displayLoop, "DisplayTask", 10000, NULL, 1, &displayTaskHnd, 1);

  // Setup WIFI
  wifiManager = new AsyncWiFiManager(&server, &dns);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String setupWifiName = "TallyLight" + mac.substring(6, 12);
  wifiManager->setAPCallback(configModeCallback);
  wifiManager->autoConnect(setupWifiName.c_str());
  configureRoutes("/");
  server.begin();

  lightValue = 0;
  state = STATE_WLAN_CONNECTED;

  // Setup VMix client
  vmix->setAckTimeout(500);
  vmix->onConnect(onConnect, vmix);
  vmix->onData(onData, vmix);
  vmix->onDisconnect(onDisconnect, vmix);
}

void handleReset() {
  if (state != STATE_PRE_RESET) {
    return;
  }
  if (preResetStartedAt + 5000 < millis()) {
    resetFunc();
  }
}

void loop() {
  handleEeprom();
  handleVmix();
  handleReset();
  readBatteryVoltage();
}