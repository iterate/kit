# HAVPE preloadable provisioning, local UI, and recovery

Prototype design and trade-off notes for handing out Home Assistant Voice Preview Edition (HAVPE)
boards preloaded with iterate firmware, so a recipient can set up Wi-Fi from a stock iOS or Android
phone without a native app or USB cable, inspect/reconfigure the board on the LAN, and recover
Wi-Fi cleanly.

## 1. User experience (step by step)

Each preloaded HAVPE ships with a small printed card (or bottom sticker) showing:

- a standard `WIFI:T:WPA;S:iterate-<id>;P:<setup-code>;;` QR code,
- the 9-character setup code (`xxxx-xxxx`, Crockford Base32, 40 bits of entropy), and
- its LAN address (`http://iterate-<id>.local/`).

### First-time setup (preloaded board, no Wi-Fi yet)

1. **Plug in the HAVPE.** Finding no Wi-Fi credentials in the `iterate_kit` partition, it enters
   Setup Mode automatically: the 12-LED ring shows a **rotating cyan comet** and it starts a
   WPA2-PSK SoftAP (`iterate-<id>`) using the per-device setup code as its passphrase.
2. **Scan the QR code with the phone's built-in Camera app** (iOS or Android). The OS prompts
   _"Join Network `iterate-<id>`?"_ — tap **Join**.
3. **Captive portal opens automatically.** The board answers UDP/53 DNS with `192.168.4.1` and
   redirects OS captive-portal probes (`/hotspot-detect.html`, `/generate_204`, `/connecttest.txt`,
   etc.) to `http://192.168.4.1/`.
4. **Pick 2.4 GHz Wi-Fi and save.** Tap a scanned nearby 2.4 GHz network (or type the SSID), enter
   the Wi-Fi password, and tap **Save Wi-Fi & Connect**. The board writes `iterate_kit` and reboots
   onto the home network.
   - If a mind credential was already preserved (after a Wi-Fi reset) or entered in the optional
     collapsible section, the board connects straight to iterate.
   - Otherwise, it joins Wi-Fi, breathes **calm teal** on the ring, and advertises
     `http://iterate-<id>.local/` over mDNS/Bonjour (`_http._tcp` and `_iterate._tcp`).
5. **Link (or relink) an iterate mind on the LAN.** Once the phone is back on home Wi-Fi, open
   `http://iterate-<id>.local/`. Enter the setup code from the card (or slide the **Mute** switch
   ON and tap the centre button to unlock for 3 minutes), then enter the mind address
   (`example.com` or `https://iterate.example.com`), OAuth client ID, and refresh token (and
   optionally adjust Status Voice or Speaker Volume). The board writes `iterate_kit`, clears any
   stale rotated token in NVS, reboots, and mounts on the mind via the existing `itx_mount` /
   `token_refresh` path.

### Wi-Fi reset vs. full wipe (no screen)

| Action          | Physical gesture                                                                | Ring feedback                                                              | What changes                                                                                                                                                 |
| --------------- | ------------------------------------------------------------------------------- | -------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **LAN unlock**  | Mute switch **ON**, short tap centre button (`< 1.5 s`)                         | 2 s green pulse                                                            | Unlocks LAN settings page for 3 minutes without starting a call                                                                                              |
| **Wi-Fi reset** | Mute switch **OFF**, hold centre button **5 s** (release between 5 s and 10 s)  | Cyan arc fills `1.5–5 s`, then rapid cyan blink; 3 cyan flashes on trigger | Clears **only** Wi-Fi SSID/password; preserves mind credential, rotated refresh token, status voice, and setup code; reboots to Setup Mode                   |
| **Full wipe**   | Mute switch **ON**, hold centre button **10 s** (release between 10 s and 15 s) | Red arc fills `1.5–10 s`, then rapid red blink; 3 red flashes on trigger   | Erases `iterate_kit` partition, NVS `iterate_token`, and kept volume; preserves per-device setup code so the printed card stays valid; reboots to Setup Mode |

Holding past the upper bound (`> 10 s` unmuted, `> 15 s` muted) or toggling the mute switch
mid-hold cancels safely instead of escalating. A button held at power-on (e.g. GPIO0 boot strap)
is ignored until released once.

## 2. Provisioning options compared

### What the HAVPE hardware supports

- **ESP32-S3** with 2.4 GHz Wi-Fi (802.11b/g/n) and Bluetooth 5.0 LE, 16 MiB flash (`ota_0` /
  `ota_1` 5 MiB each, `iterate_kit` 4 KiB), 8 MiB octal PSRAM.
- **No NFC chip** on the PCB (only an external passive NFC sticker on the enclosure is possible).
- **No screen** — only a 12-LED WS2812 ring, centre button (GPIO0), rotary dial (GPIO16/18),
  hardware mute switch (GPIO3), and speaker/mics (XMOS + AIC3204).

