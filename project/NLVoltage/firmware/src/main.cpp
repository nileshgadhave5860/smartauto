#include <Arduino.h>
#include <ArduinoJson.h>
#include <cmath>
#include <ModbusMaster.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>

constexpr uint16_t DEVICE_ID = 101;
constexpr char WIFI_SSID[] = "Ayush_EXT";
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
  "isAutoSinglePhase": false,
  "l1": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0},
  "l2": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0},
  "l3": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0},
  "ln": {"isVol": false, "minVol": 0, "maxVol": 0, "isA": false, "minA": 0, "maxA": 0}
})json";
constexpr char UNAVAILABLE_MEASUREMENTS_JSON[] = R"json([
  {"VName":"L1","VValue":0,"VStatus":-1,"AValue":0,"AStatus":-1},
  {"VName":"L2","VValue":0,"VStatus":-1,"AValue":0,"AStatus":-1},
  {"VName":"L3","VValue":0,"VStatus":-1,"AValue":0,"AStatus":-1},
  {"VName":"LN","VValue":0,"VStatus":-1,"AValue":0,"AStatus":-1}
])json";

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
constexpr uint16_t LN_VOLTAGE_REGISTER = 0;
constexpr uint16_t LN_CURRENT_REGISTER = 2;
constexpr float LN_VOLTAGE_SCALE = 1.0f;
constexpr float LN_CURRENT_SCALE = 1.0f;
// L1/L2/L3 float32 pairs (high word first). Voltages follow the L1 map (0, 2, 4);
// current registers must be confirmed against the meter register map.
constexpr uint16_t PHASE_VOLTAGE_REGISTER = 0;
constexpr uint16_t PHASE_CURRENT_REGISTER = 16;
constexpr float PHASE_VOLTAGE_SCALE = 1.0f;
constexpr float PHASE_CURRENT_SCALE = 1.0f;
constexpr uint32_t PHASE_CHECK_INTERVAL_MS = 30000;
constexpr uint32_t LN_READ_INTERVAL_MS = 2000;
constexpr uint32_t MEASUREMENT_PUBLISH_INTERVAL_MS = 60000;
constexpr uint32_t MEASUREMENT_MAX_AGE_MS = 10000;
constexpr uint32_t AVAILABILITY_PUBLISH_INTERVAL_MS = 10000;
constexpr uint8_t MAX_CONSECUTIVE_TIMEOUTS = 3;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 3000;
constexpr uint16_t MQTT_BUFFER_SIZE = 1024;

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
bool measurementsReceived = false;
bool unavailableMeasurementsPublished = false;
int autoStatus = 2;
int motorStatus = 2;
bool limitsLoaded = false;
bool singlePhaseMode = false;
bool lnReadRequested = true;
bool decisionPublished = false;
String lastDecisionReason;
uint32_t lastLnReadTime = 0;
bool availabilityRepublishRequested = false;
uint32_t lastMeasurementReceivedAt = 0;

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
  bool hasVoltage;
  bool hasCurrent;
  int voltageStatus;
  int currentStatus;
};

