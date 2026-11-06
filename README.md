# cyd-chat

A local, WiFi-based "multiplayer notepad" / mini IRC for the ESP32 **Cheap Yellow Display** (ESP32-2432S028R: ILI9341 320x240 + XPT2046 touch).

The board runs its own WiFi access point, shows a QR code to join it and a second QR code to open the page. Phones that join get a nick and a **board-assigned colour**; messages show on every phone and on the board's screen. The board also has an on-screen touch keyboard, a colour selector for the next client, and a kick button. QR codes disappear while someone is connected.

The captive DNS answers your chosen hostname with the AP address and the board serves **HTTPS with a real Let's Encrypt certificate** for that name, so phones see a trusted padlock without internet access. Port 80 redirects to HTTPS.

## Layout
- `firmware/` PlatformIO project (Arduino on espressif32, TFT_eSPI, ricmoo/QRCode, XPT2046_Touchscreen, ESP-IDF `esp_https_server`)
- `server/chat-cert.sh` issues/renews the Let's Encrypt cert on any host you control (certbot, HTTP-01 webroot, no root needed)
- `colour-test/` small app for finding the right colour-order/inversion flags for your panel (this board needed BGR + inversion)

## Setup
1. Pick a hostname under a domain you control, e.g. `chat.example.com`, and point a public A record at a host with a web server on port 80 serving a webroot (`/var/www/acme`).
2. On that host: `DOMAIN=chat.example.com ./chat-cert.sh check`, then `issue`, then `export`. Copy `<domain>.fullchain.pem` and `<domain>.key.pem` to `firmware/certs/fullchain.pem` and `firmware/certs/key.pem` (gitignored; **never commit the key**).
3. Set the hostname in `firmware/platformio.ini` (`-DCHAT_HOST=...`) to match the certificate.
4. `cd firmware && pio run -t upload --upload-port COMx`

## Notes
- The cert and key are embedded in the firmware, so a renewed certificate (every ~90 days; ECDSA P-256) needs a rebuild and reflash. Anyone with the flash contents can extract the private key: use a dedicated hostname and treat it as low-value.
- The `huge_app` partition table is required (TLS enlarges the image).
- AP is open by default (`AP_PASS` in `main.cpp`). Touch calibration constants are typical values, not verified on every unit.
- Colour flags (`TFT_RGB_ORDER`, `TFT_INVERSION_ON`, `ILI9341_2_DRIVER`) vary between CYD revisions; use `colour-test`.
