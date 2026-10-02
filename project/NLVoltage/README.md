# NLVoltage

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
npm run dev
```

## Build and upload ESP32 firmware

Open `firmware/` in VS Code with the PlatformIO extension, then build and upload
the `esp32dev` environment. The starter sketch blinks GPIO 2 and writes serial
output at 115200 baud.