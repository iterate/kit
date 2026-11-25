#include "iterate/kit/platforms/provisioning.h"
#include "iterate/kit/provisioning_state.h"

#include "esp_partition.h"
#include "esp_random.h"
#include "nvs.h"

#include <stdio.h>

#include <stddef.h>
#include <string.h>

/*
 * ESP-IDF binding for the provisioning image written by the shared CLI/web
 * flasher. Configuration is intentionally a raw custom partition rather than
 * NVS keys or a filesystem: both flashing surfaces can install one
 * versioned/CRC-protected blob, firmware can validate it with the same portable
 * decoder, and boot needs no per-field heap allocations.
 *
 * Mapping avoids a partition-sized RAM copy. The portable decoder copies only
 * validated fields into fixed-capacity caller storage, after which the mapping
 * is immediately released. Every failure leaves the destination zeroed because
 * stale Wi-Fi credentials or a stale project secret are more dangerous than a
 * loud, classifiable "needs provisioning" boot.
 */
enum {
  /*
   * Type, subtype, and label are a wire/flash ABI shared with the partition
   * table and flashing library. They are kept here explicitly instead of using
   * an application partition enum that might change their numeric identity.
   */
  ITERATE_KIT_PARTITION_TYPE = 0x40,
  ITERATE_KIT_PARTITION_SUBTYPE = 0x00,
};

static const char iterate_kit_partition_label[] = "iterate_kit";

static struct iterate_kit_platform_provisioning_result result(
    enum iterate_kit_platform_provisioning_status status,
    enum iterate_kit_configuration_error configuration_error,
    int32_t platform_error) {
  const struct iterate_kit_platform_provisioning_result value = {
    status,
    configuration_error,
    platform_error,
  };
  return value;
}

struct iterate_kit_platform_provisioning_result
iterate_kit_platform_read_provisioning(
    struct iterate_kit_configuration *configuration) {
  const esp_partition_t *partition;
  const void *mapped_image = NULL;
  esp_partition_mmap_handle_t mapping = 0U;
  esp_err_t map_error;
  enum iterate_kit_configuration_error decode_error;

  if (configuration == NULL) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_INVALID_ARGUMENT,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT,
        0);
  }
  /*
   * Clear before partition discovery, not merely before decode. That makes all
   * platform failures obey the same secret-erasure postcondition and prevents
   * callers from accidentally continuing with a previous successful read.
   */
  memset(configuration, 0, sizeof(*configuration));
  partition = esp_partition_find_first(
      (esp_partition_type_t)ITERATE_KIT_PARTITION_TYPE,
      (esp_partition_subtype_t)ITERATE_KIT_PARTITION_SUBTYPE,
      iterate_kit_partition_label);
  if (partition == NULL) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_PARTITION_NOT_FOUND,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT,
        0);
  }
  if (partition->size < ITERATE_KIT_CONFIGURATION_HEADER_SIZE) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_PARTITION_TOO_SMALL,
        ITERATE_KIT_CONFIGURATION_TRUNCATED,
        0);
  }

  map_error = esp_partition_mmap(
      partition,
      0U,
      partition->size,
      ESP_PARTITION_MMAP_DATA,
      &mapped_image,
      &mapping);
  if (map_error != ESP_OK || mapped_image == NULL) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_MMAP_FAILED,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT,
        (int32_t)map_error);
  }

  /*
   * The mapping covers the fixed partition because the encoded image carries
   * its own length and CRC. The decoder never treats erased trailing flash as a
   * field. This saves a preliminary copy/read while retaining strict bounds
   * checking in one portable implementation.
   */
  decode_error = iterate_kit_configuration_decode(
      configuration,
      mapped_image,
      partition->size);
  esp_partition_munmap(mapping);
  if (decode_error != ITERATE_KIT_CONFIGURATION_OK) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_DECODE_FAILED,
        decode_error,
        0);
  }
  return result(
      ITERATE_KIT_PLATFORM_PROVISIONING_OK,
      ITERATE_KIT_CONFIGURATION_OK,
      0);
}

const char *iterate_kit_platform_provisioning_status_name(
    enum iterate_kit_platform_provisioning_status status) {
  switch (status) {
    case ITERATE_KIT_PLATFORM_PROVISIONING_OK:
      return "ok";
    case ITERATE_KIT_PLATFORM_PROVISIONING_INVALID_ARGUMENT:
      return "invalid argument";
    case ITERATE_KIT_PLATFORM_PROVISIONING_PARTITION_NOT_FOUND:
      return "partition not found";
    case ITERATE_KIT_PLATFORM_PROVISIONING_PARTITION_TOO_SMALL:
      return "partition too small";
    case ITERATE_KIT_PLATFORM_PROVISIONING_MMAP_FAILED:
      return "flash map failed";
    case ITERATE_KIT_PLATFORM_PROVISIONING_DECODE_FAILED:
      return "decode failed";
  }
  return "unknown ESP configuration status";
}

