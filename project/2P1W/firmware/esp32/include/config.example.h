#pragma once

#define WIFI_SSID "Ayush_EXT"
#define WIFI_PASSWORD "Ayush@5860"
#define WIFI_RETRY_DELAY_MS 500

#define MQTT_HOST "88.222.213.221"
#define MQTT_PORT 1883
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""
#define MQTT_TOPIC "mfm/readings"
#define MQTT_RETRY_DELAY_MS 3000

#define MODBUS_RX_PIN 16
#define MODBUS_TX_PIN 17
#define RS485_DE_RE_PIN 4
// These must match the meter's configured Modbus serial settings.
#define MODBUS_BAUD 9600
#define MODBUS_SLAVE_ID 1

#define MFM_REGISTER_MAP_CONFIRMED 1
#define L1_VOLTAGE_REGISTER 0
#define L1_CURRENT_REGISTER 2
#define L1_VOLTAGE_SCALE 1.0f
#define L1_CURRENT_SCALE 1.0f
#define MFM_SCAN_START 0
#define MFM_SCAN_COUNT 8
#define POLL_INTERVAL_MS 30000