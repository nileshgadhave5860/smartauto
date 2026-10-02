#include <Arduino.h>
#include <ModbusMaster.h>

constexpr uint32_t METER_BAUD = 9600;
constexpr uint8_t METER_SLAVE_ID = 1;
constexpr int8_t RS485_RX_PIN = 16;
constexpr int8_t RS485_TX_PIN = 17;
constexpr int8_t RS485_DIRECTION_PIN = 4;
constexpr uint16_t FIRST_REGISTER = 0;
constexpr uint16_t LAST_REGISTER = 255;
constexpr uint32_t REGISTER_SCAN_INTERVAL_MS = 60000;
constexpr uint32_t RESPONSE_TIMEOUT_MS = 80;

HardwareSerial meterSerial(2);
ModbusMaster meter;
uint32_t lastScanTime = 0;

void enableTransmit() {
  digitalWrite(RS485_DIRECTION_PIN, HIGH);
}

void enableReceive() {
  digitalWrite(RS485_DIRECTION_PIN, LOW);
}

void scanRegisterRange(bool inputRegisters, uint16_t& successfulReads) {
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
    }

    delay(10);
  }
}

void scanRegisters() {
  uint16_t successfulReads = 0;
  Serial.println("Starting read-only Modbus scan (registers 0-255).");

  scanRegisterRange(false, successfulReads);
  scanRegisterRange(true, successfulReads);

  Serial.printf("Scan complete. Successful register reads: %u\n", successfulReads);
}

void setup() {
  Serial.begin(115200);

  pinMode(RS485_DIRECTION_PIN, OUTPUT);
  enableReceive();

  meterSerial.begin(METER_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  meterSerial.setTimeout(RESPONSE_TIMEOUT_MS);
  meter.begin(METER_SLAVE_ID, meterSerial);
  meter.preTransmission(enableTransmit);
  meter.postTransmission(enableReceive);

  delay(1000);
  scanRegisters();
  lastScanTime = millis();
}

void loop() {
  if (millis() - lastScanTime >= REGISTER_SCAN_INTERVAL_MS) {
    scanRegisters();
    lastScanTime = millis();
  }
}