### Trade-offs

| Option                                          | Works on stock iOS (no native app)?                                                               | Works on stock Android (no native app)?                             | Hardware fit on HAVPE                                     | Verdict / trade-offs                                                                                                                                                                                                                                   |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **SoftAP + captive portal + `WIFI:` QR card**   | **Yes** (Camera joins WPA2 SoftAP; CNA opens portal)                                              | **Yes** (Camera joins WPA2 SoftAP; CaptivePortalLogin opens portal) | Native Wi-Fi + `esp_http_server`; zero BLE RAM/flash cost | **Chosen.** Only zero-app path that works identically on stock iOS and Android while encrypting Wi-Fi credentials over the air (WPA2-PSK). Trade-off: phone briefly leaves home Wi-Fi during step 1, so mind sign-in happens on LAN in step 2.         |
| **Improv / BLE provisioning (Web Bluetooth)**   | **No** — WebKit/iOS Safari has no Web Bluetooth                                                   | Yes (Chrome Web Bluetooth)                                          | ESP32-S3 has BLE 5.0                                      | Rejected as primary route because iOS requires a native app (or third-party BLE browser like Bluefy). Also adds BLE stack flash + internal RAM overhead alongside TLS and WakeNet.                                                                     |
| **Matter commissioning**                        | Via Apple Home (requires Matter fabric/hub)                                                       | Via Google Home (requires Matter fabric/hub)                        | ESP32-S3 supports `esp-matter`                            | Rejected: requires CSA vendor/product certification for distribution, drags in a large C++ stack (~1+ MiB flash), still depends on an Apple/Google hub ecosystem, and Matter has no concept of an iterate mind credential (still needs a second step). |
| **Passive NFC sticker**                         | **Partial** — iOS reads `http(s)://` URLs and App Clips, but **ignores Wi-Fi (WSC) NDEF records** | Yes (reads both URLs and Wi-Fi WSC records)                         | External NTAG215 sticker only (no onboard NFC)            | Cannot replace the `WIFI:` QR code on stock iOS without a native app / App Clip. Useful as an optional companion sticker encoding `http://iterate-<id>.local/` for quick LAN page access after setup.                                                  |
| **Espressif `wifi_provisioning` (`protocomm`)** | **No** — requires Espressif's native app or custom native app                                     | **No** — requires native app                                        | Built into ESP-IDF                                        | Rejected: `protocomm` speaks protobuf + SRP6a/X25519 over custom GATT or HTTP endpoints that stock phone browsers cannot drive natively.                                                                                                               |

### What needs a native app (or platform change)

- **"Hold your phone near the puck" (BLE proximity / NFC Wi-Fi join on iOS):** Requires either a
  native iOS app (CoreBluetooth / `NEHotspotConfigurationManager` / App Clip) or Apple's MFi/Matter
  ecosystem. Without a native app, scanning a `WIFI:` QR code is the closest stock-phone equivalent.
- **One-tap mind sign-in without pasting a refresh token (future platform work, not built here):**
  Today a board mounts on a mind using `os_base_url`, `oauth_client_id`, and `oauth_refresh_token`
  (`itx_mount.h`, `token_refresh.h`), which Kit Flasher obtains via browser OAuth PKCE
  (`src/firmware/mind-sign-in.ts`). Because the LAN page runs on plain HTTP at
  `http://iterate-<id>.local/` and cannot host an HTTPS OAuth redirect URI trusted by a mind's
  issuer, this prototype accepts the existing three mind fields directly without changing any
  server-side auth.
  A cleaner future flow on the platform side would be **OAuth 2.0 Device Authorization Grant
  (RFC 8628)** on the mind's issuer (`POST /oauth2/device_authorization` + polling `/oauth2/token`):
  the user types only their mind domain on `http://iterate-<id>.local/`, the board asks the mind
  for a short user code and verification URL, and the page shows a button/link to approve the board
  in the user's logged-in browser — with no token copy-paste and no HTTPS callback to `.local`.

## 3. Security model

