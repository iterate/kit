#ifndef ITERATE_KIT_PLATFORMS_PROVISIONING_H
#define ITERATE_KIT_PLATFORMS_PROVISIONING_H

/*
 * Provisioning on a Mac: the same ITERKIT1 image a board carries in its
 * `iterate_kit` partition (scripts/config-image.ts), read from a file.
 * The Wi-Fi fields ride along unused; the OS origin, project id and key are
 * what the loop dials with.
 */

#include "iterate/kit/configuration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum iterate_kit_platform_provisioning_status {
  ITERATE_KIT_PLATFORM_PROVISIONING_OK = 0,
  ITERATE_KIT_PLATFORM_PROVISIONING_INVALID_ARGUMENT,
  ITERATE_KIT_PLATFORM_PROVISIONING_NO_PATH,
  ITERATE_KIT_PLATFORM_PROVISIONING_OPEN_FAILED,
  ITERATE_KIT_PLATFORM_PROVISIONING_READ_FAILED,
  ITERATE_KIT_PLATFORM_PROVISIONING_DECODE_FAILED,
};

struct iterate_kit_platform_provisioning_result {
  enum iterate_kit_platform_provisioning_status status;
  enum iterate_kit_configuration_error configuration_error;
  int32_t platform_error;
};

/** Where the image is; set once by main before the loop reads it. */
void iterate_kit_darwin_provisioning_set_path(const char *path);

struct iterate_kit_platform_provisioning_result
iterate_kit_platform_read_provisioning(
    struct iterate_kit_configuration *configuration);

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

#ifdef __cplusplus
}
#endif

#endif
