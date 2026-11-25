# HAVPE preloadable provisioning, local UI, and recovery

Prototype design and trade-off notes for handing out Home Assistant Voice Preview Edition (HAVPE)
boards preloaded with iterate firmware, so a recipient can set up Wi-Fi and link an iterate mind
from a stock iOS or Android phone without a native app or USB cable, inspect/adjust the board on
the LAN, and recover Wi-Fi cleanly.

## 1. User experience (step by step)

Each preloaded HAVPE ships with a small printed card (or bottom sticker) showing:

- a standard `WIFI:T:WPA;S:iterate-<id>;P:<setup-code>;;` QR code,
- the 14-character setup code (`xxxx-xxxx-xxxx`, 12 Crockford Base32 characters, 60 bits of
  entropy), and
- its LAN address (`http://iterate-<id>.local/`).

### Preloading / factory card step (before handing out a board)

1. **Flash the HAVPE over USB** with the iterate firmware release (`esptool.py` / `idf.py flash`),
   leaving the `iterate_kit` partition erased (`0xFF`).
2. **Keep USB connected and open the USB-Serial-JTAG console** (`115200` baud) while the board
   boots for the first time.
3. **Read the generated setup lines.** Once Wi-Fi RF starts (enabling the ESP32-S3 hardware RNG),
   the board generates its permanent 60-bit setup code (`xxxx-xxxx-xxxx`), saves it to NVS
   (`kit_setup/code`), and prints two lines on serial whenever it boots into Setup Mode:
   ```text
   havpe-portal: setup_ap ssid=iterate-<id> setup_code=<xxxx-xxxx-xxxx>
   havpe-portal: setup_qr WIFI:T:WPA;S:iterate-<id>;P:<xxxx-xxxx-xxxx>;;
   ```
4. **Print the card/sticker.** Encode the `setup_qr` string as a QR code alongside the human-readable
   `setup_code` and `http://iterate-<id>.local/`, and pack it with that board. Because `kit_setup`
   is preserved across both Wi-Fi resets and full wipes, the card stays valid for the board's life.

### First-time setup (recipient, no USB or native app)

1. **Plug in the HAVPE.** Finding no Wi-Fi credentials in the `iterate_kit` partition, it enters
   Setup Mode automatically: the 12-LED ring shows a **rotating cyan comet** and it starts a
   WPA2-PSK SoftAP (`iterate-<id>`) using the per-device setup code as its passphrase.
2. **Scan the QR code with the phone's built-in Camera app** (iOS or Android). The OS prompts
   _"Join Network `iterate-<id>`?"_ — tap **Join**.
3. **Captive portal opens automatically.** The board answers UDP/53 DNS with `192.168.4.1` and
   redirects OS captive-portal probes (`/hotspot-detect.html`, `/generate_204`, `/connecttest.txt`,
   etc.) to `http://192.168.4.1/`.
4. **Link the iterate mind and enter 2.4 GHz Wi-Fi over the encrypted Setup AP.**
   - Both the **Wi-Fi (2.4 GHz)** form and the **Iterate Mind** form (`mind_url`, `client_id`,
     `refresh_token`) live on the WPA2-encrypted Setup AP so neither the Wi-Fi password nor the
     OAuth refresh token ever crosses plain-HTTP LAN.
   - If you submit **Iterate Mind** first (while Wi-Fi is not yet saved), the board writes the mind
     credential and stays in Setup Mode so you can immediately pick your 2.4 GHz Wi-Fi network and
     tap **Save Wi-Fi & Connect** in one visit; it then reboots straight into Normal Mode.
   - If you submit **Wi-Fi** only, the board joins your network, breathes **calm teal** on the
     ring, and advertises `http://iterate-<id>.local/` over mDNS/Bonjour (`_http._tcp` and
     `_iterate._tcp`).
