#include <Arduino.h>
#include <ArduinoJson.h>
#include <ModbusMaster.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>

constexpr uint16_t DEVICE_ID = 101;
constexpr char WIFI_SSID[] = "SmartAuto";
constexpr char WIFI_PASSWORD[] = "Ayush@5860";
constexpr char MQTT_HOST[] = "88.222.213.221";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char AVAILABILITY_TOPIC[] = "smartauto/device/101/availability";
constexpr char AUTO_STATUS_TOPIC[] = "smartauto/device/101/auto/status";
constexpr char LIMIT_TOPIC[] = "smartauto/device/101/limit";
constexpr char MEASUREMENT_TOPIC[] = "smartauto/device/101/measurement";
constexpr char MOTOR_STATUS_TOPIC[] = "smartauto/device/101/status";
constexpr char MOTOR_REASON_TOPIC[] = "smartauto/device/101/status/reason";
constexpr char DEFAULT_LIMITS_JSON[] = R"json({
  "n": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0},
  "l": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0},
  "r": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0},
  "b": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0},
  "y": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0}
})json";

constexpr uint32_t METER_BAUD = 9600;
constexpr uint8_t METER_SLAVE_ID = 1;
constexpr int8_t RS485_RX_PIN = 16;
constexpr int8_t RS485_TX_PIN = 17;
constexpr int8_t RS485_DIRECTION_PIN = 4;
constexpr uint8_t RELAY_PIN = 26;
constexpr uint8_t RELAY_ON_LEVEL = LOW;
constexpr uint8_t RELAY_OFF_LEVEL = HIGH;
constexpr uint16_t FIRST_REGISTER = 0;
constexpr uint16_t LAST_REGISTER = 255;
constexpr uint32_t PHASE_CHECK_INTERVAL_MS = 30000;
constexpr uint32_t MEASUREMENT_PUBLISH_INTERVAL_MS = 60000;
constexpr uint32_t AVAILABILITY_PUBLISH_INTERVAL_MS = 30000;
constexpr uint8_t MAX_CONSECUTIVE_TIMEOUTS = 3;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 3000;

HardwareSerial meterSerial(2);
ModbusMaster meter;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Preferences preferences;
String lastLimitJson;
String lastMeasurementJson;
uint32_t lastScanTime = 0;
uint32_t lastMeasurementPublishTime = 0;
uint32_t lastAvailabilityPublishTime = 0;
uint32_t lastWifiAttempt = 0;
uint32_t lastMqttAttempt = 0;
bool wifiAttempted = false;
bool autoStatusReceived = false;
bool motorStatusReceived = false;
int autoStatus = 2;
int motorStatus = 2;
bool limitsLoaded = false;

struct PhaseLimit {
  bool voltageEnabled = false;
  float minVoltage = 0;
  float maxVoltage = 0;
  bool currentEnabled = false;
  float minCurrent = 0;
  float maxCurrent = 0;
};

struct PhaseReading {
  const char* name;
  float voltage;
  float current;
  bool received;
};

constexpr size_t PHASE_COUNT = 5;
const char* const PHASE_NAMES[PHASE_COUNT] = {"L", "N", "R", "B", "Y"};
PhaseLimit phaseLimits[PHASE_COUNT];
PhaseReading phaseReadings[PHASE_COUNT] = {
  {"L", 0, 0, false},
  {"N", 0, 0, false},
  {"R", 0, 0, false},
  {"B", 0, 0, false},
  {"Y", 0, 0, false}
};

bool readBoolean(JsonObjectConst object, const char* key, const char* alternateKey, bool& value) {
  const JsonVariantConst primary = object[key];
  if (primary.is<bool>()) {
    value = primary.as<bool>();
    return true;
  }

  const JsonVariantConst alternate = object[alternateKey];
  if (alternate.is<bool>()) {
    value = alternate.as<bool>();
    return true;
  }
  return false;
}

bool readNumber(JsonVariantConst value, float& number) {
  if (!value.is<int>() && !value.is<float>()) {
    return false;
  }
  number = value.as<float>();
  return true;
}

JsonObjectConst getPhaseObject(JsonObjectConst root, const char* phaseName) {
  JsonObjectConst phase = root[phaseName].as<JsonObjectConst>();
  if (phase.isNull()) {
    char lowercaseName[2] = {
      static_cast<char>(phaseName[0] + ('a' - 'A')),
      '\0'
    };
    phase = root[lowercaseName].as<JsonObjectConst>();
  }
  return phase;
}