| Surface                                              | Who can reach it                                                                                    | Allowed actions                                                                                                                                                                                                                                           | Protections                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ---------------------------------------------------- | --------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **SoftAP (`iterate-<id>`, `192.168.4.1`)**           | Only someone within Wi-Fi range who has the per-device 9-char setup code (printed on the card / QR) | View setup page, scan 2.4 GHz networks, submit Wi-Fi / optional mind / settings                                                                                                                                                                           | Active **only** when Wi-Fi is unconfigured or physically reset. WPA2-PSK encrypts over-the-air traffic (home Wi-Fi password is never sent in cleartext RF). CSRF token checked on every POST. `Host` header restricted to `192.168.4.1` / `iterate-<id>.local`.                                                                                                                                                                                                                          |
| **LAN page (`http://iterate-<id>.local/`, port 80)** | Devices on the same local network                                                                   | **Read-only (`GET /`, `GET /api/status`):** view status (Wi-Fi SSID, RSSI, IP, mind origin, connection state, firmware version, status voice, volume). **Write (`POST /wifi`, `/mind`, `/settings`, `/reset-wifi`, `/reboot`):** modify config or reboot. | Read-only endpoints **never** expose stored secrets (Wi-Fi password, OAuth refresh token, project API key, or setup code). Every mutating POST requires: (1) valid `Host` header (`iterate-<id>.local` or current LAN IP — blocks DNS rebinding), (2) per-boot CSRF token, and (3) either the per-device setup code (constant-time compare, exponential lockout after 5 wrong guesses) or a **physical presence unlock** (Mute ON + tap centre button on the puck, valid for 3 minutes). |
| **External / cloud**                                 | None                                                                                                | None                                                                                                                                                                                                                                                      | No cloud endpoint, tunnel, or remote admin API is opened. Existing Cap'n Web session to the mind is unchanged.                                                                                                                                                                                                                                                                                                                                                                           |

**Trade-offs acknowledged:** LAN traffic to `http://iterate-<id>.local/` is plain HTTP (browsers do
not trust self-signed certs for `.local`), so a passive observer on the same untrusted Wi-Fi segment
could sniff credentials submitted on the LAN page. This is acceptable on a trusted home LAN and is
another reason RFC 8628 device-code pairing (where the refresh token goes directly over HTTPS from
the mind's issuer to the board) is the right next step on the platform.

## 4. What was built, tested, and NOT proven

### Tested on the host (`pnpm firmware:test:host`, ASan + UBSan)

- `ITERKIT1` C encoder and partial-config decoder (`components/core/src/configuration.c`), verified
  byte-for-byte against the TypeScript golden images (`configuration_test.c`), plus Wi-Fi-only,
  Wi-Fi-reset (mind preserved), password/voice validation, and mind-origin normalization.
- Boot-mode selection, short-ID/hostname derivation, Crockford Base32 setup-code generation and
  constant-time matching, `WIFI:` QR escaping, Host-header DNS-rebinding filter, CSRF check,
  physical unlock window, and brute-force backoff (`provisioning_state_test.c`).
- RFC 1035 captive DNS responder for A/ANY, AAAA NODATA, and malformed packets (`captive_dns_test.c`).
- Form URL decoding, HTML XSS escaping, captive-probe detection, Wi-Fi/Mind/Settings form handling,
  and secret-redaction checks on rendered HTML and `/api/status` JSON (`web_portal_test.c`).
- HAVPE physical recovery gesture timing (boot-held ignore, debounce, unmuted 5 s Wi-Fi reset,
  muted 10 s full wipe, muted tap LAN unlock, mid-hold mute cancel, over-hold cancel) and LED ring
  mode/gesture renderers (`havpe_reset_gesture_test.c`).

### Built only (compiled with ESP-IDF 6.1 for `esp32s3`, partition layout checked)

- `iterate-kit-havpe.bin` builds cleanly under ESP-IDF 6.1 (`0x1491d0` / 1,348,048 bytes, 74% free
  in the `0x500000` `ota_0` partition) and passes `scripts/firmware-release.ts build` (partition
  table layout check, `iterate_kit` non-overlap check, and `filesOutsideInputs` check: 540 firmware
  files read, all inside HAVPE's inputs).
- `platforms/iterate_esp_idf/configuration.c`: flash read/write/clear-Wi-Fi/full-wipe and NVS setup
  code persistence.
- `platforms/iterate_esp_idf/components/board/led_ring.c` and `board.c`: custom ring pixel overlay
  and current-volume getter.
- `devices/havpe/havpe_portal.c`, `havpe_device.c`, `havpe_ui.c`: SoftAP (`WIFI_MODE_APSTA`),
  2.4 GHz scan, UDP/53 captive DNS task, `esp_http_server` routes, `espressif/mdns` advertisement
  (`_http._tcp` + `_iterate._tcp`), and Normal-mode integration.

### NOT proven (no hardware attached)

- **Nothing ran on physical HAVPE hardware.**
- Real iOS (CNA) and Android (CaptivePortalLogin) captive-portal sheet pop-up behavior, QR scan
  UX, and `iterate-<id>.local` mDNS resolution on real home routers/phones.
- Physical LED ring brightness/legibility, button/mute feel during 5 s / 10 s holds, and interaction
  between `board.c`'s down-edge call start and the start of an unmuted 5 s Wi-Fi reset hold while
  connected to a mind.
- Internal RAM headroom and audio latency when `esp_http_server` + `espressif/mdns` run alongside
  active GPT-Live audio, TLS, and WakeNet on real silicon.
- Flash erase/rewrite durability and power-loss recovery of `iterate_kit` on real flash.