5. **Check status, adjust non-secret settings, or reconnect a mind later from the LAN.** Open
   `http://iterate-<id>.local/` from any browser on the same Wi-Fi to view status (Wi-Fi SSID,
   RSSI, IP, mind origin, connection state, firmware version). To make changes in **that browser**:
   - Enter the 12-character setup code from the card (**Unlock this browser**), **or**
   - Slide the **Mute** switch ON, short-tap the centre button (opens a 30 s one-time claim window),
     refresh `http://iterate-<id>.local/`, and tap **Claim physical unlock**.
   - Once unlocked in that browser (3 minutes), you can change **Status Voice** or **Speaker
     Volume**, reboot, reset Wi-Fi, or tap **Enter Setup Mode** (which reboots once into the
     WPA2-PSK SoftAP _without_ erasing saved Wi-Fi or Mind so you can replace the Mind refresh
     token or Wi-Fi password over WPA2 and reboot straight back onto your network).

### Wi-Fi reset vs. full wipe (no screen)

| Action            | Physical gesture                                                                | Ring feedback                                                                                         | What changes                                                                                                                                                                                                     |
| ----------------- | ------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **LAN unlock**    | Mute switch **ON**, short tap centre button (`< 1.5 s`)                         | 2 s green pulse                                                                                       | Arms a 30 s **one-time** physical-unlock claim (does not start a call). The first browser to click **Claim physical unlock** receives an exclusive 3-minute unlocked session cookie                              |
| **Wi-Fi reset**   | Mute switch **OFF**, hold centre button **5 s** (release between 5 s and 10 s)  | Cyan arc fills `1.5–5 s`, then rapid cyan blink; 3 cyan flashes on verified clear                     | Disables portal writes, clears RAM Wi-Fi, stops HTTP server, clears **only** Wi-Fi SSID/password from flash, verifies read-back (preserving mind, rotated token, status voice, and setup code); reboots to Setup |
| **Full wipe**     | Mute switch **ON**, hold centre button **10 s** (release between 10 s and 15 s) | Red arc fills `1.5–10 s`, then rapid red blink; 3 red flashes on verified erase                       | Disables portal writes, zeroes RAM credentials, stops HTTP server, erases `iterate_kit` partition + NVS `iterate_token` + NVS `kit/volume`, verifies all three by read-back (preserves `kit_setup` card code)    |
| **Erase failure** | (If flash or NVS erase / read-back verification fails)                          | Alternating **red / amber fault ring** continuously (never plays success flashes, never reboots okay) | Leaves portal stopped and writes permanently disabled so the user is never misled into thinking credentials were erased before handing the board on                                                              |

Holding past the upper bound (`> 10 s` unmuted, `> 15 s` muted) or toggling the mute switch
mid-hold cancels safely instead of escalating. A button held at power-on (e.g. GPIO0 boot strap)
is ignored until released once.

Because `board.c` starts voice calls on the centre button's down-edge (for minimal voice latency),
an unmuted hold emits `end_press` once it reaches `1.5 s` (as the cyan Wi-Fi reset arc appears),
cancelling any opening/active call cleanly so neither an aborted hold nor a completed Wi-Fi reset
leaves a call running.

## 2. Provisioning options compared

### What the HAVPE hardware supports

- **ESP32-S3** with 2.4 GHz Wi-Fi (802.11b/g/n) and Bluetooth 5.0 LE, 16 MiB flash (`ota_0` /
  `ota_1` 5 MiB each, `iterate_kit` 4 KiB), 8 MiB octal PSRAM.
- **No NFC chip** on the PCB (only an external passive NFC sticker on the enclosure is possible).
- **No screen** — only a 12-LED WS2812 ring, centre button (GPIO0), rotary dial (GPIO16/18),
  hardware mute switch (GPIO3), and speaker/mics (XMOS + AIC3204).

### Trade-offs