bool parseLimitPhase(JsonObjectConst object, PhaseLimit& limit) {
  return readBoolean(object, "isVol", "flagVol", limit.voltageEnabled)
      && readNumber(object["minVol"], limit.minVoltage)
      && readNumber(object["maxVol"], limit.maxVoltage)
      && readBoolean(object, "isA", "flagA", limit.currentEnabled)
      && readNumber(object["minA"], limit.minCurrent)
      && readNumber(object["maxA"], limit.maxCurrent)
      && (!limit.voltageEnabled || limit.minVoltage <= limit.maxVoltage)
      && (!limit.currentEnabled || limit.minCurrent <= limit.maxCurrent);
}

bool parseLimits(const String& json) {
  JsonDocument document;
  if (deserializeJson(document, json)) {
    return false;
  }

  const JsonObjectConst root = document.as<JsonObjectConst>();
  PhaseLimit parsedLimits[PHASE_COUNT];
  for (size_t index = 0; index < PHASE_COUNT; ++index) {
    if (!parseLimitPhase(getPhaseObject(root, PHASE_NAMES[index]), parsedLimits[index])) {
      return false;
    }
  }

  for (size_t index = 0; index < PHASE_COUNT; ++index) {
    phaseLimits[index] = parsedLimits[index];
  }
  return true;
}

bool parseMeasurements(const String& json) {
  JsonDocument document;
  if (deserializeJson(document, json) || !document.is<JsonArray>()) {
    return false;
  }

  for (PhaseReading& reading : phaseReadings) {
    reading.received = false;
  }

  for (JsonObjectConst item : document.as<JsonArrayConst>()) {
    const char* name = item["VName"];
    if (name == nullptr) {
      continue;
    }

    for (PhaseReading& reading : phaseReadings) {
      if (strcmp(name, reading.name) == 0) {
        reading.received = readNumber(item["VValue"], reading.voltage)
            && readNumber(item["AValue"], reading.current);
        break;
      }
    }
  }
  return true;
}

// Returns true only when this phase has every enabled value inside its inclusive limits.
bool checkPhaseStatus(const PhaseReading& reading, const PhaseLimit& limit, String& reason) {
  if (!limit.voltageEnabled && !limit.currentEnabled) {
    return true;
  }
  if (!reading.received) {
    reason = String(reading.name) + " measurement is missing or invalid";
    return false;
  }
  if (limit.voltageEnabled
      && (reading.voltage < limit.minVoltage || reading.voltage > limit.maxVoltage)) {
    reason = String(reading.name) + " voltage is outside its limits";
    return false;
  }
  if (limit.currentEnabled
      && (reading.current < limit.minCurrent || reading.current > limit.maxCurrent)) {
    reason = String(reading.name) + " current is outside its limits";
    return false;
  }
  return true;
}

void updateRelay();

void publishMotorDecision(bool shouldRun, const String& reason) {
  motorStatus = shouldRun ? 1 : 2;
  motorStatusReceived = true;
  preferences.putInt("status", motorStatus);
  updateRelay();

  if (mqttClient.connected()) {
    mqttClient.publish(MOTOR_STATUS_TOPIC, shouldRun ? "1" : "2", true);
    mqttClient.publish(MOTOR_REASON_TOPIC, reason.c_str(), true);
  }
  Serial.printf("Motor %s: %s\n", shouldRun ? "ON" : "OFF", reason.c_str());
}

void publishMeasurementsIfDue() {
  const uint32_t now = millis();
  if (now - lastMeasurementPublishTime < MEASUREMENT_PUBLISH_INTERVAL_MS) {
    return;
  }
  lastMeasurementPublishTime = now;

  if (lastMeasurementJson.isEmpty()) {
    Serial.println("No MQTT phase measurements available to publish.");
    return;
  }
  if (!mqttClient.connected()) {
    Serial.println("Cannot publish measurements: MQTT is disconnected.");
    return;
  }

  mqttClient.publish(MEASUREMENT_TOPIC, lastMeasurementJson.c_str(), true);
  Serial.println("Published latest measurements.");
}

void publishAvailabilityIfDue(bool force = false) {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED
      || !mqttClient.connected()
      || (!force && now - lastAvailabilityPublishTime < AVAILABILITY_PUBLISH_INTERVAL_MS)) {
    return;
  }

  lastAvailabilityPublishTime = now;
  const bool published = mqttClient.publish(AVAILABILITY_TOPIC, "online", true);
  Serial.printf("Availability heartbeat: %s\n", published ? "online" : "publish failed");
}

