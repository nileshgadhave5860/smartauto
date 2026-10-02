#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "Ayush";
const char* WIFI_PASSWORD = "Ayush@5860";

const char* MQTT_SERVER = "88.222.213.221";
const int MQTT_PORT = 1883;

const char* MQTT_TOPIC = "smartauto/device/101";
const char* LIMIT_TOPIC = "smartauto/device/101/limit";
const char* STATUS_TOPIC = "smartauto/device/101/status";
const char* MEASUREMENT_TOPIC = "smartauto/device/101/measurement";
const unsigned long MEASUREMENT_INTERVAL_MS = 1000;

#define RELAY_PIN 26

// Your working relay test showed Active LOW
#define RELAY_ON LOW
#define RELAY_OFF HIGH

WiFiClient espClient;
PubSubClient mqttClient(espClient);

Preferences preferences;

int lastStatus;

struct PhaseLimit
{
  bool flagVol;
  float minVol;
  float maxVol;
  bool flagA;
  float minA;
  float maxA;
};

struct DeviceLimits
{
  PhaseLimit R;
  PhaseLimit B;
  PhaseLimit Y;
};

DeviceLimits limits;

// Demo readings only. Replace these values with actual sensor measurements.
float measuredVoltage[3] = {400, 400, 400};
float measuredCurrent[3] = {12, 12, 12};
unsigned long lastMeasurementPublishMs = 0;

String defaultLimits = R"json({
  "device": 101,
  "R": {"flagVol": true, "minVol": 315, "maxVol": 450, "flagA": true, "minA": 10, "maxA": 15},
  "B": {"flagVol": true, "minVol": 315, "maxVol": 450, "flagA": true, "minA": 10, "maxA": 15},
  "Y": {"flagVol": true, "minVol": 315, "maxVol": 450, "flagA": true, "minA": 10, "maxA": 15}
})json";

bool parseBoolean(JsonObjectConst object, const char* primaryKey, const char* alternateKey, bool& value)
{
  JsonVariantConst primaryValue = object[primaryKey];
  if (primaryValue.is<bool>())
  {
    value = primaryValue.as<bool>();
    return true;
  }

  JsonVariantConst alternateValue = object[alternateKey];
  if (alternateValue.is<bool>())
  {
    value = alternateValue.as<bool>();
    return true;
  }

  return false;
}

bool parsePhase(JsonObjectConst object, PhaseLimit& phase)
{
  if (!parseBoolean(object, "flagVol", "isVol", phase.flagVol) ||
      !(object["minVol"].is<int>() || object["minVol"].is<float>()) ||
      !(object["maxVol"].is<int>() || object["maxVol"].is<float>()) ||
      !parseBoolean(object, "flagA", "isA", phase.flagA) ||
      !(object["minA"].is<int>() || object["minA"].is<float>()) ||
      !(object["maxA"].is<int>() || object["maxA"].is<float>()))
  {
    return false;
  }

  phase.minVol = object["minVol"].as<float>();
  phase.maxVol = object["maxVol"].as<float>();
  phase.minA = object["minA"].as<float>();
  phase.maxA = object["maxA"].as<float>();

  return phase.minVol <= phase.maxVol && phase.minA <= phase.maxA;
}

bool parseLimits(const String& json, DeviceLimits& parsedLimits)
{
  JsonDocument document;
  if (deserializeJson(document, json))
  {
    return false;
  }

  if ((!document["device"].isNull() && document["device"].as<int>() != 101))
  {
    return false;
  }

  JsonObjectConst phaseR = document["R"].as<JsonObjectConst>();
  JsonObjectConst phaseB = document["B"].as<JsonObjectConst>();
  JsonObjectConst phaseY = document["Y"].as<JsonObjectConst>();
  if (phaseR.isNull()) phaseR = document["r"].as<JsonObjectConst>();
  if (phaseB.isNull()) phaseB = document["b"].as<JsonObjectConst>();
  if (phaseY.isNull()) phaseY = document["y"].as<JsonObjectConst>();

  return parsePhase(phaseR, parsedLimits.R) &&
         parsePhase(phaseB, parsedLimits.B) &&
         parsePhase(phaseY, parsedLimits.Y);
}