| Option                                          | Works on stock iOS (no native app)?                                                               | Works on stock Android (no native app)?                             | Hardware fit on HAVPE                                     | Verdict / trade-offs                                                                                                                                                                                                                                           |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- | --------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **SoftAP + captive portal + `WIFI:` QR card**   | **Yes** (Camera joins WPA2 SoftAP; CNA opens portal)                                              | **Yes** (Camera joins WPA2 SoftAP; CaptivePortalLogin opens portal) | Native Wi-Fi + `esp_http_server`; zero BLE RAM/flash cost | **Chosen.** Only zero-app path that works identically on stock iOS and Android while encrypting Wi-Fi and Mind credentials over the air (60-bit WPA2-PSK) so secrets never cross plain-HTTP LAN. Trade-off: phone briefly leaves home Wi-Fi during Setup Mode. |
| **Improv / BLE provisioning (Web Bluetooth)**   | **No** — WebKit/iOS Safari has no Web Bluetooth                                                   | Yes (Chrome Web Bluetooth)                                          | ESP32-S3 has BLE 5.0                                      | Rejected as primary route because iOS requires a native app (or third-party BLE browser like Bluefy). Also adds BLE stack flash + internal RAM overhead alongside TLS and WakeNet.                                                                             |
| **Matter commissioning**                        | Via Apple Home (requires Matter fabric/hub)                                                       | Via Google Home (requires Matter fabric/hub)                        | ESP32-S3 supports `esp-matter`                            | Rejected: requires CSA vendor/product certification for distribution, drags in a large C++ stack (~1+ MiB flash), still depends on an Apple/Google hub ecosystem, and Matter has no concept of an iterate mind credential (still needs a second step).         |
| **Passive NFC sticker**                         | **Partial** — iOS reads `http(s)://` URLs and App Clips, but **ignores Wi-Fi (WSC) NDEF records** | Yes (reads both URLs and Wi-Fi WSC records)                         | External NTAG215 sticker only (no onboard NFC)            | Cannot replace the `WIFI:` QR code on stock iOS without a native app / App Clip. Useful as an optional companion sticker encoding `http://iterate-<id>.local/` for quick LAN page access after setup.                                                          |
| **Espressif `wifi_provisioning` (`protocomm`)** | **No** — requires Espressif's native app or custom native app                                     | **No** — requires native app                                        | Built into ESP-IDF                                        | Rejected: `protocomm` speaks protobuf + SRP6a/X25519 over custom GATT or HTTP endpoints that stock phone browsers cannot drive natively.                                                                                                                       |

### What needs a native app (or platform change)

- **"Hold your phone near the puck" (BLE proximity / NFC Wi-Fi join on iOS):** Requires either a
  native iOS app (CoreBluetooth / `NEHotspotConfigurationManager` / App Clip) or Apple's MFi/Matter
  ecosystem. Without a native app, scanning a `WIFI:` QR code is the closest stock-phone equivalent.
- **One-tap mind sign-in without pasting a refresh token (future platform work, not built here):**
  Today a board mounts on a mind using `os_base_url`, `oauth_client_id`, and `oauth_refresh_token`
  (`itx_mount.h`, `token_refresh.h`), which Kit Flasher obtains via browser OAuth PKCE
  (`src/firmware/mind-sign-in.ts`). Because the LAN page runs on plain HTTP at
  `http://iterate-<id>.local/` and cannot host an HTTPS OAuth redirect URI trusted by a mind's
  issuer, this prototype accepts the existing three mind fields only on the encrypted WPA2 SoftAP
  without changing any server-side auth.
  A cleaner future flow on the platform side would be **OAuth 2.0 Device Authorization Grant
  (RFC 8628)** on the mind's issuer (`POST /oauth2/device_authorization` + polling `/oauth2/token`):
  the user types only their mind domain on `http://iterate-<id>.local/`, the board asks the mind
  for a short user code and verification URL, the user approves the board in their logged-in
  browser, and the refresh token arrives directly over HTTPS from the mind's issuer to the board —
  with no token copy-paste and no secrets ever crossing plain HTTP.

## 3. Security model

