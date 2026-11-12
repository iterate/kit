#!/usr/bin/env bash
# scripts/ci/qemu-boot.sh — A BOARD'S FIRMWARE BOOTS IN ESPRESSIF'S QEMU: the chip, the second-stage
# bootloader, the partition table, octal PSRAM, and the app up to its first look at the board's
# configuration. No board is needed.
#
#   source "$IDF_PATH/export.sh"; scripts/ci/qemu-boot.sh [target]      (default: havpe)
#
# It builds the target with targets/common/sdkconfig.qemu (the console on UART0: QEMU shows no
# USB-Serial-JTAG), merges the build into one 16 MB flash image, and boots it with an empty
# configuration partition. The app must reach app_main and say the board is not provisioned.
#
# What QEMU does not have, so this does not test: I2S and the audio parts, Wi-Fi and Bluetooth.
# A provisioned image goes on to the board's audio setup and stops there, so this boots an
# unprovisioned one.
#
# QEMU is Espressif's release, pinned and checked by its SHA-256; QEMU=<path> uses another
# qemu-system-xtensa (a Mac: the release's aarch64-apple-darwin build).
set -euo pipefail

target="${1:-havpe}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/qemu-boot-$target"
release=esp-develop-9.2.2-20261109
archive=qemu-xtensa-softmmu-esp_develop_9.2.2_20261109-x86_64-linux-gnu.tar.xz
sha256=fbc3bffa9fa71192847969a627a584aabe2e86d85bed4902a096ff06c2b8a815
mkdir -p "$work"

if [ -z "${QEMU:-}" ]; then
  if [ ! -x "$work/qemu/bin/qemu-system-xtensa" ]; then
    curl -fsSL --retry 3 -o "$work/$archive" \
      "https://github.com/espressif/qemu/releases/download/$release/$archive"
    echo "$sha256  $work/$archive" | sha256sum -c -
    tar -xJf "$work/$archive" -C "$work"
  fi
  QEMU="$work/qemu/bin/qemu-system-xtensa"
fi

# The avatar atlases are generated and gitignored (components/avatar/src/.gitignore).
(cd "$root" && python3 firmware/tools/generate-atlases.py)
ITERATE_KIT_QEMU=1 idf.py -C "$root/firmware/targets/$target" -B "$work/build" \
  -D IDF_TARGET=esp32s3 -D SDKCONFIG="$work/sdkconfig" build
grep -q '^CONFIG_ESP_CONSOLE_UART_DEFAULT=y' "$work/sdkconfig" ||
  { echo "the build's console is not UART0: sdkconfig.qemu was not applied" >&2; exit 1; }
(cd "$work/build" && python -m esptool --chip esp32s3 merge-bin --pad-to-size 16MB \
  -o "$work/flash.bin" @flash_args)

psram=(-m 8M)
grep -q '^CONFIG_SPIRAM_MODE_OCT=y' "$work/sdkconfig" &&
  psram+=(-global driver=ssi_psram,property=is_octal,value=true)
status=0
timeout 60 "$QEMU" -nographic -machine esp32s3 "${psram[@]}" \
  -drive "file=$work/flash.bin,if=mtd,format=raw" -serial mon:stdio </dev/null >"$work/boot.log" 2>&1 ||
  status=$?
cat "$work/boot.log"
# timeout ends QEMU with 124; anything else is QEMU failing on its own
[ "$status" = 124 ] || { echo "QEMU ended with $status before the timeout" >&2; exit 1; }
for broken in "Guru Meditation" "abort() was called" "Rebooting..."; do
  if grep -q "$broken" "$work/boot.log"; then echo "the firmware crashed: $broken" >&2; exit 1; fi
done
for line in "ESP-IDF v6.1 2nd stage bootloader" "esp_psram: SPI SRAM memory test OK" \
  "main_task: Calling app_main()" "device is not provisioned"; do
  grep -q "$line" "$work/boot.log" || { echo "the boot never said: $line" >&2; exit 1; }
done
echo "$target boots in QEMU to app_main and reads its configuration partition."
