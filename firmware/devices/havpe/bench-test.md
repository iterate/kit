# HAVPE preloadable provisioning — bench test plan

Step-by-step hardware checklist for proving the `havpe-neon` provisioning prototype on Jonas's
bench Home Assistant Voice Preview Edition (HAVPE).

- **Target board identity:** ROM MAC / USB serial `D8:3B:DA:46:20:34` (see
  [jonas-test-devices.md](../../jonas-test-devices.md) and [firmware/README.md](../../README.md)).
- **Do not probe other ports:** never run `esptool read_mac` or open serial monitors on unknown
  ports (that can reset other attached bench boards). Always resolve the HAVPE's port passively via
  USB metadata.
- **No secrets in git or logs you commit:** keep Wi-Fi passwords, personal access tokens, and OAuth
  refresh tokens in shell environment variables or temporary files outside the repo.

---

## 1. Find the HAVPE USB port passively

- **Do:** From the repository root, resolve the port by USB serial:
  ```sh
  PORT="$(firmware/tools/port-for-mac.sh D8:3B:DA:46:20:34)"
  echo "$PORT"
  ```
- **Must see:** A single serial device path (e.g. `/dev/cu.usbmodem...` on macOS or `/dev/ttyACM...`
  on Linux). Other attached bench boards remain untouched.
- **If it fails, record:** Output of `port-for-mac.sh`, OS USB device listing (`ioreg` / `lsusb`),
  and whether the HAVPE USB-C cable is a data cable (not charge-only).

## 2. Build this branch and flash with an erased `iterate_kit` partition

- **Do:** Activate ESP-IDF 6.1, build the HAVPE release bundle, flash its parts, and erase ONLY the
  `iterate_kit` partition so the board boots as a fresh preloaded unit:
  ```sh
  node scripts/firmware-release.ts build \
    --device home-assistant-voice-preview-edition --out /tmp/kit-havpe-bench
  cd /tmp/kit-havpe-bench/release/assets
  CFG_OFFSET="$(jq -r .configurationPartition.offset manifest.json)"
  CFG_SIZE="$(jq -r .configurationPartition.size manifest.json)"
  python -m esptool --chip esp32s3 -p "$PORT" write_flash \
    $(jq -r '.builds[0].parts[]|"\(.offset) \(.path|ltrimstr("./"))"' manifest.json)
  python -m esptool --chip esp32s3 -p "$PORT" erase_region "$CFG_OFFSET" "$CFG_SIZE"
  ```
- **Must see:** Clean `esptool` write + hash verification of every part, and `erase_region` succeeds
  at `0x510000` (`0x1000` bytes).
- **If it fails, record:** Exact `firmware-release.ts` or `esptool` command output, `$PORT`, and
  `manifest.json` offsets.

## 3. Read `setup_ap` and `setup_qr` from serial on first boot

- **Do:** Open a read-only serial monitor at `115200` baud on `$PORT` (or `idf.py -p "$PORT"
monitor`) and power-cycle or reset the HAVPE. Render the printed `setup_qr` string as a QR code
  (e.g. `printf '%s' "$SETUP_QR" | qrencode -t ANSIUTF8` or any local QR generator) so phones can
  scan it.
- **Must see:**
  - Boot log contains `device is not provisioned: storage=decode failed — entering Setup Mode`.
  - After Wi-Fi starts, serial prints both lines:
    ```text
    havpe-portal: setup_ap ssid=iterate-<id> setup_code=<xxxx-xxxx-xxxx>
    havpe-portal: setup_qr WIFI:T:WPA;S:iterate-<id>;P:<xxxx-xxxx-xxxx>;;
    ```
  - `<id>` is `2034` (last two bytes of the Wi-Fi MAC; note exact value printed), and `setup_code`
    is 12 lowercase Crockford Base32 characters formatted `xxxx-xxxx-xxxx`.
  - The 12-LED ring shows a **rotating cyan comet** (not the red/amber fault ring).