bool readingsWithinLimits(const PhaseLimit& phase, float voltage, float current, const char* name)
{
  bool withinLimits = true;

  if ((phase.flagVol && phase.minVol >= phase.maxVol) ||
      (phase.flagA && phase.minA >= phase.maxA))
  {
    Serial.printf("Phase %s has an invalid zero-width limit range\n", name);
    withinLimits = false;
  }

  if (phase.flagVol && (voltage < phase.minVol || voltage > phase.maxVol))
  {
    Serial.printf("Phase %s voltage out of range: %.1f V\n", name, voltage);
    withinLimits = false;
  }

  if (phase.flagA && (current < phase.minA || current > phase.maxA))
  {
    Serial.printf("Phase %s current out of range: %.1f A\n", name, current);
    withinLimits = false;
  }

  return withinLimits;
}

bool canStartRelay()
{
  bool phaseRWithinLimits = readingsWithinLimits(limits.R, measuredVoltage[0], measuredCurrent[0], "R");
  bool phaseBWithinLimits = readingsWithinLimits(limits.B, measuredVoltage[1], measuredCurrent[1], "B");
  bool phaseYWithinLimits = readingsWithinLimits(limits.Y, measuredVoltage[2], measuredCurrent[2], "Y");

  return phaseRWithinLimits && phaseBWithinLimits && phaseYWithinLimits;
}

int measurementStatus(bool enabled, float value, float minimum, float maximum)
{
  if (!enabled)
  {
    return 0;
  }

  return minimum < maximum && value >= minimum && value <= maximum ? 1 : 2;
}

void addPhaseMeasurement(JsonArray readings, const char* phaseName, const PhaseLimit& phase,
                         float voltage, float current)
{
  JsonObject reading = readings.add<JsonObject>();
  reading["VName"] = phaseName;
  reading["VValue"] = phase.flagVol ? voltage : 0;
  reading["VStatus"] = measurementStatus(phase.flagVol, voltage, phase.minVol, phase.maxVol);
  reading["AValue"] = phase.flagA ? current : 0;
  reading["AStatus"] = measurementStatus(phase.flagA, current, phase.minA, phase.maxA);
}

void publishMeasurements()
{
  JsonDocument document;
  JsonArray readings = document.to<JsonArray>();

  addPhaseMeasurement(readings, "R", limits.R, measuredVoltage[0], measuredCurrent[0]);
  addPhaseMeasurement(readings, "B", limits.B, measuredVoltage[1], measuredCurrent[1]);
  addPhaseMeasurement(readings, "Y", limits.Y, measuredVoltage[2], measuredCurrent[2]);

  String payload;
  serializeJson(document, payload);
  mqttClient.publish(MEASUREMENT_TOPIC, payload.c_str(), true);
}

void stopRelayIfLimitsFail()
{
  if (lastStatus != 1 || canStartRelay())
  {
    return;
  }

  digitalWrite(RELAY_PIN, RELAY_OFF);
  lastStatus = 2;
  preferences.putInt("status", 2);
  mqttClient.publish(STATUS_TOPIC, "2");
  Serial.println("RELAY STOPPED: readings outside limits");
}