void evaluateMotor() {
  if (!autoStatusReceived || autoStatus != 1) {
    publishMotorDecision(false, "auto status is off or unavailable");
    return;
  }
  if (!limitsLoaded) {
    updateRelay();
    Serial.println("Phase limits unavailable; keeping the latest motor status.");
    return;
  }

  bool anyLimitEnabled = false;
  for (size_t index = 0; index < PHASE_COUNT; ++index) {
    const PhaseLimit& limit = phaseLimits[index];
    anyLimitEnabled = anyLimitEnabled || limit.voltageEnabled || limit.currentEnabled;
    if ((limit.voltageEnabled || limit.currentEnabled) && !phaseReadings[index].received) {
      motorStatus = 2;
      motorStatusReceived = true;
      updateRelay();
      Serial.printf("Waiting for %s measurement; relay held OFF.\n", phaseReadings[index].name);
      return;
    }

    String reason;
    if (!checkPhaseStatus(phaseReadings[index], limit, reason)) {
      publishMotorDecision(false, reason);
      return;
    }
  }

    publishMotorDecision(true, anyLimitEnabled
      ? "phase limits are within range"
      : "auto is on; no phase limits are enabled");
}

void updateRelay() {
  const bool shouldRun = autoStatusReceived
      && motorStatusReceived
      && autoStatus == 1
      && motorStatus == 1;
  digitalWrite(RELAY_PIN, shouldRun ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

bool anyPhaseLimitEnabled() {
  for (const PhaseLimit& limit : phaseLimits) {
    if (limit.voltageEnabled || limit.currentEnabled) {
      return true;
    }
  }
  return false;
}

void handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int index = 0; index < length; ++index) {
    message += static_cast<char>(payload[index]);
  }

  if (strcmp(topic, LIMIT_TOPIC) == 0) {
    if (parseLimits(message)) {
      lastLimitJson = message;
      limitsLoaded = true;
      preferences.putString("limits", lastLimitJson);
      Serial.println("Saved current limit configuration.");
      evaluateMotor();
    } else {
      limitsLoaded = false;
      Serial.println("Rejected invalid limit configuration.");
      evaluateMotor();
    }
  } else if (strcmp(topic, AUTO_STATUS_TOPIC) == 0) {
    autoStatus = message == "1" ? 1 : 2;
    autoStatusReceived = true;
    preferences.putInt("auto", autoStatus);
    if (autoStatus == 2) {
      motorStatus = 2;
      preferences.putInt("status", motorStatus);
    }
    evaluateMotor();
    Serial.printf("Auto status: %d\n", autoStatus);
  } else if (strcmp(topic, MEASUREMENT_TOPIC) == 0) {
    if (parseMeasurements(message)) {
      lastMeasurementJson = message;
      evaluateMotor();
    } else {
      Serial.println("Rejected invalid measurement payload.");
      publishMotorDecision(false, "measurement payload is invalid");
    }
  } else if (strcmp(topic, MOTOR_STATUS_TOPIC) == 0) {
    motorStatus = message == "1" ? 1 : 2;
    motorStatusReceived = true;
    preferences.putInt("status", motorStatus);
    updateRelay();
    Serial.printf("Motor status: %d\n", motorStatus);
  } else if (strcmp(topic, MOTOR_REASON_TOPIC) == 0) {
    Serial.printf("Motor reason: %s\n", message.c_str());
  } else if (strcmp(topic, AVAILABILITY_TOPIC) == 0) {
    Serial.printf("Retained device availability: %s\n", message.c_str());
  }
}

void maintainConnections() {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiAttempted || now - lastWifiAttempt >= WIFI_RETRY_INTERVAL_MS) {
      wifiAttempted = true;
      lastWifiAttempt = now;
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      Serial.println("Connecting to Wi-Fi...");
    }
    return;
  }

  if (!mqttClient.connected() && now - lastMqttAttempt >= MQTT_RETRY_INTERVAL_MS) {
    lastMqttAttempt = now;
    String clientId = "ESP32_";
    clientId += String(DEVICE_ID);
    clientId += "_";
    clientId += String(random(10000));

    if (mqttClient.connect(
            clientId.c_str(), nullptr, nullptr, AVAILABILITY_TOPIC, 1, true, "offline")) {
      publishAvailabilityIfDue(true);
      mqttClient.subscribe(AVAILABILITY_TOPIC);
      mqttClient.subscribe(AUTO_STATUS_TOPIC);
      mqttClient.subscribe(LIMIT_TOPIC);
      mqttClient.subscribe(MEASUREMENT_TOPIC);
      mqttClient.subscribe(MOTOR_STATUS_TOPIC);
      mqttClient.subscribe(MOTOR_REASON_TOPIC);
      Serial.println("Connected to MQTT and subscribed to device topics.");
    } else {
      Serial.printf("MQTT connection failed: %d\n", mqttClient.state());
    }
  }
}