/*
 * THE NEWEST REFRESH TOKEN, IN NVS. The configuration partition stays the
 * image Kit wrote; NVS (which board.c already opens for the volume) keeps what
 * the issuer rotated it into, beside a digest of the image's token it grew
 * from. Writes come once a refresh, about once an hour while the board is on:
 * NVS spreads them over its pages.
 */
static const char token_namespace[] = "iterate_token";

static void seed_digest(const char *seed, char digest[9]) {
  uint32_t hash = UINT32_C(2166136261);
  for (const unsigned char *cursor = (const unsigned char *)seed; *cursor != '\0'; ++cursor) {
    hash ^= *cursor;
    hash *= UINT32_C(16777619);
  }
  (void)snprintf(digest, 9, "%08lx", (unsigned long)hash);
}

void iterate_kit_platform_refresh_token_load(
    const struct iterate_kit_configuration *configuration,
    char *destination,
    size_t capacity) {
  nvs_handle_t handle;
  char expected[9];
  char kept[9] = {0};
  size_t size = sizeof(kept);
  if (destination == NULL || capacity == 0U) return;
  destination[0] = '\0';
  if (configuration == NULL) return;
  (void)snprintf(destination, capacity, "%s", configuration->oauth_refresh_token);
  if (nvs_open(token_namespace, NVS_READONLY, &handle) != ESP_OK) return;
  seed_digest(configuration->oauth_refresh_token, expected);
  if (nvs_get_str(handle, "seed", kept, &size) == ESP_OK && strcmp(kept, expected) == 0) {
    size = capacity;
    if (nvs_get_str(handle, "token", destination, &size) != ESP_OK || destination[0] == '\0')
      (void)snprintf(destination, capacity, "%s", configuration->oauth_refresh_token);
  }
  nvs_close(handle);
}

bool iterate_kit_platform_refresh_token_store(
    const struct iterate_kit_configuration *configuration,
    const char *token) {
  nvs_handle_t handle;
  char digest[9];
  bool stored;
  if (configuration == NULL || token == NULL || token[0] == '\0') return false;
  if (nvs_open(token_namespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  seed_digest(configuration->oauth_refresh_token, digest);
  stored = nvs_set_str(handle, "seed", digest) == ESP_OK &&
      nvs_set_str(handle, "token", token) == ESP_OK &&
      nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return stored;
}

void iterate_kit_platform_refresh_token_clear(void) {
  nvs_handle_t handle;
  if (nvs_open(token_namespace, NVS_READWRITE, &handle) != ESP_OK) return;
  (void)nvs_erase_all(handle);
  (void)nvs_commit(handle);
  nvs_close(handle);
}

struct iterate_kit_platform_provisioning_result
iterate_kit_platform_read_stored_provisioning(
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence) {
  const esp_partition_t *partition;
  const void *mapped_image = NULL;
  esp_partition_mmap_handle_t mapping = 0U;
  esp_err_t map_error;
  enum iterate_kit_configuration_error decode_error;

  if (presence != NULL) memset(presence, 0, sizeof(*presence));
  if (configuration == NULL || presence == NULL) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_INVALID_ARGUMENT,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT,
        0);
  }
  memset(configuration, 0, sizeof(*configuration));
  partition = esp_partition_find_first(
      (esp_partition_type_t)ITERATE_KIT_PARTITION_TYPE,
      (esp_partition_subtype_t)ITERATE_KIT_PARTITION_SUBTYPE,
      iterate_kit_partition_label);
  if (partition == NULL) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_PARTITION_NOT_FOUND,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT,
        0);
  }
  if (partition->size < ITERATE_KIT_CONFIGURATION_HEADER_SIZE) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_PARTITION_TOO_SMALL,
        ITERATE_KIT_CONFIGURATION_TRUNCATED,
        0);
  }
  map_error = esp_partition_mmap(
      partition, 0U, partition->size, ESP_PARTITION_MMAP_DATA, &mapped_image, &mapping);
  if (map_error != ESP_OK || mapped_image == NULL) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_MMAP_FAILED,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT,
        (int32_t)map_error);
  }
  decode_error = iterate_kit_configuration_decode_stored(
      configuration, presence, mapped_image, partition->size);
  esp_partition_munmap(mapping);
  if (decode_error != ITERATE_KIT_CONFIGURATION_OK) {
    return result(
        ITERATE_KIT_PLATFORM_PROVISIONING_DECODE_FAILED,
        decode_error,
        0);
  }
  return result(
      ITERATE_KIT_PLATFORM_PROVISIONING_OK,
      ITERATE_KIT_CONFIGURATION_OK,
      0);
}

