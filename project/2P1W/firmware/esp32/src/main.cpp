#if __has_include("meter_config.h")
#include "meter_config.h"
#else
#include "config.example.h"
#endif

#include <Arduino.h>
#include <cmath>
#include <cstring>
#include <ModbusMaster.h>
#include <PubSubClient.h>
#include <WiFi.h>

HardwareSerial meterSerial(2);
ModbusMaster meter;
WiFiClient networkClient;
PubSubClient mqtt(networkClient);
unsigned long lastPollAt = 0;

void enableTransmit()
{
    digitalWrite(RS485_DE_RE_PIN, HIGH);
}

void enableReceive()
{
    digitalWrite(RS485_DE_RE_PIN, LOW);
}

void connectWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(WIFI_RETRY_DELAY_MS);
    }
}

bool connectMqtt()
{
    while (!mqtt.connected())
    {
        const String clientId = "esp32-mfm-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        bool connected;
        if (strlen(MQTT_USERNAME) > 0)
        {
            connected = mqtt.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD);
        }
        else
        {
            connected = mqtt.connect(clientId.c_str());
        }

        if (!connected)
        {
            delay(MQTT_RETRY_DELAY_MS);
        }
    }
    return true;
}

bool decodeFloatRegisterPair(uint8_t responseIndex, float scale, float &value)
{
    const uint32_t rawValue =
        (static_cast<uint32_t>(meter.getResponseBuffer(responseIndex)) << 16) |
        meter.getResponseBuffer(responseIndex + 1);
    std::memcpy(&value, &rawValue, sizeof(value));
    value *= scale;
    return std::isfinite(value);
}

bool readL1Measurements(float &voltage, float &current)
{
    const uint8_t result = meter.readInputRegisters(L1_VOLTAGE_REGISTER, 4);
    if (result != meter.ku8MBSuccess)
    {
        Serial.printf("Input register block read failed at %u: 0x%02X\n",
                      L1_VOLTAGE_REGISTER,
                      result);
        return false;
    }

    const uint8_t currentOffset = L1_CURRENT_REGISTER - L1_VOLTAGE_REGISTER;
    return decodeFloatRegisterPair(0, L1_VOLTAGE_SCALE, voltage) &&
           decodeFloatRegisterPair(currentOffset, L1_CURRENT_SCALE, current);
}

void scanRegisterRange(const char *registerType, bool inputRegisters)
{
    Serial.printf("\n%s registers, start=%u, count=%u\n",
                  registerType,
                  MFM_SCAN_START,
                  MFM_SCAN_COUNT);

    const uint8_t result = inputRegisters
                               ? meter.readInputRegisters(MFM_SCAN_START, MFM_SCAN_COUNT)
                               : meter.readHoldingRegisters(MFM_SCAN_START, MFM_SCAN_COUNT);
    Serial.printf("Modbus result: 0x%02X\n", result);
    if (result != meter.ku8MBSuccess)
    {
        return;
    }

    for (uint8_t index = 0; index < MFM_SCAN_COUNT; ++index)
    {
        const uint16_t address = MFM_SCAN_START + index;
        const uint16_t rawValue = meter.getResponseBuffer(index);
        Serial.printf("Register %u: 0x%04X (%u)\n", address, rawValue, rawValue);
    }
}

void setup()
{
    Serial.begin(115200);
    pinMode(RS485_DE_RE_PIN, OUTPUT);
    enableReceive();
    meterSerial.begin(MODBUS_BAUD, SERIAL_8N1, MODBUS_RX_PIN, MODBUS_TX_PIN);
    meter.begin(MODBUS_SLAVE_ID, meterSerial);
    meter.preTransmission(enableTransmit);
    meter.postTransmission(enableReceive);

#if MFM_REGISTER_MAP_CONFIRMED
    connectWifi();
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
#else
    Serial.printf("MFM read-only diagnostic; slave=%u baud=%u\n",
                  MODBUS_SLAVE_ID,
                  MODBUS_BAUD);
    Serial.println("Configure these to match the meter. No MQTT values will be published.");
#endif
}

void loop()
{
#if MFM_REGISTER_MAP_CONFIRMED
    if (WiFi.status() != WL_CONNECTED)
    {
        connectWifi();
    }
    if (!mqtt.connected())
    {
        connectMqtt();
    }
    mqtt.loop();

    if (millis() - lastPollAt < POLL_INTERVAL_MS)
    {
        return;
    }
    lastPollAt = millis();

    float l1Voltage;
    float l1Current;
    if (!readL1Measurements(l1Voltage, l1Current))
    {
        Serial.println("L1 voltage/current read failed; check MFM registers and RS-485 wiring");
        return;
    }

    char payload[112];
    snprintf(payload, sizeof(payload), "{\"LNVolt\":%.3f,\"LNA\":%.3f}",
             l1Voltage, l1Current);
    if (!mqtt.publish(MQTT_TOPIC, payload, true))
    {
        Serial.println("MQTT publish failed");
    }
    else
    {
        Serial.println(payload);
    }
#else
    if (millis() - lastPollAt < POLL_INTERVAL_MS)
    {
        return;
    }
    lastPollAt = millis();
    scanRegisterRange("holding (function 03)", false);
    scanRegisterRange("input (function 04)", true);
#endif
}