| Surface                                              | Who can reach it                                                                                     | Allowed actions                                                                                                                                                                                                                                                | Protections                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| ---------------------------------------------------- | ---------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **SoftAP (`iterate-<id>`, `192.168.4.1`)**           | Only someone within Wi-Fi range who has the per-device 14-char setup code (printed on the card / QR) | View setup page, scan 2.4 GHz networks, submit Wi-Fi credentials (`POST /wifi`), submit Mind/Project credentials (`POST /mind`), change non-secret settings (`POST /settings`), exit Setup Mode (`POST /reboot`)                                               | Active **only** when Wi-Fi is unconfigured, physically reset, or explicitly entered once via `POST /enter-setup`. WPA2-PSK (60-bit hardware-random passphrase) encrypts over-the-air traffic so neither the home Wi-Fi password nor the OAuth refresh token crosses an open LAN. Per-client `HttpOnly; SameSite=Strict` session cookie (`kit_sid`) + per-session 128-bit CSRF token checked on every POST. `Host` header restricted to `192.168.4.1` / `iterate-<id>.local`.                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| **LAN page (`http://iterate-<id>.local/`, port 80)** | Devices on the same local network                                                                    | **Read-only (`GET /`, `GET /api/status`):** view status (Wi-Fi SSID, RSSI, IP, mind origin, connection state, firmware version, status voice, volume). **Write (non-secret only):** `POST /settings`, `POST /enter-setup`, `POST /reset-wifi`, `POST /reboot`. | **No credentials on plain-HTTP LAN:** `POST /wifi` and `POST /mind` are neither rendered nor accepted on LAN (server returns `403 Forbidden`). Read-only endpoints **never** expose stored secrets (Wi-Fi password, OAuth refresh token, project API key, or setup code) or other clients' unlock status. Every mutating POST requires: (1) valid `Host` header (blocks DNS rebinding), (2) a valid per-client `HttpOnly; SameSite=Strict` `kit_sid` cookie + matching per-session 128-bit CSRF token, and (3) an active 3-minute unlock bound **exclusively to that client's session** (earned by submitting the 60-bit setup code to `POST /unlock` with exponential backoff, or claiming a 30 s one-time physical Mute+tap window via `POST /claim-unlock`). Both unlock paths rotate the session cookie and CSRF token; nothing extends an unlock window without re-entering the setup code or pressing the hardware button again. |
| **USB-Serial-JTAG console**                          | Anyone with physical USB access to the puck                                                          | Reads boot logs, including `setup_ap` and `setup_qr` while in Setup Mode                                                                                                                                                                                       | Anyone with physical USB access can already trigger Mute+tap unlock, hold for Wi-Fi reset/wipe, or dump/reflash flash directly; USB serial disclosure stays inside the physical-possession boundary rather than adding a remote attack surface.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| **External / cloud**                                 | None                                                                                                 | None                                                                                                                                                                                                                                                           | No cloud endpoint, tunnel, or remote admin API is opened. Existing Cap'n Web session to the mind is unchanged.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |

### Design choices and residual trade-offs

- **Why credentials are accepted only on the WPA2 SoftAP (never on plain-HTTP LAN):** Browsers do
  not trust self-signed HTTPS certificates for `.local` hostnames, so `http://iterate-<id>.local/`
  must run on plain HTTP. Because these boards are handed to friends and early adopters who may put
  them on shared or guest Wi-Fi networks, a trusted-LAN assumption does not hold: a passive LAN
  observer could sniff a pasted OAuth refresh token, and a LAN attacker spoofing mDNS could collect
  it. Accepting Wi-Fi and Mind credentials exclusively on the board's own 60-bit WPA2-PSK SoftAP
  (`192.168.4.1`) guarantees that neither the OAuth refresh token nor the Wi-Fi password ever
  crosses plain-HTTP LAN, while keeping the LAN page useful for status, volume/voice settings, and
  one-click entry into Setup Mode (`POST /enter-setup`, via a one-boot RTC flag that preserves saved
  Wi-Fi and Mind credentials).
- **Per-client sessions, rotation, and physical-claim trade-off:** Each browser receives its own
  128-bit `kit_sid` cookie (`HttpOnly; SameSite=Strict; Path=/`) and its own 128-bit CSRF token.
  Unlocking via `POST /unlock` or `POST /claim-unlock` rotates both tokens and unlocks **only** that
  session slot; other LAN clients remain locked and cannot read that session's CSRF token or
  piggyback on its unlock. Unauthenticated `GET /` floods evict only inactive or oldest locked slots
  so an active unlocked session is not displaced.
  - _Residual trade-off on plain-HTTP LAN:_ A hostile on-path LAN attacker who can sniff plain HTTP
    traffic on the LAN can still steal the `kit_sid` cookie or setup code if submitted on LAN, or
    race to click `POST /claim-unlock` during the 30-second window after a physical Mute+tap. That
    attacker still **cannot** submit or steal a Mind refresh token or Wi-Fi password over LAN (only
    adjust non-secret volume/status-voice or reboot into WPA2 Setup Mode, which still requires the
    60-bit WPA2 passphrase from the card to join).