bool iterate_kit_platform_write_provisioning(
    const struct iterate_kit_configuration *configuration,
    const struct iterate_kit_configuration_presence *presence) {
  static uint8_t image[4096];
  const esp_partition_t *partition;
  size_t written = 0U;
  struct iterate_kit_configuration verify_cfg;
  struct iterate_kit_configuration_presence verify_presence;
  if (configuration == NULL || presence == NULL) return false;
  partition = esp_partition_find_first(
      (esp_partition_type_t)ITERATE_KIT_PARTITION_TYPE,
      (esp_partition_subtype_t)ITERATE_KIT_PARTITION_SUBTYPE,
      iterate_kit_partition_label);
  if (partition == NULL || partition->size > sizeof(image) ||
      partition->size < ITERATE_KIT_CONFIGURATION_HEADER_SIZE) {
    return false;
  }
  if (iterate_kit_configuration_encode(
          configuration, presence, image, partition->size, &written) !=
      ITERATE_KIT_CONFIGURATION_OK) {
    memset(image, 0, sizeof(image));
    return false;
  }
  const bool ok =
      esp_partition_erase_range(partition, 0U, partition->size) == ESP_OK &&
      esp_partition_write(partition, 0U, image, written) == ESP_OK;
  memset(image, 0, sizeof(image));
  if (!ok) return false;
  const struct iterate_kit_platform_provisioning_result readback =
      iterate_kit_platform_read_stored_provisioning(&verify_cfg, &verify_presence);
  memset(&verify_cfg, 0, sizeof(verify_cfg));
  return readback.status == ITERATE_KIT_PLATFORM_PROVISIONING_OK &&
      verify_presence.has_wifi == presence->has_wifi &&
      verify_presence.has_platform == presence->has_platform;
}

bool iterate_kit_platform_clear_wifi_provisioning(void) {
  struct iterate_kit_configuration cfg;
  struct iterate_kit_configuration_presence presence;
  const struct iterate_kit_platform_provisioning_result loaded =
      iterate_kit_platform_read_stored_provisioning(&cfg, &presence);
  if (loaded.status != ITERATE_KIT_PLATFORM_PROVISIONING_OK) {
    /* Already unprovisioned or corrupt: erase only the partition. */
    const esp_partition_t *partition = esp_partition_find_first(
        (esp_partition_type_t)ITERATE_KIT_PARTITION_TYPE,
        (esp_partition_subtype_t)ITERATE_KIT_PARTITION_SUBTYPE,
        iterate_kit_partition_label);
    return partition != NULL &&
        esp_partition_erase_range(partition, 0U, partition->size) == ESP_OK;
  }
  iterate_kit_configuration_clear_wifi(&cfg, &presence);
  if (!presence.has_platform && !presence.has_status_voice) {
    const esp_partition_t *partition = esp_partition_find_first(
        (esp_partition_type_t)ITERATE_KIT_PARTITION_TYPE,
        (esp_partition_subtype_t)ITERATE_KIT_PARTITION_SUBTYPE,
        iterate_kit_partition_label);
    memset(&cfg, 0, sizeof(cfg));
    return partition != NULL &&
        esp_partition_erase_range(partition, 0U, partition->size) == ESP_OK;
  }
  const bool ok = iterate_kit_platform_write_provisioning(&cfg, &presence);
  memset(&cfg, 0, sizeof(cfg));
  return ok;
}

bool iterate_kit_platform_wipe_provisioning(void) {
  const esp_partition_t *partition = esp_partition_find_first(
      (esp_partition_type_t)ITERATE_KIT_PARTITION_TYPE,
      (esp_partition_subtype_t)ITERATE_KIT_PARTITION_SUBTYPE,
      iterate_kit_partition_label);
  nvs_handle_t handle;
  iterate_kit_platform_refresh_token_clear();
  if (nvs_open("kit", NVS_READWRITE, &handle) == ESP_OK) {
    (void)nvs_erase_key(handle, "volume");
    (void)nvs_commit(handle);
    nvs_close(handle);
  }
  return partition != NULL &&
      esp_partition_erase_range(partition, 0U, partition->size) == ESP_OK;
}

static const char setup_namespace[] = "kit_setup";

bool iterate_kit_platform_setup_code_load_or_create(
    char out[ITERATE_KIT_SETUP_CODE_CAPACITY]) {
  nvs_handle_t handle;
  char stored[ITERATE_KIT_SETUP_CODE_CAPACITY] = {0};
  size_t size = sizeof(stored);
  uint8_t entropy[ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES];
  if (out == NULL) return false;
  out[0] = '\0';
  if (nvs_open(setup_namespace, NVS_READONLY, &handle) == ESP_OK) {
    const esp_err_t err = nvs_get_str(handle, "code", stored, &size);
    nvs_close(handle);
    if (err == ESP_OK && iterate_kit_setup_code_valid(stored)) {
      memcpy(out, stored, ITERATE_KIT_SETUP_CODE_CAPACITY);
      memset(stored, 0, sizeof(stored));
      return true;
    }
  }
  /* Caller guarantees Wi-Fi RF is active so esp_fill_random uses HWRNG noise. */
  esp_fill_random(entropy, sizeof(entropy));
  iterate_kit_setup_code_format(entropy, out);
  memset(entropy, 0, sizeof(entropy));
  if (nvs_open(setup_namespace, NVS_READWRITE, &handle) == ESP_OK) {
    (void)nvs_set_str(handle, "code", out);
    (void)nvs_commit(handle);
    nvs_close(handle);
  }
  return iterate_kit_setup_code_valid(out);
}