- **If it fails, record:** Full serial boot log from `ESP-IDF v6.1 2nd stage bootloader` through
  `havpe-portal:` lines, and exact ring colour/animation seen.

## 4. Join SoftAP by QR code on an iPhone (stock iOS, no app)

- **Do:** Point the iPhone's built-in **Camera** app at the rendered `setup_qr` QR code and tap the
  _"Join `iterate-<id>` Network"_ banner. Wait up to 10 s for the Captive Network Assistant (CNA)
  sheet to appear. (If CNA does not auto-open, open Safari to `http://192.168.4.1/`.)
- **Must see:**
  - iPhone joins WPA2 network `iterate-<id>` without manually typing the passphrase.
  - Captive portal sheet opens automatically to `http://192.168.4.1/` and displays the Setup Mode
    page with nearby 2.4 GHz SSIDs, the Wi-Fi form, and the Iterate Mind form.
- **If it fails, record:** iOS version, whether WPA2 join succeeded, whether CNA auto-popped or
  required manual `http://192.168.4.1/`, any Safari/CNA error text, and `havpe-portal` serial logs.

## 5. Join SoftAP by QR code on an Android phone (stock Android, no app)

- **Do:** Disconnect the iPhone from `iterate-<id>`, then scan the same `setup_qr` QR code using an
  Android phone's stock Camera / Wi-Fi QR scanner and tap **Connect**. Wait for the _"Sign in to
  network"_ notification / CaptivePortalLogin window.
- **Must see:**
  - Android joins WPA2 network `iterate-<id>` without typing the passphrase.
  - Captive portal notification / sheet opens `http://192.168.4.1/` showing the Setup Mode page.
- **If it fails, record:** Android version & phone model, whether WPA2 join succeeded, whether
  CaptivePortalLogin auto-opened or required Chrome to `http://192.168.4.1/`, and serial logs.

## 6. Exercise captive page: made-up Wi-Fi, recover, then real Wi-Fi

- **Do:**
  1. On the captive page (`http://192.168.4.1/`), enter a **made-up 2.4 GHz SSID** (e.g.
     `nonexistent-bench-ap`) and a dummy 8+ char password, and tap **Save Wi-Fi & Connect**.
  2. Observe the reboot and serial log for ~15 s.
  3. Hold the **Wi-Fi reset** gesture (Mute switch **OFF**, hold centre button **5–10 s** until
     rapid cyan blink, then release) to return to Setup Mode.
  4. Rejoin `iterate-<id>` by QR code, pick your **real 2.4 GHz Wi-Fi network** (leave the Mind
     form blank for now), enter its real password, and tap **Save Wi-Fi & Connect**.
- **Must see:**
  - After made-up Wi-Fi: board reboots, ring pulses blue (connecting), serial logs Wi-Fi join
    failure (`not joined` / `STA_DISCONNECTED`), and the Wi-Fi reset gesture cleanly brings it back
    to Setup Mode (cyan comet).
  - After real Wi-Fi: board reboots into **LAN Awaiting-Mind** mode, joins the 2.4 GHz AP, serial
    logs its DHCP IP and `open http://iterate-<id>.local/`, and the ring breathes **calm teal**.
- **If it fails, record:** Whether cookies survived the `POST /wifi` redirect inside the phone's
  captive mini-browser (watch for `Session expired` / `Form token expired`), serial logs across
  reboot, and router band (confirm 2.4 GHz, not 5 GHz-only).

## 7. Check `http://iterate-<id>.local/` on the LAN

- **Do:** With your laptop or phone back on the same home/bench Wi-Fi, open
  `http://iterate-<id>.local/` (and optionally `http://iterate-<id>.local/api/status`).
- **Must see:**
  - mDNS resolves `iterate-<id>.local` to the board's IP and loads the LAN page.
  - Status shows Wi-Fi SSID, RSSI, IP, mode `awaiting_mind`, and firmware version `dev`.
  - **Security check:** page has NO setup-code box, NO Wi-Fi password or refresh-token input, NO
    reset/wipe/reboot button, and `/api/status` contains no secret values.