- **Why 40 bits is not enough for a WPA2 passphrase (and why we use 60 bits):** WPA2-PSK uses
  PBKDF2-HMAC-SHA1 (4,096 iterations). An 8-character Crockford Base32 code (40 bits,
  `2^40 ≈ 1.1 × 10^12` candidates) can be cracked offline from a single passively captured 4-way
  handshake during Setup Mode in roughly **8 hours (~$100–$120)** on a rented 8× RTX 4090 rig
  (~19M PMKs/s). Lengthening the setup code to **12 Crockford Base32 characters (`xxxx-xxxx-xxxx`,
  60 bits, `2^60 ≈ 1.15 × 10^18`)** raises that offline attack to roughly **~960 years / >$100M** on
  the same rig while keeping a single QR-scannable code on the card.
- **Hardware RNG ordering (setup code AND session/CSRF tokens in all three boot modes):** On
  ESP32-S3, `esp_random()` / `esp_fill_random()` return pseudo-random numbers unless Wi-Fi/BT RF is
  active or `bootloader_random_enable()` is enabled. Because `bootloader_random_enable()`
  reconfigures SAR ADC units 1 & 2 on ESP32-S3 (though unlike original ESP32 it does not touch I2S)
  and must be disabled before Wi-Fi starts, all three boot modes start Wi-Fi RF (`esp_wifi_start()`)
  **before** setting `rf_entropy_ready = true`, creating/loading the setup code, or starting the HTTP
  server that mints per-session and per-session CSRF tokens:
  - **Setup AP mode:** starts `WIFI_MODE_STA` to draw RF entropy for the setup code without beaconing
    an unconfigured AP, stops Wi-Fi (`rf_entropy_ready = false`), then starts `WIFI_MODE_APSTA`
    (`rf_entropy_ready = true`) before starting `esp_http_server` (which mints session/CSRF tokens).
  - **LAN Unlinked mode:** starts `WIFI_MODE_STA`, sets `rf_entropy_ready = true`, loads/creates the
    setup code, and starts mDNS + `esp_http_server`.
  - **Normal mode:** defers `init_secrets_with_rf_active()` and `start_http_server()` until Wi-Fi
    STA is already running and has an IPv4 lease (`rf_entropy_ready = true`).
- **Atomic write-lock, server stop, RAM wipe, and verified erase:** Before any physical Wi-Fi reset
  or full wipe, the board takes `portal.lock`, latches `writes_disabled = true`, zeroes the
  in-memory Wi-Fi (or full config) copies, releases the lock so any queued HTTP handler fails fast
  without deadlocking, and calls `httpd_stop()` to drain all HTTP handlers. Only then does it erase
  flash and NVS, check every NVS erase/commit return code, and verify by read-back (every byte
  `0xFF` on `iterate_kit` for wipe, decoded `has_wifi == false` for Wi-Fi reset, and
  `ESP_ERR_NVS_NOT_FOUND` on `iterate_token` and `kit/volume`). If any step or read-back fails, the
  ring displays an unmistakable alternating red/amber fault pattern continuously instead of showing
  success flashes or rebooting.

## 4. What was built, tested, and NOT proven

### Tested on the host (`pnpm firmware:test:host`, ASan + UBSan, GCC 16 host and GCC 13 Ubuntu 24.04 container)

- `ITERKIT1` C encoder and partial-config decoder (`components/core/src/configuration.c`), verified
  byte-for-byte against the TypeScript golden images (`configuration_test.c`), plus Wi-Fi-only,
  Wi-Fi-reset (mind preserved), password/voice validation, and mind-origin normalization.
- Boot-mode selection, short-ID/hostname derivation, 60-bit Crockford Base32 setup-code generation
  (`xxxx-xxxx-xxxx`) and constant-time matching, 128-bit session/CSRF token formatting, `WIFI:` QR
  escaping, Host-header DNS-rebinding filter, per-client session isolation, per-session CSRF
  verification, token rotation on unlock, refusal to extend an unlock window without re-submitting
  the setup code, brute-force backoff, 30 s one-time physical-claim consumption and expiry,
  eviction protection for unlocked sessions, and `writes_disabled` write-lock refusal
  (`provisioning_state_test.c`).