constexpr size_t PHASE_COUNT = 4;
const char* const PHASE_NAMES[PHASE_COUNT] = {"L1", "L2", "L3", "LN"};
PhaseLimit phaseLimits[PHASE_COUNT];
PhaseReading phaseReadings[PHASE_COUNT] = {
  {"L1", 0, 0, false, false, -1, -1},
  {"L2", 0, 0, false, false, -1, -1},
  {"L3", 0, 0, false, false, -1, -1},
  {"LN", 0, 0, false, false, -1, -1}
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

bool readInteger(JsonVariantConst value, int& number) {
  if (!value.is<int>()) {
    return false;
  }
  number = value.as<int>();
  return true;
}

JsonObjectConst getPhaseObject(JsonObjectConst root, const char* phaseName) {
  JsonObjectConst phase = root[phaseName].as<JsonObjectConst>();
  if (phase.isNull()) {
    char lowercaseName[3] = {};
    for (size_t index = 0; phaseName[index] != '\0'; ++index) {
      lowercaseName[index] = phaseName[index] >= 'A' && phaseName[index] <= 'Z'
          ? static_cast<char>(phaseName[index] + ('a' - 'A'))
          : phaseName[index];
    }
    phase = root[lowercaseName].as<JsonObjectConst>();
  }
  if (phase.isNull() && strcmp(phaseName, "L1") == 0) {
    phase = root["L"].as<JsonObjectConst>();
    if (phase.isNull()) {
      phase = root["l"].as<JsonObjectConst>();
    }
  } else if (phase.isNull() && strcmp(phaseName, "LN") == 0) {
    phase = root["N"].as<JsonObjectConst>();
    if (phase.isNull()) {
      phase = root["n"].as<JsonObjectConst>();
    }
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
  if (root.isNull()) {
    return false;
  }

  const JsonVariantConst singlePhase = root["isAutoSinglePhase"];
  bool parsedSinglePhaseMode = false;
  if (singlePhase.is<bool>()) {
    parsedSinglePhaseMode = singlePhase.as<bool>();
  } else if (singlePhase.isNull()) {
    const char* legacyMode = root["mode"];
    if (legacyMode != nullptr) {
      if (strcmp(legacyMode, "LN") == 0) {
        parsedSinglePhaseMode = true;
      } else if (strcmp(legacyMode, "L1L2L3") != 0) {
        return false;
      }
    }
  } else {
    return false;
  }

  PhaseLimit parsedLimits[PHASE_COUNT];
  for (size_t index = 0; index < PHASE_COUNT; ++index) {
    const JsonObjectConst phase = getPhaseObject(root, PHASE_NAMES[index]);
    if (phase.isNull()) {
      parsedLimits[index] = PhaseLimit{};
    } else if (!parseLimitPhase(phase, parsedLimits[index])) {
      return false;
    }
  }

  for (size_t index = 0; index < PHASE_COUNT; ++index) {
    phaseLimits[index] = parsedLimits[index];
  }
  singlePhaseMode = parsedSinglePhaseMode;
  return true;
}

void clearMeasurements() {
  for (PhaseReading& reading : phaseReadings) {
    reading.voltage = 0;
    reading.current = 0;
    reading.hasVoltage = false;
    reading.hasCurrent = false;
    reading.voltageStatus = -1;
    reading.currentStatus = -1;
  }
}

bool parseMeasurements(const String& json) {
  JsonDocument document;
  if (deserializeJson(document, json) || !document.is<JsonArray>()) {
    return false;
  }

  clearMeasurements();

  for (JsonObjectConst item : document.as<JsonArrayConst>()) {
    const char* name = item["VName"];
    if (name == nullptr) {
      continue;
    }

    const char* normalizedName = strcmp(name, "L") == 0 ? "L1"
        : strcmp(name, "N") == 0 ? "LN"
        : name;
    for (PhaseReading& reading : phaseReadings) {
      if (strcmp(normalizedName, reading.name) == 0) {
        reading.hasVoltage = readNumber(item["VValue"], reading.voltage)
          && readInteger(item["VStatus"], reading.voltageStatus);
        reading.hasCurrent = readNumber(item["AValue"], reading.current)
          && readInteger(item["AStatus"], reading.currentStatus);
        break;
      }
    }
  }
  return true;
}

// Returns true only when this phase has every enabled value inside its inclusive limits.
bool checkPhaseStatus(const PhaseReading& reading, const PhaseLimit& limit, String& reason) {
  if (limit.voltageEnabled && !reading.hasVoltage) {
    reason = String(reading.name) + " voltage measurement is missing or invalid";
    return false;
  }
  if (limit.currentEnabled && !reading.hasCurrent) {
    reason = String(reading.name) + " current measurement is missing or invalid";
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

bool decodeFloatRegisterPair(uint8_t responseIndex, float scale, float& value) {
  const uint32_t rawValue =
      (static_cast<uint32_t>(meter.getResponseBuffer(responseIndex)) << 16)
      | meter.getResponseBuffer(responseIndex + 1);
  std::memcpy(&value, &rawValue, sizeof(value));
  value *= scale;
  return std::isfinite(value);
}

int measurementStatus(bool enabled, float value, float minimum, float maximum) {
  if (!enabled) {
    return 0;
  }
  return value >= minimum && value <= maximum ? 1 : 2;
}

bool readLnMeasurements() {
  if (!singlePhaseMode) {
    return false;
  }

  const uint8_t result = meter.readInputRegisters(LN_VOLTAGE_REGISTER, 4);
  if (result != meter.ku8MBSuccess) {
    Serial.printf("LN input-register read failed at %u: 0x%02X\n",
        LN_VOLTAGE_REGISTER,
        result);
    // Keep the last good reading; it expires through MEASUREMENT_MAX_AGE_MS.
    return false;
  }

  float voltage = 0;
  float current = 0;
  const uint8_t currentOffset = LN_CURRENT_REGISTER - LN_VOLTAGE_REGISTER;
  if (!decodeFloatRegisterPair(0, LN_VOLTAGE_SCALE, voltage)
      || !decodeFloatRegisterPair(currentOffset, LN_CURRENT_SCALE, current)) {
    Serial.println("LN voltage/current register value is not a finite float.");
    clearMeasurements();
    measurementsReceived = false;
    return false;
  }

  constexpr size_t LN_PHASE_INDEX = 3;
  PhaseReading& reading = phaseReadings[LN_PHASE_INDEX];
  const PhaseLimit& limit = phaseLimits[LN_PHASE_INDEX];
  reading.voltage = voltage;
  reading.current = current;
  reading.hasVoltage = true;
  reading.hasCurrent = true;
  reading.voltageStatus = measurementStatus(
      limit.voltageEnabled, voltage, limit.minVoltage, limit.maxVoltage);
  reading.currentStatus = measurementStatus(
      limit.currentEnabled, current, limit.minCurrent, limit.maxCurrent);
  measurementsReceived = true;
  lastMeasurementReceivedAt = millis();
  unavailableMeasurementsPublished = false;

  JsonDocument document;
  JsonArray readings = document.to<JsonArray>();
  JsonObject ln = readings.add<JsonObject>();
  ln["VName"] = "LN";
  ln["VValue"] = voltage;
  ln["VStatus"] = reading.voltageStatus;
  ln["AValue"] = current;
  ln["AStatus"] = reading.currentStatus;
  lastMeasurementJson = "";
  serializeJson(document, lastMeasurementJson);

  if (mqttClient.connected()
      && !mqttClient.publish(MEASUREMENT_TOPIC, lastMeasurementJson.c_str(), true)) {
    Serial.println("Failed to publish LN voltage/current measurement.");
    return false;
  }

  Serial.printf("LN voltage: %.3f V, current: %.3f A\n", voltage, current);
  return true;
}

void updateRelay();

bool readThreePhaseMeasurements() {
  if (singlePhaseMode) {
    return false;
  }

  float voltages[3] = {0, 0, 0};
  float currents[3] = {0, 0, 0};
  bool hasCurrentValues = false;

  uint8_t result = meter.readInputRegisters(PHASE_VOLTAGE_REGISTER, 6);
  if (result != meter.ku8MBSuccess) {
    Serial.printf("L1-L3 voltage read failed at %u: 0x%02X\n", PHASE_VOLTAGE_REGISTER, result);
    return false;
  }
  for (uint8_t phase = 0; phase < 3; ++phase) {
    if (!decodeFloatRegisterPair(phase * 2, PHASE_VOLTAGE_SCALE, voltages[phase])) {
      Serial.println("L1-L3 voltage register value is not a finite float.");
      return false;
    }
  }

  bool needCurrent = false;
  for (size_t index = 0; index < 3; ++index) {
    needCurrent = needCurrent || phaseLimits[index].currentEnabled;
  }
  if (needCurrent) {
    result = meter.readInputRegisters(PHASE_CURRENT_REGISTER, 6);
    if (result != meter.ku8MBSuccess) {
      Serial.printf("L1-L3 current read failed at %u: 0x%02X\n", PHASE_CURRENT_REGISTER, result);
      return false;
    }
    for (uint8_t phase = 0; phase < 3; ++phase) {
      if (!decodeFloatRegisterPair(phase * 2, PHASE_CURRENT_SCALE, currents[phase])) {
        Serial.println("L1-L3 current register value is not a finite float.");
        return false;
      }
    }
    hasCurrentValues = true;
  }

  JsonDocument document;
  JsonArray readings = document.to<JsonArray>();
  for (size_t index = 0; index < 3; ++index) {
    PhaseReading& reading = phaseReadings[index];
    const PhaseLimit& limit = phaseLimits[index];
    reading.voltage = voltages[index];
    reading.current = currents[index];
    reading.hasVoltage = true;
    reading.hasCurrent = hasCurrentValues;
    reading.voltageStatus = measurementStatus(
        limit.voltageEnabled, reading.voltage, limit.minVoltage, limit.maxVoltage);
    reading.currentStatus = hasCurrentValues
        ? measurementStatus(limit.currentEnabled, reading.current, limit.minCurrent, limit.maxCurrent)
        : -1;

    JsonObject item = readings.add<JsonObject>();
    item["VName"] = reading.name;
    item["VValue"] = reading.voltage;
    item["VStatus"] = reading.voltageStatus;
    item["AValue"] = reading.current;
    item["AStatus"] = reading.currentStatus;
  }
  measurementsReceived = true;
  lastMeasurementReceivedAt = millis();
  unavailableMeasurementsPublished = false;
  lastMeasurementJson = "";
  serializeJson(document, lastMeasurementJson);

  if (mqttClient.connected()
      && !mqttClient.publish(MEASUREMENT_TOPIC, lastMeasurementJson.c_str(), true)) {
    Serial.println("Failed to publish L1-L3 measurements.");
  }
  Serial.printf("L1 %.1f V, L2 %.1f V, L3 %.1f V | %.2f A, %.2f A, %.2f A\n",
      voltages[0], voltages[1], voltages[2], currents[0], currents[1], currents[2]);
  return true;
}

void publishMotorDecision(bool shouldRun, const String& reason) {
  const int newStatus = shouldRun ? 1 : 2;
  const bool changed = !decisionPublished || newStatus != motorStatus || reason != lastDecisionReason;
  if (newStatus != motorStatus || !motorStatusReceived) {
    preferences.putInt("status", newStatus);
  }
  motorStatus = newStatus;
  motorStatusReceived = true;
  updateRelay();

  if (!changed) {
    return;
  }
  lastDecisionReason = reason;
  if (mqttClient.connected()) {
    mqttClient.publish(MOTOR_STATUS_TOPIC, shouldRun ? "1" : "2", true);
    mqttClient.publish(MOTOR_REASON_TOPIC, reason.c_str(), true);
    decisionPublished = true;
  }
  Serial.printf("Motor %s: %s\n", shouldRun ? "ON" : "OFF", reason.c_str());
}

void publishMeasurementsIfDue() {
  const uint32_t now = millis();
  if (!mqttClient.connected()) {
    Serial.println("Cannot publish measurements: MQTT is disconnected.");
    return;
  }

  const bool measurementsStale = !measurementsReceived
      || now - lastMeasurementReceivedAt > MEASUREMENT_MAX_AGE_MS;
  if (measurementsStale) {
    if (unavailableMeasurementsPublished) {
      return;
    }
    unavailableMeasurementsPublished = true;
    lastMeasurementPublishTime = now;
    clearMeasurements();
    lastMeasurementJson = UNAVAILABLE_MEASUREMENTS_JSON;
    mqttClient.publish(MEASUREMENT_TOPIC, UNAVAILABLE_MEASUREMENTS_JSON, true);
    Serial.println("Published zero measurements: MFM data is unavailable or stale.");
    return;
  }

  unavailableMeasurementsPublished = false;
  if (now - lastMeasurementPublishTime < MEASUREMENT_PUBLISH_INTERVAL_MS) {
    return;
  }
  lastMeasurementPublishTime = now;

  if (lastMeasurementJson.isEmpty()) {
    Serial.println("No MQTT phase measurements available to publish.");
    return;
  }

  mqttClient.publish(MEASUREMENT_TOPIC, lastMeasurementJson.c_str(), true);
  Serial.println("Published latest measurements.");
}

void publishAvailabilityIfDue(bool force = false) {
  const uint32_t now = millis();
  force = force || availabilityRepublishRequested;
  if (WiFi.status() != WL_CONNECTED
      || !mqttClient.connected()
      || (!force && now - lastAvailabilityPublishTime < AVAILABILITY_PUBLISH_INTERVAL_MS)) {
    return;
  }

  availabilityRepublishRequested = false;
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
    publishMotorDecision(false, "phase limits are unavailable");
    return;
  }
  if (!measurementsReceived
      || millis() - lastMeasurementReceivedAt > MEASUREMENT_MAX_AGE_MS) {
    publishMotorDecision(false, "measurements are unavailable or stale");
    return;
  }

  for (size_t index = 0; index < PHASE_COUNT; ++index) {
    if (singlePhaseMode ? index != 3 : index == 3) {
      continue;
    }
    const PhaseLimit& limit = phaseLimits[index];
    String reason;
    if (!checkPhaseStatus(phaseReadings[index], limit, reason)) {
      publishMotorDecision(false, reason);
      return;
    }
  }

  publishMotorDecision(true, "all phase statuses and enabled limits are OK");
}

void updateRelay() {
  const bool hasFreshMeasurements = measurementsReceived
      && millis() - lastMeasurementReceivedAt <= MEASUREMENT_MAX_AGE_MS;
  const bool shouldRun = limitsLoaded
      && hasFreshMeasurements
      && autoStatusReceived
      && motorStatusReceived
      && autoStatus == 1
      && motorStatus == 1;
  digitalWrite(RELAY_PIN, shouldRun ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
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
      lnReadRequested = true;
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
    // The device is the source of readings; ignore echoes and stale retained data.
    return;
  } else if (strcmp(topic, MOTOR_REASON_TOPIC) == 0) {
    Serial.printf("Motor reason: %s\n", message.c_str());
  } else if (strcmp(topic, AVAILABILITY_TOPIC) == 0) {
    Serial.printf("Retained device availability: %s\n", message.c_str());
    // A stale last-will "offline" must not override a live device.
    if (message != "online") {
      lastAvailabilityPublishTime = 0;
      availabilityRepublishRequested = true;
    }
  }
}

void maintainConnections() {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiAttempted || now - lastWifiAttempt >= WIFI_RETRY_INTERVAL_MS) {
      wifiAttempted = true;
      lastWifiAttempt = now;
      WiFi.mode(WIFI_STA);
      // Lower TX power reduces current spikes that trigger brownout on weak supplies.
      WiFi.setTxPower(WIFI_POWER_11dBm);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      Serial.println("Connecting to Wi-Fi...");
    }
    return;
  }

  // The relay keeps following local readings while the network is down.
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
      mqttClient.subscribe(MOTOR_REASON_TOPIC);
      decisionPublished = false;
      lnReadRequested = true;
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
  motorStatus = 2;
  lastLimitJson = preferences.getString("limits", "");
  limitsLoaded = lastLimitJson.length() > 0 && parseLimits(lastLimitJson);
  if (!limitsLoaded) {
    lastLimitJson = DEFAULT_LIMITS_JSON;
    limitsLoaded = parseLimits(lastLimitJson);
    if (limitsLoaded) {
      preferences.putString("limits", lastLimitJson);
    }
  }
  autoStatusReceived = true;
  motorStatusReceived = false;
  updateRelay();
  Serial.printf("Initialized auto status %d, motor status %d, limits %u bytes.\n",
      autoStatus,
      motorStatus,
      static_cast<unsigned int>(lastLimitJson.length()));

  // Four-phase limit and measurement JSON exceeds PubSubClient's default 256-byte packet.
  if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE)) {
    Serial.println("Failed to allocate MQTT packet buffer.");
  }
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

  delay(3000);
  maintainConnections();
  readLnMeasurements();
  readThreePhaseMeasurements();
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
      if (millis() - lastLnReadTime >= LN_READ_INTERVAL_MS) {
        lnReadRequested = true;
      }
      if (lnReadRequested) {
        lnReadRequested = false;
        lastLnReadTime = millis();
        if (singlePhaseMode) {
          readLnMeasurements();
        } else {
          readThreePhaseMeasurements();
        }
        evaluateMotor();
      }
      delay(10);
    }

    evaluateMotor();
    publishAvailabilityIfDue();
    publishMeasurementsIfDue();
    lastScanTime = millis();
  } while (true);
}
