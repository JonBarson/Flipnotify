# WiFi Toolkit

Standalone WiFi management and passive WiFi analysis for the **Flipper Zero** + official **WiFi Devboard (ESP32-S2)**, usable without a phone or PC during normal use.

The Flipper handles all UI, navigation and input. The ESP32 devboard does the WiFi work (scan, connect, RSSI, saved credentials in NVS). They talk over UART with a simple newline-terminated text protocol.

> **Scope:** passive scan + connection management only. No deauth, packet injection, handshake capture, rogue-AP or similar offensive features - by design.

## Layout

- `esp32/wifi_toolkit_esp32.ino` - ESP32 firmware (Arduino).
- `flipper/` - Flipper application (FAP): `application.fam` + `wifi_toolkit.c`.

The `.fap` is built automatically by CI and published to the repo's **latest** release. Download it there or build locally with `ufbt`.

## 1. Flash the ESP32 devboard

The official devboard ships with different firmware (Black Magic / debug), which does not speak this protocol, so it must be flashed once.

Using Arduino IDE:

1. Install the ESP32 board package (Boards Manager -> "esp32" by Espressif).
2. Select board **ESP32S2 Dev Module**.
3. Connect the devboard over USB-C and put it in download mode: hold **BOOT**, tap **RESET**, release BOOT.
4. Open `esp32/wifi_toolkit_esp32.ino` and click **Upload**.

UART runs at **115200** baud, 8N1. The devboard connects to the Flipper's GPIO UART (pins 13 TX / 14 RX).

## 2. Install the Flipper app

Copy `wifi_toolkit.fap` to the Flipper SD card under `/ext/apps/GPIO/` (via qFlipper, the mobile app, or a card reader). It then appears under **Apps -> GPIO -> WiFi Toolkit**.

## 3. Use it

Attach the devboard, launch WiFi Toolkit. If the menu appears, the devboard answered `PING`. If it stays on "Detecting devboard", check the board is seated and flashed; press OK to retry.

- **Scan Networks** - list of SSID / RSSI / channel / security (hidden shown as `<hidden>`). Right = rescan, OK = detail.
- **Network Detail** - Connect or Analyze signal.
- **Connect** - on-screen keyboard for the password (Shift for caps/symbols, SHW to reveal, OK to submit). OPEN networks skip the password.
- **Current Connection** - SSID, IP, RSSI, channel, gateway. Left = disconnect, OK = refresh.
- **Saved Networks** - stored in ESP32 NVS. Connect, toggle auto-connect, or forget.
- **Analyzer** - Signal Monitor (live RSSI graph) and Channel View (AP occupancy per 2.4 GHz channel).
- **Settings** - auto-reconnect, reset saved WiFi, ESP32 firmware version.

## UART protocol (v1)

Each message is a single line terminated by `\n`. Flipper -> ESP32 commands:

```
PING
SCAN
CONNECT|<ssid>|<password>
DISCONNECT
STATUS
RSSI|<bssid>
SAVED_LIST
SAVED_CONNECT|<ssid>
SAVED_DELETE|<ssid>
SAVED_AUTO|<ssid>|<0|1>
SAVED_CLEAR
AUTOCONNECT|<0|1>
VERSION
```

ESP32 -> Flipper responses:

```
PONG|<proto>|<fw>
VERSION|<proto>|<fw>
SCAN_BEGIN|<count>
NET|<ssid>|<bssid>|<rssi>|<channel>|<security>
SCAN_END
STATUS|CONNECTING
STATUS|CONNECTED|<ssid>|<ip>|<rssi>|<channel>|<gateway>|<dns>
STATUS|DISCONNECTED
STATUS|ERROR|<code>
RSSI|<bssid>|<value|NA>
SAVED_BEGIN|<count>
SAVED|<ssid>|<auto 0|1>
SAVED_END
SAVED_CLEARED
AUTOCONNECT|<0|1>
```

The Flipper parser tolerates partial and malformed lines and uses bounded buffers; it never blocks the UI thread (UART is handled on a worker thread).

## Status

v0.1: devboard detection, scan, detail, connect (+ keyboard), current connection, saved networks, signal monitor, channel view, settings. Passive scope complete.