void enableTransmit() {
  digitalWrite(RS485_DIRECTION_PIN, HIGH);
}

void enableReceive() {
  digitalWrite(RS485_DIRECTION_PIN, LOW);
}

void keepMqttAlive() {
  mqttClient.loop();
}

bool scanRegisterRange(bool inputRegisters, uint16_t& successfulReads) {
  uint8_t consecutiveTimeouts = 0;
  for (uint32_t address = FIRST_REGISTER; address <= LAST_REGISTER; ++address) {
    const uint8_t result = inputRegisters
        ? meter.readInputRegisters(static_cast<uint16_t>(address), 1)
        : meter.readHoldingRegisters(static_cast<uint16_t>(address), 1);

    if (result == meter.ku8MBSuccess) {
      Serial.printf("%s 0x%04X = %u\n",
          inputRegisters ? "INPUT" : "HOLDING",
          static_cast<unsigned int>(address),
          meter.getResponseBuffer(0));
      ++successfulReads;
      consecutiveTimeouts = 0;
    } else if (result == ModbusMaster::ku8MBResponseTimedOut) {
      ++consecutiveTimeouts;
      if (consecutiveTimeouts >= MAX_CONSECUTIVE_TIMEOUTS) {
        Serial.println("No Modbus response. Check RS-485 wiring, parity, baud, and slave ID.");
        return false;
      }
    } else {
      consecutiveTimeouts = 0;
    }

    mqttClient.loop();
    publishAvailabilityIfDue();
    delay(5);
  }

  return true;
}

void scanRegisters() {
  uint16_t successfulReads = 0;
  Serial.println("Starting read-only Modbus scan (registers 0-255).");

  if (scanRegisterRange(false, successfulReads)) {
    scanRegisterRange(true, successfulReads);
  }

  Serial.printf("Scan complete. Successful register reads: %u\n", successfulReads);
}

void setup() {
  Serial.begin(115200);
  randomSeed(micros());

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);

  preferences.begin("motor101", false);
  autoStatus = preferences.getInt("auto", 2);
  motorStatus = preferences.getInt("status", 2);
  lastLimitJson = preferences.getString("limits", "");
  limitsLoaded = lastLimitJson.length() > 0 && parseLimits(lastLimitJson);
  if (!limitsLoaded) {
    lastLimitJson = DEFAULT_LIMITS_JSON;
    limitsLoaded = parseLimits(lastLimitJson);
    if (limitsLoaded) {
      preferences.putString("limits", lastLimitJson);
    }
  }
    autoStatusReceived = preferences.isKey("auto") && autoStatus == 1;
    const bool savedMotorStatusExists = preferences.isKey("status")
      && motorStatus == 1;
    motorStatusReceived = preferences.isKey("status")
      && (motorStatus == 2 || (savedMotorStatusExists
        && limitsLoaded
        && !anyPhaseLimitEnabled()));
    updateRelay();
  Serial.printf("Restored auto status %d, motor status %d, limits %u bytes.\n",
      autoStatus,
      motorStatus,
      static_cast<unsigned int>(lastLimitJson.length()));

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(handleMqttMessage);
  mqttClient.setKeepAlive(60);

  pinMode(RS485_DIRECTION_PIN, OUTPUT);
  enableReceive();

  meterSerial.begin(METER_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  meter.begin(METER_SLAVE_ID, meterSerial);
  meter.preTransmission(enableTransmit);
  meter.postTransmission(enableReceive);
  meter.idle(keepMqttAlive);

  delay(1000);
  maintainConnections();
  scanRegisters();
  lastScanTime = millis();
  lastMeasurementPublishTime = millis();
}

void loop() {
  maintainConnections();
  mqttClient.loop();

  do {
    while (millis() - lastScanTime < PHASE_CHECK_INTERVAL_MS) {
      maintainConnections();
      mqttClient.loop();
      publishAvailabilityIfDue();
      publishMeasurementsIfDue();
      delay(10);
    }

    scanRegisters();
    evaluateMotor();
    publishAvailabilityIfDue();
    publishMeasurementsIfDue();
    lastScanTime = millis();
  } while (true);
}