#ifndef ITERATE_KIT_PLATFORMS_PROVISIONING_H
#define ITERATE_KIT_PLATFORMS_PROVISIONING_H

#include "iterate/kit/configuration.h"

#include <stdbool.h>
#include <stddef.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Separates flash-layout failures from portable image-format failures.
 *
 * Provisioning tools need to distinguish "the custom partition was never
 * flashed" from "bytes were present but failed CRC/schema validation". Folding
 * both into a generic boot error would make field recovery guesswork and could
 * encourage retrying a permanently corrupt image forever.
 */
enum iterate_kit_platform_provisioning_status {
  ITERATE_KIT_PLATFORM_PROVISIONING_OK = 0,
  ITERATE_KIT_PLATFORM_PROVISIONING_INVALID_ARGUMENT,
  ITERATE_KIT_PLATFORM_PROVISIONING_PARTITION_NOT_FOUND,
  ITERATE_KIT_PLATFORM_PROVISIONING_PARTITION_TOO_SMALL,
  ITERATE_KIT_PLATFORM_PROVISIONING_MMAP_FAILED,
  ITERATE_KIT_PLATFORM_PROVISIONING_DECODE_FAILED,
};

/**
 * Complete, allocation-free provisioning diagnostic.
 *
 * configuration_error is meaningful for DECODE_FAILED (and describes the
 * portable validation class); platform_error carries the ESP-IDF error for
 * MMAP_FAILED. The explicit outer status tells callers which secondary field
 * is evidence rather than forcing them to interpret a coincidental zero.
 */
struct iterate_kit_platform_provisioning_result {
  enum iterate_kit_platform_provisioning_status status;
  enum iterate_kit_configuration_error configuration_error;
  int32_t platform_error;
};

/**
 * Finds the custom `iterate_kit` partition, maps it directly from flash,
 * decodes it into fixed-capacity caller storage, and immediately unmaps it.
 * No filesystem, heap allocation, or full-partition RAM copy is involved.
 * The output is cleared for every non-OK result, so a failed reprovision cannot
 * leave an earlier project's bearer secret live in caller-visible state.
 *
 * Call only during boot/provisioning setup. The returned configuration is a
 * caller-owned copy; it remains valid after the flash mapping is released.
 */
struct iterate_kit_platform_provisioning_result
iterate_kit_platform_read_provisioning(
    struct iterate_kit_configuration *configuration);

/** Stable human-readable classification for boot logs; never returns NULL. */
const char *iterate_kit_platform_provisioning_status_name(
    enum iterate_kit_platform_provisioning_status status);

/**
 * A MIND BOARD'S NEWEST REFRESH TOKEN (iterate/kit/token_refresh.h). Each
 * refresh rotates the token, so the board keeps the newer one where a restart
 * finds it. It is kept with a digest of the image's own token: a reflash with
 * a new image starts again from that image's token, never from a token of the
 * grant before.
 *
 * load copies the kept token for this image into `destination`, or the
 * image's own when none is kept. store keeps `token` for this image; false
 * when it could not (the board then refreshes with the older token, which the
 * issuer still takes once).
 */
void iterate_kit_platform_refresh_token_load(
    const struct iterate_kit_configuration *configuration,
    char *destination,
    size_t capacity);
bool iterate_kit_platform_refresh_token_store(
    const struct iterate_kit_configuration *configuration,
    const char *token);

/**
 * Permanently disable rotated refresh-token NVS writes (latched before a full
 * wipe so the background network task cannot commit a fresh token back to NVS
 * after the erase has been verified).
 */
void iterate_kit_platform_refresh_token_disable_writes(void);

bool iterate_kit_platform_refresh_token_writes_disabled(void);

/**
 * Erase any stored rotated refresh token from NVS (on mind change or full wipe)
 * and verify by read-back that both `token` and `seed` keys are absent.
 */
bool iterate_kit_platform_refresh_token_clear(void);

/**
 * Read the `iterate_kit` partition allowing partial configurations (Wi-Fi-only
 * or mind-only). If a previous Wi-Fi-only reset lost power after erasing
 * `iterate_kit`, transparently restores the verified pending NVS backup
 * (`kit_wifi_rst`) back into `iterate_kit` and clears the backup. A valid
 * `iterate_kit` partition always wins and clears any stale backup so a fresh
 * Kit Flasher USB flash is never overwritten. An erased partition (all 0xFF)
 * without a pending backup is reported as DECODE_FAILED / INVALID_MAGIC with
 * `presence` cleared, selecting Setup AP mode.
 */
struct iterate_kit_platform_provisioning_result
iterate_kit_platform_read_stored_provisioning(
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence);

/**
 * Encode `configuration` (for groups enabled in `presence`) as an ITERKIT1
 * image, erase and write the `iterate_kit` partition, verify read-back, and
 * clear any stale Wi-Fi-reset NVS backup.
 */
bool iterate_kit_platform_write_provisioning(
    const struct iterate_kit_configuration *configuration,
    const struct iterate_kit_configuration_presence *presence);

/**
 * Power-loss-safe Wi-Fi reset: clear ONLY the stored Wi-Fi SSID and password
 * while preserving any saved mind/project credential, rotated refresh token,
 * and status voice. Fails closed without erasing the partition if the current
 * configuration cannot be read or if staging a verified pending copy of the
 * retained image in NVS (`kit_wifi_rst`) fails before the sector erase. Clears
 * and verifies the NVS backup only after the rewritten `iterate_kit` partition
 * reads back Wi-Fi-free.
 */
bool iterate_kit_platform_clear_wifi_provisioning(void);

/**
 * Full wipe: permanently disables refresh-token NVS writes (serializing with
 * any in-flight `itx_transport` token commit), erases the `iterate_kit`
 * partition, drops any rotated refresh token in NVS (`iterate_token`), any
 * pending Wi-Fi-reset backup (`kit_wifi_rst`), and kept speaker volume, and
 * verifies all four by read-back. Preserves the per-device setup code in NVS
 * so the physical QR/setup card stays valid.
 */
bool iterate_kit_platform_wipe_provisioning(void);

/**
 * Load the per-device 14-char setup code (`xxxx-xxxx-xxxx`, 60 bits) from NVS
 * (`kit_setup`), or generate one with `esp_fill_random()` on first boot,
 * commit it to NVS, and verify it by reading it back from NVS before use.
 * Returns false and zeroes `out` if persistence or read-back fails.
 *
 * PRECONDITION: Wi-Fi/BT RF must already be started (or `bootloader_random`
 * enabled) before calling this so `esp_fill_random()` draws from the hardware
 * RNG noise source rather than an unseeded PRNG.
 */
bool iterate_kit_platform_setup_code_load_or_create(char out[15]);

#ifdef __cplusplus
}
#endif

#endif
