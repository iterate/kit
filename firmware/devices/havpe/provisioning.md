# HAVPE preloadable provisioning, local UI, and recovery

Prototype design and trade-off notes for handing out Home Assistant Voice Preview Edition (HAVPE)
boards preloaded with iterate firmware, so a recipient can set up Wi-Fi and link an iterate mind
from a stock iOS or Android phone without a native app or USB cable, inspect/adjust the board on
the LAN, and recover Wi-Fi cleanly.

## 1. User experience (step by step)

Each preloaded HAVPE ships with a small printed card (or bottom sticker) showing:

- a standard `WIFI:T:WPA;S:iterate-<id>;P:<setup-code>;;` QR code,
- the 14-character SoftAP passphrase (`xxxx-xxxx-xxxx`, 12 Crockford Base32 characters, 60 bits of
  entropy — used **only** for joining the WPA2 SoftAP, never entered on the LAN), and
- its LAN address (`http://iterate-<id>.local/`).

### Preloading / factory card step (before handing out a board)

1. **Flash the HAVPE over USB** with the iterate firmware release (`esptool.py` / `idf.py flash`),
   leaving the `iterate_kit` partition erased (`0xFF`).
2. **Keep USB connected and open the USB-Serial-JTAG console** (`115200` baud) while the board
   boots for the first time.
3. **Read the generated setup lines.** Once Wi-Fi RF starts (enabling the ESP32-S3 hardware RNG),
   the board generates its permanent 60-bit SoftAP passphrase (`xxxx-xxxx-xxxx`), saves it to NVS
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
     `refresh_token`) live exclusively on the WPA2-encrypted Setup AP so neither the Wi-Fi password
     nor the OAuth refresh token ever crosses plain-HTTP LAN.
   - If you submit **Iterate Mind** first (while Wi-Fi is not yet saved), the board writes the mind
     credential and stays in Setup Mode so you can immediately pick your 2.4 GHz Wi-Fi network and
     tap **Save Wi-Fi & Connect** in one visit; it then reboots straight into Normal Mode.
   - If you submit **Wi-Fi** only, the board joins your network, breathes **calm teal** on the
     ring, and advertises `http://iterate-<id>.local/` over mDNS/Bonjour (`_http._tcp` and
     `_iterate._tcp`).
