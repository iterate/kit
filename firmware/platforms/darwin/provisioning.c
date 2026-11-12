/* provisioning.c: the board's provisioning image, read from a file. */
#include "iterate/kit/platforms/provisioning.h"

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

enum { IMAGE_CAPACITY = 4096 };

static const char *image_path;

void iterate_kit_darwin_provisioning_set_path(const char *path) {
  image_path = path;
}

static struct iterate_kit_platform_provisioning_result result(
    enum iterate_kit_platform_provisioning_status status,
    enum iterate_kit_configuration_error configuration_error,
    int32_t platform_error) {
  const struct iterate_kit_platform_provisioning_result value = {
    .status = status,
    .configuration_error = configuration_error,
    .platform_error = platform_error,
  };
  return value;
}

struct iterate_kit_platform_provisioning_result
iterate_kit_platform_read_provisioning(
    struct iterate_kit_configuration *configuration) {
  static uint8_t image[IMAGE_CAPACITY];
  FILE *file;
  size_t size;
  enum iterate_kit_configuration_error decode_error;
  if (configuration == NULL) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_INVALID_ARGUMENT,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT, 0);
  }
  /* Cleared before any failure, so nothing continues on a previous read. */
  memset(configuration, 0, sizeof(*configuration));
  if (image_path == NULL || image_path[0] == '\0') {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_NO_PATH,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT, 0);
  }
  file = fopen(image_path, "rb");
  if (file == NULL) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_OPEN_FAILED,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT, (int32_t)errno);
  }
  size = fread(image, 1U, sizeof(image), file);
  if (ferror(file) != 0) {
    const int error = errno;
    (void)fclose(file);
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_READ_FAILED,
        ITERATE_KIT_CONFIGURATION_TRUNCATED, (int32_t)error);
  }
  (void)fclose(file);
  decode_error = iterate_kit_configuration_decode(configuration, image, size);
  memset(image, 0, sizeof(image)); /* the key never lingers in a second buffer */
  if (decode_error != ITERATE_KIT_CONFIGURATION_OK) {
    return result(ITERATE_KIT_PLATFORM_PROVISIONING_DECODE_FAILED, decode_error, 0);
  }
  return result(ITERATE_KIT_PLATFORM_PROVISIONING_OK, ITERATE_KIT_CONFIGURATION_OK, 0);
}

const char *iterate_kit_platform_provisioning_status_name(
    enum iterate_kit_platform_provisioning_status status) {
  switch (status) {
    case ITERATE_KIT_PLATFORM_PROVISIONING_OK: return "ok";
    case ITERATE_KIT_PLATFORM_PROVISIONING_INVALID_ARGUMENT: return "invalid-argument";
    case ITERATE_KIT_PLATFORM_PROVISIONING_NO_PATH: return "no --config path";
    case ITERATE_KIT_PLATFORM_PROVISIONING_OPEN_FAILED: return "open-failed";
    case ITERATE_KIT_PLATFORM_PROVISIONING_READ_FAILED: return "read-failed";
    case ITERATE_KIT_PLATFORM_PROVISIONING_DECODE_FAILED: return "decode-failed";
  }
  return "unknown";
}

/*
 * THE NEWEST REFRESH TOKEN, IN A FILE BESIDE THE IMAGE (`<--config>.token`):
 * one line with a digest of the image's own token, one with the newest. The
 * image stays what Kit's encoder wrote.
 */
static void seed_digest(const char *seed, char digest[9]) {
  uint32_t hash = UINT32_C(2166136261);
  for (const unsigned char *cursor = (const unsigned char *)seed; *cursor != '\0'; ++cursor) {
    hash ^= *cursor;
    hash *= UINT32_C(16777619);
  }
  (void)snprintf(digest, 9, "%08lx", (unsigned long)hash);
}

static bool token_path(char *path, size_t capacity) {
  if (image_path == NULL || image_path[0] == '\0') return false;
  const int written = snprintf(path, capacity, "%s.token", image_path);
  return written > 0 && (size_t)written < capacity;
}

void iterate_kit_platform_refresh_token_load(
    const struct iterate_kit_configuration *configuration,
    char *destination,
    size_t capacity) {
  char path[1024];
  char line[ITERATE_KIT_OAUTH_TOKEN_CAPACITY + 2];
  char expected[9];
  FILE *file;
  if (destination == NULL || capacity == 0U) return;
  destination[0] = '\0';
  if (configuration == NULL) return;
  (void)snprintf(destination, capacity, "%s", configuration->oauth_refresh_token);
  if (!token_path(path, sizeof(path))) return;
  file = fopen(path, "r");
  if (file == NULL) return;
  seed_digest(configuration->oauth_refresh_token, expected);
  if (fgets(line, sizeof(line), file) != NULL && strncmp(line, expected, 8U) == 0 &&
      fgets(line, sizeof(line), file) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] != '\0') (void)snprintf(destination, capacity, "%s", line);
  }
  memset(line, 0, sizeof(line));
  (void)fclose(file);
}

bool iterate_kit_platform_refresh_token_store(
    const struct iterate_kit_configuration *configuration,
    const char *token) {
  char path[1024];
  char temporary[1040];
  char digest[9];
  FILE *file;
  bool written;
  if (configuration == NULL || token == NULL || token[0] == '\0' ||
      !token_path(path, sizeof(path)))
    return false;
  (void)snprintf(temporary, sizeof(temporary), "%s.new", path);
  seed_digest(configuration->oauth_refresh_token, digest);
  file = fopen(temporary, "w");
  if (file == NULL) return false;
  (void)chmod(temporary, 0600);
  written = fprintf(file, "%s\n%s\n", digest, token) > 0;
  written = fclose(file) == 0 && written;
  return written && rename(temporary, path) == 0;
}