- **If it fails, record:** Client OS/browser, whether `dns-sd -B _iterate._tcp` / `avahi-browse -a`
  sees the board, whether the direct DHCP IP (`http://<ip>/`) works when `.local` does not, and
  HTTP response code/body.

## 8. Confirmed visual LAN pairing and volume change

- **Do:**
  1. First verify cancellation: slide Mute **ON**, short-tap centre button (**Tap 1** — calm
     breathing white ring), click **Request pairing pattern** on `http://iterate-<id>.local/`, note
     the browser's colour + quadrant, then slide Mute **OFF** without Tap 2. Confirm pairing cancels
     and volume cannot be saved.
  2. Now complete pairing: slide Mute **ON**, **Tap 1** (white ring), refresh/click **Request
     pairing pattern**, inspect the physical ring against the browser's colour + clock quadrant
     (`Top / Right / Bottom / Left`), wait **at least 2 seconds** (also try tapping at `< 2 s` if
     you want to verify early taps are ignored), then keep Mute **ON** and **Tap 2**.
  3. In the unlocked browser, change **Speaker Volume** (e.g. `50%`) and click **Save Settings**.
- **Must see:**
  - Ring shows a 3-LED quadrant arc in the exact colour and position shown by that browser (note
    specifically whether physical LED index `0` aligns with 12 o'clock on the HAVPE housing!).
  - Tap 2 after `>= 2 s` triggers a **2 s green confirmation pulse** and unlocks Status Voice +
    Volume for 3 minutes in that browser only.
  - Saving volume succeeds, updates the displayed percentage, and persists across a reboot.
- **If it fails, record:** Browser colour/quadrant vs physical LED positions seen (clockwise index
  offset if rotated), serial logs (`LAN browser pairing window opened`, `ignoring tap`, `confirmed`,
  `cancelled`), and any HTTP error banner.

## 9. Gesture 1 — Enter Setup Mode (no erase) and link Mind / Project

- **Do:**
  1. Slide Mute **ON**, hold centre button **3–6 s** (violet arc fills `1.5–3 s`, rapid violet
     blink `3–6 s`), and release during the violet blink.
  2. Rejoin `iterate-<id>` by QR code and open `http://192.168.4.1/`.
  3. Confirm saved Wi-Fi is still listed as configured (not erased).
  4. Enter your real Mind (`mind_url`, `client_id`, `refresh_token`) or bench project credential
     (keep secrets out of git), and submit.
- **Must see:**
  - Releasing during `3–6 s` flashes **3 violet pulses** and reboots once into Setup Mode (cyan
    comet) with Wi-Fi still intact.
  - After submitting Mind/Project, the board reboots into **Normal Mode**, joins Wi-Fi, connects to
    iterate, and reaches `READY` (serial logs `status: …` / ready state; ring shows idle/ready).
- **If it fails, record:** Exact hold duration, ring feedback, serial log showing whether
  `one-boot Setup Mode requested` fired across `ESP_RST_SW`, and any HTTP/mount error on serial.

## 10. Verify a voice call still works after setup

- **Do:** With Mute **OFF**, press the centre button once (or say `"jarvis"`), ask a short question
  (e.g. _"Say the single word banana"_), listen for the spoken reply, and press the button again to
  hang up. Optionally run the air-path script from `iterate`'s monorepo (`voice-board.ts --device
home_assistant_voice_preview_edition`; see [firmware/README.md](../../README.md#release-and-proof)).
- **Must see:**
  - Wake chime / spoken greeting plays cleanly, speech is heard and answered with working AEC, and
    `http://iterate-<id>.local/` still responds cleanly during and after the call with no audio
    glitches or watchdog resets.
- **If it fails, record:** Serial log around button press and call teardown, `health` counters if
  reachable, and whether mDNS/HTTP caused any audible stutter or OOM.

## 11. Gesture 2 — Wi-Fi reset (preserves Mind, cancels opening call)

- **Do:**
  1. In Normal Mode with Mute **OFF**, hold the centre button continuously for **5–10 s** (past the
     `1.5 s` mark where the cyan arc begins filling, into the `5–10 s` rapid cyan blink), then
     release.
  2. Rejoin `iterate-<id>` by QR code and inspect `http://192.168.4.1/`.
  3. Re-enter only your real 2.4 GHz Wi-Fi (do NOT re-enter Mind) and tap **Save Wi-Fi & Connect**.
- **Must see:**
  - The down-edge may begin opening a call, and at `1.5 s` (as the cyan arc appears) the opening
    call cancels cleanly.
  - Releasing during `5–10 s` flashes **3 cyan pulses** and reboots into Setup Mode (cyan comet).
  - Setup page shows Mind/Project **still linked** while Wi-Fi is cleared.
  - Saving Wi-Fi alone reboots straight back into Normal Mode and reconnects to the Mind without
    re-entering Mind credentials.
- **If it fails, record:** Whether a ghost call stayed open after `1.5 s` or after aborting a 3 s
  hold, ring colours seen, and serial log of `Wi-Fi reset` and reboot.

## 12. Gesture 3 — Full wipe (preserves card `setup_code`)

- **Do:**
  1. Slide Mute **ON**, hold the centre button **10–15 s** (violet `1.5–6 s` -> growing red dead
     zone `6–10 s` -> rapid red blink `10–15 s`), and release during the rapid red blink.
  2. Watch serial and rejoin `iterate-<id>` using the **same** QR code from Step 3.
- **Must see:**
  - Releasing during `10–15 s` flashes **3 red pulses** and reboots promptly into Setup Mode.
  - Serial prints the **same** `setup_code` as Step 3 (`kit_setup` preserved).
  - Setup page shows **neither** Wi-Fi nor Mind configured, and saved volume is back to default
    (`100%`).
  - (Optional safety check: hold `6–10 s` muted and release in the red fill dead-zone — must do
    nothing; hold `> 15 s` muted — must cancel safely without wiping.)
- **If it fails, record:** Hold timing, whether fault ring (3 s red/amber) appeared instead of 3 red
  flashes, serial output, and whether any prior SSID/Mind/volume remained after reboot.

## 13. Restore the bench HAVPE to its released firmware

- **Do:** Put the bench board back on its released `main` firmware and bench provisioning image so
  ordinary bench testing is unaffected:
  1. Either flash the latest released HAVPE build via [k.iterate.com](https://k.iterate.com) over
     Web Serial (signing in with the bench account / project from
     [jonas-test-devices.md](../../jonas-test-devices.md)), **or** download the latest GitHub
     release assets (`gh release download` for the latest
     `kit-firmware/home-assistant-voice-preview-edition/*` tag) and flash them with `esptool` plus
     a freshly built bench config image:
     ```sh
     node scripts/config-image.ts image \
       --wifi-ssid "$BENCH_WIFI_SSID" --wifi-password "$BENCH_WIFI_PASSWORD" \
       --os-base-url https://os.iterate.com \
       --project-id prj_3afc6e86c9202218ef3c6b7059e5be82 \
       --project-api-key "$KIT_BENCH_TOKEN" \
       --status-voice greensleeves --out /tmp/havpe-bench-cfg.bin
     python -m esptool --chip esp32s3 -p "$PORT" write_flash \
       $(jq -r '.builds[0].parts[]|"\(.offset) \(.path|ltrimstr("./"))"' manifest.json) \
       "$(jq -r .configurationPartition.offset manifest.json)" /tmp/havpe-bench-cfg.bin
     rm -f /tmp/havpe-bench-cfg.bin
     ```
  2. Press the centre button once after boot and confirm it mounts and completes a short call.
- **Must see:** Released firmware boots (non-`dev` version in boot log), joins bench Wi-Fi, mounts
  `prj-kit-bench`, reaches `READY`, and completes a call. Temporary config file is deleted.
- **If it fails, record:** Release tag flashed, `esptool` output, and serial boot log.