// =========================
// MQTT MESSAGE
// =========================
void callback(char* topic, byte* payload, unsigned int length)
{
  String message = "";

  for (unsigned int i = 0; i < length; i++)
  {
    message += (char)payload[i];
  }

  Serial.print("Received: ");
  Serial.println(message);

  if (strcmp(topic, LIMIT_TOPIC) == 0)
  {
    DeviceLimits updatedLimits;
    if (parseLimits(message, updatedLimits))
    {
      limits = updatedLimits;
      defaultLimits = message;
      preferences.putString("limits", message);
      Serial.println("Saved updated device limits");
      stopRelayIfLimitsFail();
    }
    else
    {
      Serial.println("Rejected invalid limit configuration");
    }
  }
  else if (message == "1")
  {
    if (!canStartRelay())
    {
      digitalWrite(RELAY_PIN, RELAY_OFF);
      lastStatus = 2;
      preferences.putInt("status", 2);
      mqttClient.publish(STATUS_TOPIC, "2");
      Serial.println("RELAY START BLOCKED: readings outside limits");
      return;
    }

    digitalWrite(RELAY_PIN, RELAY_ON);

    lastStatus = 1;
    preferences.putInt("status", 1);

    Serial.println("RELAY ON");
    Serial.println("Saved STATUS = 1");

    mqttClient.publish(STATUS_TOPIC, "1");
  }

  else if (message == "2")
  {
    digitalWrite(RELAY_PIN, RELAY_OFF);

    lastStatus = 2;
    preferences.putInt("status", 2);

    Serial.println("RELAY OFF");
    Serial.println("Saved STATUS = 2");

    mqttClient.publish(STATUS_TOPIC, "2");
  }
}


// =========================
// WIFI
// =========================
void connectWiFi()
{
  Serial.println("Connecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi Connected");

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}


// =========================
// MQTT
// =========================
void connectMQTT()
{
  while (!mqttClient.connected())
  {
    Serial.println("Connecting MQTT...");

    String clientId = "ESP32_101_";
    clientId += String(random(10000));

    if (mqttClient.connect(clientId.c_str()))
    {
      Serial.println("MQTT Connected");

      mqttClient.subscribe(MQTT_TOPIC);
      mqttClient.subscribe(LIMIT_TOPIC);

      Serial.print("Subscribed: ");
      Serial.println(MQTT_TOPIC);

      // Send current status
      if (lastStatus == 1)
      {
        mqttClient.publish(STATUS_TOPIC, "1");
        Serial.println("Sent current status: 1");
      }
      else
      {
        mqttClient.publish(STATUS_TOPIC, "2");
        Serial.println("Sent current status: 2");
      }
    }
    else
    {
      Serial.print("MQTT failed, state = ");
      Serial.println(mqttClient.state());

      delay(3000);
    }
  }
}


// =========================
// SETUP
// =========================
void setup()
{
  Serial.begin(115200);

  // Relay
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  // Open ESP32 permanent memory
  preferences.begin("motor", false);

  String savedLimits = preferences.getString("limits", defaultLimits);
  if (!parseLimits(savedLimits, limits))
  {
    parseLimits(defaultLimits, limits);
    preferences.putString("limits", defaultLimits);
  }

  // Read previous status
  lastStatus = preferences.getInt("status", 2);

  Serial.println();
  Serial.println("==============================");
  Serial.println("ESP32 MOTOR CONTROLLER");
  Serial.println("==============================");

  Serial.print("Saved status = ");
  Serial.println(lastStatus);

  // Restore the relay only if the saved limits allow it.
  if (lastStatus == 1 && canStartRelay())
  {
    digitalWrite(RELAY_PIN, RELAY_ON);
    Serial.println("RESTORE -> RELAY ON");
  }
  else
  {
    digitalWrite(RELAY_PIN, RELAY_OFF);
    Serial.println("RESTORE -> RELAY OFF");
    if (lastStatus == 1)
    {
      lastStatus = 2;
      preferences.putInt("status", 2);
    }
  }

  // WiFi
  connectWiFi();

  // MQTT
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(callback);
  mqttClient.setBufferSize(512);
}


// =========================
// LOOP
// =========================
void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectWiFi();
  }

  if (!mqttClient.connected())
  {
    connectMQTT();
  }

  mqttClient.loop();
  stopRelayIfLimitsFail();

  if (millis() - lastMeasurementPublishMs >= MEASUREMENT_INTERVAL_MS)
  {
    lastMeasurementPublishMs = millis();
    publishMeasurements();
  }
}