5. **Check status or adjust non-secret settings on the LAN (`http://iterate-<id>.local/`).**
   - Anyone on the LAN can view read-only status (Wi-Fi SSID, RSSI, IP, mind origin, connection
     state, firmware version).
   - To adjust **Status Voice** or **Speaker Volume** from a LAN browser (the only writes allowed on
     LAN; the setup code is never entered on LAN):
     1. Slide the **Mute** switch ON and short-tap the centre button (**Tap 1** — ring breathes calm
        white for 30 s).
     2. Refresh `http://iterate-<id>.local/` and click **Request pairing pattern**. The browser
        shows a colour and ring quadrant (for example _Magenta at Right (3 o'clock)_) and the puck's
        LED ring lights up the exact colour and quadrant of the claim it holds.
     3. Verify the puck's ring matches your browser, then keep Mute ON and short-tap the centre
        button again (**Tap 2** — 2 s green confirmation pulse). Only that confirmed browser gets a
        3-minute session for Status Voice and Volume. Anything else (sliding Mute OFF, holding a
        button, clicking Cancel, or letting 30 s expire) cancels cleanly with zero write access.
6. **Reconnect a Mind or change Wi-Fi later (physical gesture -> encrypted Setup Mode).**
   - No LAN client can enter Setup Mode, reset Wi-Fi, reboot, or touch credentials.
   - Slide **Mute ON** and hold the centre button **3 s** (release during the **violet blink**,
     `3–6 s`). The puck reboots once into its encrypted WPA2 SoftAP _without_ erasing saved Wi-Fi or
     Mind; scan the card's QR code to join over WPA2, update Mind or Wi-Fi (or click **Exit Setup
     Mode & Return to Wi-Fi**), and it restarts back onto your network.

### Physical gestures (no screen)

| Action                          | Physical gesture                                                                | Ring feedback                                                                                                      | What changes                                                                                                                                                                                                       |
| ------------------------------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **LAN browser pairing**         | Mute switch **ON**, short tap (`< 1.5 s`) twice with visual ring check          | Tap 1: calm breathing white (`30 s`); after browser requests: 3-LED quadrant arc in colour; Tap 2: 2 s green pulse | Tap 1 opens a 30 s claim slot; browser requests a random colour+quadrant shown on the ring; Tap 2 confirms **only** that browser for 3 min (Status Voice & Volume only). Sliding Mute OFF or anything else cancels |
| **Enter Setup Mode (no erase)** | Mute switch **ON**, hold centre button **3 s** (release between 3 s and 6 s)    | Violet arc fills `1.5–3 s`, then rapid violet blink (`3–6 s`); 3 violet flashes on trigger                         | Arms a one-boot RTC flag and reboots into the encrypted WPA2 SoftAP **without** erasing saved Wi-Fi or Mind credentials. Releasing during `6–10 s` (growing red arc) is a safe dead-zone that does nothing         |
| **Wi-Fi reset**                 | Mute switch **OFF**, hold centre button **5 s** (release between 5 s and 10 s)  | Cyan arc fills `1.5–5 s`, then rapid cyan blink (`5–10 s`); 3 cyan flashes on verified clear                       | Latches writes off immediately (never waits for HTTP handlers), clears RAM Wi-Fi, clears **only** Wi-Fi SSID/password from flash, verifies read-back (preserving mind, rotated token, voice, and card code)        |
| **Full wipe**                   | Mute switch **ON**, hold centre button **10 s** (release between 10 s and 15 s) | Red arc fills `6–10 s`, then rapid red blink (`10–15 s`); 3 red flashes on verified erase                          | Latches writes off immediately (never waits for HTTP handlers), zeroes RAM credentials, erases `iterate_kit` + NVS `iterate_token` + NVS `kit/volume`, verifies all three by read-back (preserves card code)       |
| **Erase failure**               | (If flash or NVS erase / read-back verification fails)                          | Alternating **red / amber fault ring** continuously (never plays success flashes, never reboots okay)              | Leaves writes permanently disabled so the user is never misled into thinking credentials were erased before handing the board on                                                                                   |

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

| Surface                                              | Who can reach it                                                                                     | Allowed actions                                                                                                                                                                                                                                     | Protections                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| ---------------------------------------------------- | ---------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **SoftAP (`iterate-<id>`, `192.168.4.1`)**           | Only someone within Wi-Fi range who has the per-device 14-char setup code (printed on the card / QR) | View setup page, scan 2.4 GHz networks, submit Wi-Fi (`POST /wifi`), submit Mind/Project (`POST /mind`), change non-secret settings (`POST /settings`), exit one-boot Setup Mode (`POST /reboot`)                                                   | Active **only** when Wi-Fi is unconfigured, physically reset, or explicitly entered via the physical Mute ON + 3 s hold gesture. WPA2-PSK (60-bit hardware-random passphrase, never sent on LAN) encrypts over-the-air traffic so neither the home Wi-Fi password nor the OAuth refresh token crosses an open LAN. Per-client `HttpOnly; SameSite=Strict` session cookie (`kit_sid`) + per-session 128-bit CSRF token, tight body caps + 2 s deadline, and `Host` check (`192.168.4.1` / `iterate-<id>.local`).                                                                                                                                                                                                                                                                                                                 |
| **LAN page (`http://iterate-<id>.local/`, port 80)** | Devices on the same local network                                                                    | **Read-only (`GET /`, `GET /api/status`):** view status (Wi-Fi SSID, RSSI, IP, mind origin, connection state, firmware version, status voice, volume). **Write (low-impact, non-secret only):** `POST /settings` (Status Voice and Speaker Volume). | **No setup code, no credentials, and no reset/setup/reboot on LAN:** the setup code is never loaded or accepted on LAN; `POST /wifi`, `POST /mind`, and `POST /reboot` return `403 Forbidden`; no HTTP route exists to reset Wi-Fi or enter Setup Mode. Read-only endpoints never expose stored secrets or another browser's pairing state. `POST /settings` requires: (1) valid `Host` header (blocks DNS rebinding), (2) per-client `HttpOnly; SameSite=Strict` `kit_sid` cookie + per-session 128-bit CSRF token (checked before reading the 256-byte body under a 2 s deadline), and (3) a 3-minute session that the owner **visually confirmed** on the puck (Tap 1 -> browser requests random colour+quadrant -> owner checks ring -> Tap 2 confirms; anything else cancels; unconfirmed claims grant zero write access). |
| **USB-Serial-JTAG console**                          | Anyone with physical USB access to the puck                                                          | Reads boot logs, including `setup_ap` and `setup_qr` while in Setup Mode                                                                                                                                                                            | Anyone with physical USB access can already trigger physical gestures or dump/reflash flash directly; USB serial disclosure stays inside the physical-possession boundary rather than adding a remote attack surface.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| **External / cloud**                                 | None                                                                                                 | None                                                                                                                                                                                                                                                | No cloud endpoint, tunnel, or remote admin API is opened. Existing Cap'n Web session to the mind is unchanged.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |

### Design choices and residual trade-offs

- **The SoftAP passphrase never travels on the LAN:** The 60-bit setup code (`xxxx-xxxx-xxxx`) is
  used exclusively as the SoftAP's WPA2-PSK passphrase and is not even loaded into RAM in LAN modes.
  Because it never crosses plain-HTTP LAN, a passive LAN observer cannot capture it to later join or
  decrypt the provisioning SoftAP.
- **No credentials and no disruptive actions on plain-HTTP LAN:** Browsers do not trust self-signed
  HTTPS certs for `.local`, so `http://iterate-<id>.local/` runs over plain HTTP. On a shared or
  untrusted LAN, accepting Wi-Fi/Mind credentials or allowing remote Wi-Fi reset / Setup Mode entry
  would expose credentials or enable denial of service. Restricting LAN writes to non-secret
  **Status Voice** and **Speaker Volume** (and keeping Setup Mode entry, Wi-Fi reset, and full wipe
  strictly physical) keeps credentials off the LAN and bounds the impact of any plain-HTTP LAN
  attacker.
- **Visual out-of-band confirmation prevents blind claim hijacking:** A LAN browser cannot light up
  the ring without physical Tap 1 first, and claiming a window (`POST /claim-request`) gives **zero
  write access** until physical Tap 2 confirms it. If a LAN attacker races Tap 1 and claims first,
  the puck displays the attacker's random colour+quadrant (4 colours × 4 quadrants = 16 patterns),
  while the owner's browser shows _"Another browser has a pending pairing request"_ and never shows
  that pattern. Seeing no match, the owner slides Mute OFF (or waits 30 s) to cancel the attacker's
  claim without ever granting writes.
  - _Residual trade-off on plain-HTTP LAN:_ A hostile on-path LAN attacker who can sniff plain HTTP
    traffic after the owner completes Tap 2 can still read that browser's `kit_sid` cookie for the
    remainder of the 3-minute window; that lets them change only Status Voice or Speaker Volume
    (never credentials, never Wi-Fi reset, never Setup Mode, never reboot, and never the SoftAP
    passphrase).
- **Why 40 bits is not enough for a WPA2 passphrase (and why we use 60 bits):** WPA2-PSK uses
  PBKDF2-HMAC-SHA1 (4,096 iterations). An 8-character Crockford Base32 code (40 bits,
  `2^40 ≈ 1.1 × 10^12` candidates) can be cracked offline from a single passively captured 4-way
  handshake during Setup Mode in roughly **8 hours (~$100–$120)** on a rented 8× RTX 4090 rig
  (~19M PMKs/s). Lengthening the setup code to **12 Crockford Base32 characters (`xxxx-xxxx-xxxx`,
  60 bits, `2^60 ≈ 1.15 × 10^18`)** raises that offline attack to roughly **~960 years / >$100M** on
  the same rig while remaining QR-scannable on the card.
- **Hardware RNG ordering in all three boot modes:** On ESP32-S3, `esp_random()` /
  `esp_fill_random()` return pseudo-random numbers unless Wi-Fi/BT RF is active or
  `bootloader_random_enable()` is enabled. Because `bootloader_random_enable()` reconfigures SAR ADC
  units 1 & 2 on ESP32-S3 (though unlike original ESP32 it does not touch I2S) and must be disabled
  before Wi-Fi starts, all three boot modes start Wi-Fi RF (`esp_wifi_start()`) **before** setting
  `rf_entropy_ready = true`, creating/loading the SoftAP passphrase (Setup AP only), or starting the
  HTTP server that mints session cookies, per-session CSRF tokens, and visual pairing patterns.
- **Bounded request bodies and non-blocking physical erases:** Every POST endpoint enforces a small
  per-route body limit (`128` B for pairing/reboot, `256` B for settings, `512` B for Wi-Fi,
  `1024` B for Mind), verifies Host, mode policy, write-latch, and session/unlock **before** reading
  the body, and enforces a hard `2 s` monotonic deadline (plus 2 s `esp_http_server` recv/send
  timeouts) so a stalled client socket is closed instead of looping forever. `portal.lock` is never
  held across network `recv` or `send`, and physical Enter Setup Mode, Wi-Fi reset, and full wipe
  **never call blocking `httpd_stop()`**: they latch an atomic write-disable flag, acquire
  `portal.lock`, permanently disable guard writes, clear RAM copies, and execute the verified
  flash/NVS erase while holding `portal.lock`. Every NVS erase/commit return code is checked and
  verified by read-back (`0xFF` across `iterate_kit` for wipe, decoded `!has_wifi` for Wi-Fi reset,
  `ESP_ERR_NVS_NOT_FOUND` for `iterate_token` and `kit/volume`); any failure displays a continuous
  alternating red/amber fault ring instead of success flashes.

## 4. What was built, tested, and NOT proven

### Tested on the host (`pnpm firmware:test:host`, ASan + UBSan, GCC 16 host and GCC 13 Ubuntu 24.04 container)

- `ITERKIT1` C encoder and partial-config decoder (`components/core/src/configuration.c`), verified
  byte-for-byte against the TypeScript golden images (`configuration_test.c`), plus Wi-Fi-only,
  Wi-Fi-reset (mind preserved), password/voice validation, and mind-origin normalization.
- Boot-mode selection, short-ID/hostname derivation, 60-bit Crockford Base32 setup-code generation
  (`xxxx-xxxx-xxxx`), 128-bit session/CSRF token formatting, `WIFI:` QR escaping, Host-header
  DNS-rebinding filter, per-client session isolation, per-session CSRF verification, two-tap visual
  claim pairing (proving an unconfirmed claim grants **zero** write access, another browser cannot
  see or hijack a pending claim, Mute OFF / cancel / 30 s expiry all revoke cleanly, Tap 2 confirms
  only the claiming browser for 3 min with no extension on POST, eviction protection preserves
  pending and unlocked sessions), and `writes_disabled` write-lock refusal
  (`provisioning_state_test.c`).
- RFC 1035 captive DNS responder for A/ANY, AAAA NODATA, and malformed packets (`captive_dns_test.c`).
- Cookie header parsing (`iterate_kit_web_cookie_get`), mode action policy (`WIFI`, `MIND`, and
  `REBOOT` allowed only in `SETUP_AP`; only `SETTINGS` allowed on LAN), captive-probe detection,
  form URL decoding, HTML XSS escaping, Wi-Fi/Mind/Settings form handling, verification that LAN
  HTML has no setup-code unlock, no password/refresh-token inputs, and no reset/setup/reboot
  buttons, verification that the visual pairing pattern is rendered only for the claiming browser,
  and secret-redaction checks on rendered HTML and `/api/status` JSON (`web_portal_test.c`).
- HAVPE physical gestures (boot-held ignore, debounce, muted short tap `PAIR_TAP`, muted 3–6 s
  `ENTER_SETUP` with violet progress/armed feedback, muted 6–10 s safe dead-zone, muted 10–15 s
  `FULL_WIPE` with red feedback, unmuted 5–10 s `WIFI_RESET` with cyan feedback, mid-hold mute
  cancel, over-hold cancel), LED ring renderers (including Setup Mode violet, claim-open white,
  exact 3-LED quadrant claim pattern, and alternating red/amber erase-failure ring), and end-to-end
  call cancellation through `button.c` + `session_grammar.c` (`havpe_reset_gesture_test.c`).

### Built only (compiled with ESP-IDF 6.1 for `esp32s3`, partition layout checked)

- `iterate-kit-havpe.bin` builds cleanly under ESP-IDF 6.1 and passes
  `scripts/firmware-release.ts build` (partition table layout check, `iterate_kit` non-overlap
  check, and `filesOutsideInputs` check).
- `platforms/iterate_esp_idf/configuration.c`: flash read/write/clear-Wi-Fi/full-wipe with NVS
  error checks and read-back verification (`verify_partition_erased`,
  `iterate_kit_platform_refresh_token_clear`, `clear_and_verify_nvs_volume`), and NVS setup-code
  persistence.
- `platforms/iterate_esp_idf/components/board/led_ring.c` and `board.c`: custom ring pixel overlay
  and current-volume getter.
- `devices/havpe/havpe_portal.c`, `havpe_device.c`, `havpe_ui.c`: RF-first HWRNG gating in all
  three modes, SoftAP-only passphrase loading, one-boot RTC `setup_once_rtc` flag triggered by the
  physical Enter Setup Mode gesture, SoftAP (`WIFI_MODE_APSTA`), 2.4 GHz scan, UDP/53 captive DNS
  task, `esp_http_server` with 2 s recv/send timeouts, hard 2 s body deadline, per-route body caps,
  pre-body session/unlock checks, per-client `kit_sid` `HttpOnly; SameSite=Strict` cookies, HTTP 403
  refusal of `/wifi`, `/mind`, `/reboot` on LAN, non-blocking write-latched physical erase with
  red/amber failure ring, `espressif/mdns` advertisement (`_http._tcp` + `_iterate._tcp`), and
  Normal-mode `read_gestures` call-cancellation wiring.

### NOT proven (no hardware attached)

- **Nothing ran on physical HAVPE hardware.**
- Real hardware entropy quality on silicon and the `WIFI_MODE_STA` -> `esp_wifi_stop()` ->
  `WIFI_MODE_APSTA` startup transition timing in Setup AP mode.
- Real iOS (CNA) and Android (CaptivePortalLogin) captive-portal sheet pop-up behavior, cookie
  persistence inside OS captive-portal mini-browsers during `GET /` -> `POST /wifi` / `POST /mind`
  redirects, QR scan UX, and `iterate-<id>.local` mDNS resolution on real home routers/phones.
- Physical LED orientation mapping of the 12 WS2812 ring indices (0..11) to visual clock positions
  (`Top / Right / Bottom / Left` quadrants) on the HAVPE enclosure — pixel index `0` vs 12 o'clock
  must be checked on a real board so the browser diagram and physical ring quadrant align!
- One-boot RTC `setup_once_rtc` transition across `esp_restart()` on real ESP32-S3 hardware.
- Physical LED ring brightness/legibility (including violet Setup Mode, 4 pairing colours, and the
  red/amber erase-failure ring), button/mute hold ergonomics across the 3 s / 5 s / 10 s windows,
  and audible chime/status-voice feel during the 1.5 s before a Wi-Fi reset hold cancels an opening
  call.
- Stalled-client slow-POST stress testing against real lwIP / `esp_http_server` sockets concurrently
  with physical reset/wipe gestures.
- Internal RAM headroom and audio latency when `esp_http_server` + `espressif/mdns` run alongside
  active GPT-Live audio, TLS, and WakeNet on real silicon.
- Flash and NVS erase/rewrite durability, read-back verification under real flash faults, and
  power-loss recovery of `iterate_kit` on real flash.
