# SmartMotorAuto

This workspace contains three independent projects:

- `api/` - ASP.NET Core Web API targeting .NET 9.
- `web/` - React JavaScript app built with Vite.
- `firmware/` - ESP32 Arduino firmware project for PlatformIO.

## Run the API

```powershell
cd api
dotnet run
```

## Run the web app

```powershell
cd web
npm install
npm start
```

## Build and upload ESP32 firmware

Open `firmware/` in VS Code with the PlatformIO extension, then build and upload
the `esp32dev` environment. In Auto Single Phase mode, the firmware reads LN
voltage and current from Modbus input registers 0-3 and publishes the reading
to the device measurement topic. The defaults assume high-word-first 32-bit
floats, with voltage in registers 0-1 and current in registers 2-3; update the
register and scale constants in `firmware/src/main.cpp` to match the meter.