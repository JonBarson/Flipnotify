# FlipNotify 0.1 MVP

Android notification mirroring to Flipper Zero over the stock BLE Serial profile.

## What it does
- Android `NotificationListenerService` receives posted notifications.
- The app connects to the Flipper BLE Serial service and writes `app<TAB>title<TAB>text` to RX.
- The Flipper FAP switches to the exported serial BLE profile, shows the latest notification and vibrates.
- Exiting the FAP restores the normal Flipper Bluetooth profile.

## Phone-only build/install workflow
1. Create a GitHub repository from this folder (GitHub web/app is enough).
2. Push/upload all files and open **Actions > Build FlipNotify > Run workflow**.
3. Download the two build artifacts on Android: `app-debug.apk` and `flipnotify.fap`.
4. Install the APK. Android may ask you to allow installs from your browser/files app.
5. Put `flipnotify.fap` on the Flipper SD card under `/ext/apps/Tools/` using a phone workflow that can transfer files to the Flipper. If your Flipper mobile app build does not expose arbitrary file upload, a USB-C OTG SD-card reader is the reliable phone-only fallback.
6. Start **FlipNotify** on Flipper. While it is open, it owns the BLE serial profile, so the official Flipper mobile connection is temporarily replaced.
7. In Android FlipNotify, grant **Notification access**, then grant **Nearby devices/Bluetooth** and tap **Scan / connect Flipper**. Complete pairing if Android asks.
8. Once status says **Ready**, new notifications should appear on Flipper and vibrate.

## Current MVP limitations
- Only the newest notification is displayed; no history yet.
- Message payload is capped around 240 bytes and long UTF-8 text can be clipped badly at the boundary.
- No app whitelist/blacklist yet.
- Connection does not automatically persist/reconnect after process death.
- Android UI status updates from GATT callbacks are intentionally minimal.
- The BLE profile uses authenticated characteristics; first connection may require pairing.

## BLE UUIDs
- Service: `8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000`
- RX: `19ed82ae-ed21-4c9d-4145-228e62fe0000`

These are the UUIDs used by Flipper's stock Serial service; the FAP uses the firmware-exported `ble_profile_serial` API rather than defining a private BLE stack.
