# MFM Meter MQTT Projects

This workspace contains two projects:

- `firmware/esp32`: ESP32 polls L1 voltage and current from MFM input registers over RS-485, then publishes JSON to MQTT.
- `api/MeterApi`: ASP.NET Core API subscribes to that MQTT topic and returns the latest reading as JSON.

## Data flow

```text
MFM meter -- Modbus RTU / RS-485 --> ESP32 -- MQTT --> Broker <-- MQTT -- .NET API
                                                                                |
                                                        GET /api/readings <-----+
```

The shared MQTT payload is `{"LNVolt": 241.7, "LNA": 0.0}` on `mfm/readings`. `LNVolt` is voltage in volts and `LNA` is current in amperes.

## Configure and run the ESP32

1. Install PlatformIO and open `firmware/esp32` as a PlatformIO project.
2. Copy `include/config.example.h` to `include/meter_config.h` and set the Wi-Fi credentials, reachable MQTT broker IP, RS-485 pins, Modbus slave ID, register addresses, and scaling factors.
3. The example assumes L1 voltage is a 32-bit float in input registers `0-1` and L1 current is a 32-bit float in input registers `2-3`, high word first. Verify those addresses and scales against the MFM443-TX register map before relying on the readings.
4. Build and upload with PlatformIO. The RS-485 adapter's DE and RE pins are assumed tied together and connected to `RS485_DE_RE_PIN`.

## Run the API

Start an MQTT broker (for example, Mosquitto), then configure `Mqtt:Host`, `Mqtt:Port`, `Mqtt:Topic`, and optional credentials in `api/MeterApi/appsettings.json`. The default topic matches the ESP32 configuration.

```powershell
dotnet run --project api/MeterApi --urls http://0.0.0.0:5080
```

Read the latest JSON response at `http://localhost:5080/api/readings`:

```json
{
  "available": true,
  "LNVolt": 241.7,
  "LNA": 0.0,
  "receivedAtUtc": "2026-10-03T12:00:00+00:00"
}
```

Before the first MQTT message, `available` is `false` and `LNVolt`/`LNA` are `null`. The API keeps the latest reading in memory; it does not persist readings across restarts.