- RFC 1035 captive DNS responder for A/ANY, AAAA NODATA, and malformed packets (`captive_dns_test.c`).
- Cookie header parsing (`iterate_kit_web_cookie_get`), mode action policy (`WIFI` and `MIND`
  permitted only in `SETUP_AP`, refused on LAN), captive-probe detection, form URL decoding, HTML
  XSS escaping, Wi-Fi/Mind/Settings form handling, verification that LAN HTML omits all password and
  refresh-token inputs and exposes `Enter Setup Mode` + `Claim physical unlock`, and secret-redaction
  checks on rendered HTML and `/api/status` JSON (`web_portal_test.c`).
- HAVPE physical recovery gesture timing (boot-held ignore, debounce, unmuted 5 s Wi-Fi reset,
  muted 10 s full wipe, muted tap LAN unlock, mid-hold mute cancel, over-hold cancel), LED ring
  mode/gesture/erase-error renderers, and end-to-end call cancellation through `button.c` +
  `session_grammar.c` proving a Wi-Fi reset hold cancels any opening/active call at 1.5 s and never
  leaves a call running (`havpe_reset_gesture_test.c`).

### Built only (compiled with ESP-IDF 6.1 for `esp32s3`, partition layout checked)

- `iterate-kit-havpe.bin` builds cleanly under ESP-IDF 6.1 and passes
  `scripts/firmware-release.ts build` (partition table layout check, `iterate_kit` non-overlap
  check, and `filesOutsideInputs` check).
- `platforms/iterate_esp_idf/configuration.c`: flash read/write/clear-Wi-Fi/full-wipe with NVS
  error checks and read-back verification (`verify_partition_erased`, `iterate_kit_platform_refresh_token_clear`,
  `clear_and_verify_nvs_volume`), and NVS setup-code persistence.
- `platforms/iterate_esp_idf/components/board/led_ring.c` and `board.c`: custom ring pixel overlay
  and current-volume getter.
- `devices/havpe/havpe_portal.c`, `havpe_device.c`, `havpe_ui.c`: RF-first HWRNG gating for setup
  code and per-session/CSRF tokens in all three modes, one-boot RTC `enter-setup` flag, SoftAP
  (`WIFI_MODE_APSTA`), 2.4 GHz scan, UDP/53 captive DNS task, `esp_http_server` routes with
  per-client `kit_sid` `HttpOnly; SameSite=Strict` cookies and HTTP 403 refusal of `/wifi` & `/mind`
  on LAN, locked/stopped/verified wipe & reset path with red/amber failure ring, `espressif/mdns`
  advertisement (`_http._tcp` + `_iterate._tcp`), and Normal-mode `read_gestures` call-cancellation
  wiring.

### NOT proven (no hardware attached)

- **Nothing ran on physical HAVPE hardware.**
- Real hardware entropy quality on silicon and the `WIFI_MODE_STA` -> `esp_wifi_stop()` ->
  `WIFI_MODE_APSTA` startup transition timing in Setup AP mode.
- Real iOS (CNA) and Android (CaptivePortalLogin) captive-portal sheet pop-up behavior, cookie
  persistence inside OS captive-portal mini-browsers (iOS CNA / Android CaptivePortalLogin) during
  `GET /` -> `POST /wifi` / `POST /mind` redirects, QR scan UX, and `iterate-<id>.local` mDNS
  resolution on real home routers/phones.
- One-boot RTC `setup_once_rtc` transition across `esp_restart()` on real ESP32-S3 hardware.
- Physical LED ring brightness/legibility (including the red/amber erase-failure ring), button/mute
  feel during 5 s / 10 s holds, and audible chime/status-voice feel during the 1.5 s before a Wi-Fi
  reset hold cancels the opening call.
- Internal RAM headroom and audio latency when `esp_http_server` + `espressif/mdns` run alongside
  active GPT-Live audio, TLS, and WakeNet on real silicon.
- Flash and NVS erase/rewrite durability, read-back verification under real flash faults, and
  power-loss recovery of `iterate_kit` on real flash.
