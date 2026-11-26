# Safe Homes

CpE 414 Embedded Systems final project: an IoT home-safety monitor.

- **Arduino Pro Mini 5V 16 MHz**: ultrasonic, DHT22 and smoke/gas sensors, buzzer, fan, LED, DS3231 RTC, SD card queue. Modes: AUTO, MAINTENANCE, SLEEP (Power-Down with BOD disabled).
- **ESP-01**: WiFi bridge to the laptop server; wakes the Arduino through the INT0 pin (D2).
- **Laptop server**: Flask + SQLite (offline database), website with login, `sync.py` uploads to Supabase (online database).

## Folders

| Folder | Content |
|---|---|
| `arduino/SafeHomes` | Pro Mini program (`SafeHomes.ino`) |
| `arduino/SafeHomes_ESP` | ESP-01 program (`SafeHomes_ESP.ino`) |
| `arduino/libraries` | `SafeSensors`, `EspLink`, `EspNet` (one `.h` and one `.cpp` each) |
| `server` | `server.py`, `database.py`, `sync.py`, `templates/index.html` |
| `docs` | Code walkthrough (PDF) |

## Arduino libraries
Copy the three folders in `arduino/libraries` into your Arduino `libraries` folder. Install from the Library Manager: DHT sensor library, Adafruit Unified Sensor, RTClib. ESP-01 board: Generic ESP8266 Module.

## Run the server (Windows)
```powershell
cd server
pip install flask requests
python -u server.py
```
Open http://localhost:5000, create an account, and sign in. The ESP-01 setup page (access point `SafeHomes-Setup`) stores the WiFi name, password, server address and device key in the ESP EEPROM.

## Online database (optional)
Set these in the same terminal before starting the server. Never commit the key.
```powershell
$env:ONLINE_URL = "https://<project>.supabase.co"
$env:ONLINE_KEY = "<service key>"
```
Tables: `readings` and `sleep_log` (unique column `uid`).

## Device key
`DEVICE_KEY` (environment variable, see `server.py`) must match the key saved in the ESP-01.

## Before you use this code (secrets are placeholders)
Replace these placeholders with your own values. Do not commit the real ones.
- `CHANGE_ME_DEVICE_KEY` in `arduino/libraries/EspNet/EspNet.cpp` and `server/server.py` (or the `DEVICE_KEY` environment variable): the same value on both sides.
- `change-me-123` in `arduino/SafeHomes_ESP/SafeHomes_ESP.ino`: password of the setup access point (8 characters or more).
- `https://YOUR-PROJECT.supabase.co` in `server/sync.py` (or the `ONLINE_URL` environment variable), and the `ONLINE_KEY` environment variable.
- `192.168.x.x` in `EspNet.cpp`: the laptop address, or set it on the ESP setup page.
