#include "iterate/kit/platforms/provisioning.h"

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

/*
 * Serializes all writes and erases on `iterate_token` across `iterate-net`
 * (token refresh) and the app/portal task (mind update or full wipe). Once
 * `refresh_token_writes_disabled` is latched for a full wipe, no concurrent or
 * subsequent token refresh can commit a credential back into NVS before reboot.
 */
static portMUX_TYPE token_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t token_mutex_buf;
static SemaphoreHandle_t token_mutex;
static bool refresh_token_writes_disabled;

static SemaphoreHandle_t get_token_mutex(void) {
  SemaphoreHandle_t m = __atomic_load_n(&token_mutex, __ATOMIC_ACQUIRE);
  if (m != NULL) return m;
  portENTER_CRITICAL(&token_mutex_init_lock);
  m = __atomic_load_n(&token_mutex, __ATOMIC_RELAXED);
  if (m == NULL) {
    m = xSemaphoreCreateMutexStatic(&token_mutex_buf);
    __atomic_store_n(&token_mutex, m, __ATOMIC_RELEASE);
  }
  portEXIT_CRITICAL(&token_mutex_init_lock);
  return m;
}

void iterate_kit_platform_refresh_token_disable_writes(void) {
  __atomic_store_n(&refresh_token_writes_disabled, true, __ATOMIC_SEQ_CST);
}

static bool refresh_token_writes_off(void) {
  return __atomic_load_n(&refresh_token_writes_disabled, __ATOMIC_SEQ_CST);
}

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
  bool stored = false;
  if (configuration == NULL || token == NULL || token[0] == '\0') return false;
  if (refresh_token_writes_off()) return false;
  SemaphoreHandle_t lock = get_token_mutex();
  if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) return false;
  if (!refresh_token_writes_off() &&
      nvs_open(token_namespace, NVS_READWRITE, &handle) == ESP_OK) {
    seed_digest(configuration->oauth_refresh_token, digest);
    stored = nvs_set_str(handle, "seed", digest) == ESP_OK &&
        nvs_set_str(handle, "token", token) == ESP_OK &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
  }
  xSemaphoreGive(lock);
  return stored;
}

static bool refresh_token_clear_locked(void) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(token_namespace, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    const bool erased =
        nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!erased) return false;
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }

  /* Verify by read-back that no rotated token or seed digest remains in NVS. */
  err = nvs_open(token_namespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return true;
  if (err != ESP_OK) return false;
  size_t size = 0U;
  const esp_err_t tok_err = nvs_get_str(handle, "token", NULL, &size);
  size = 0U;
  const esp_err_t seed_err = nvs_get_str(handle, "seed", NULL, &size);
  nvs_close(handle);
  return tok_err == ESP_ERR_NVS_NOT_FOUND && seed_err == ESP_ERR_NVS_NOT_FOUND;
}

bool iterate_kit_platform_refresh_token_clear(void) {
  SemaphoreHandle_t lock = get_token_mutex();
  if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) return false;
  const bool ok = refresh_token_clear_locked();
  xSemaphoreGive(lock);
  return ok;
}

static bool verify_partition_erased(const esp_partition_t *partition) {
  uint8_t chunk[256];
  if (partition == NULL || partition->size == 0U) return false;
  for (size_t offset = 0U; offset < partition->size; offset += sizeof(chunk)) {
    const size_t remaining = partition->size - offset;
    const size_t len = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    if (esp_partition_read(partition, offset, chunk, len) != ESP_OK) {
      return false;
    }
    for (size_t i = 0U; i < len; ++i) {
      if (chunk[i] != 0xffU) return false;
    }
  }
  return true;
}

static bool clear_and_verify_nvs_volume(void) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("kit", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    const esp_err_t erase_err = nvs_erase_key(handle, "volume");
    bool ok = true;
    if (erase_err == ESP_OK) {
      ok = nvs_commit(handle) == ESP_OK;
    } else if (erase_err != ESP_ERR_NVS_NOT_FOUND) {
      ok = false;
    }
    nvs_close(handle);
    if (!ok) return false;
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }

  err = nvs_open("kit", NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return true;
  if (err != ESP_OK) return false;
  uint8_t kept = 0U;
  const esp_err_t get_err = nvs_get_u8(handle, "volume", &kept);
  nvs_close(handle);
  return get_err == ESP_ERR_NVS_NOT_FOUND;
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
  const esp_partition_t *partition = esp_partition_find_first(
      (esp_partition_type_t)ITERATE_KIT_PARTITION_TYPE,
      (esp_partition_subtype_t)ITERATE_KIT_PARTITION_SUBTYPE,
      iterate_kit_partition_label);
  const struct iterate_kit_platform_provisioning_result loaded =
      iterate_kit_platform_read_stored_provisioning(&cfg, &presence);
  if (loaded.status != ITERATE_KIT_PLATFORM_PROVISIONING_OK) {
    /* Fail closed on read errors without erasing so a valid mind pairing is kept. */
    if (loaded.status == ITERATE_KIT_PLATFORM_PROVISIONING_DECODE_FAILED &&
        loaded.configuration_error == ITERATE_KIT_CONFIGURATION_INVALID_MAGIC &&
        verify_partition_erased(partition)) {
      return true;
    }
    return false;
  }
  if (!presence.has_wifi) {
    memset(&cfg, 0, sizeof(cfg));
    return true;
  }
  iterate_kit_configuration_clear_wifi(&cfg, &presence);
  if (!presence.has_platform && !presence.has_status_voice) {
    memset(&cfg, 0, sizeof(cfg));
    return partition != NULL &&
        esp_partition_erase_range(partition, 0U, partition->size) == ESP_OK &&
        verify_partition_erased(partition);
  }
  const bool wrote = iterate_kit_platform_write_provisioning(&cfg, &presence);
  memset(&cfg, 0, sizeof(cfg));
  if (!wrote) return false;

  struct iterate_kit_configuration verify_cfg;
  struct iterate_kit_configuration_presence verify_presence;
  const struct iterate_kit_platform_provisioning_result readback =
      iterate_kit_platform_read_stored_provisioning(&verify_cfg, &verify_presence);
  const bool cleared = readback.status == ITERATE_KIT_PLATFORM_PROVISIONING_OK &&
      !verify_presence.has_wifi &&
      verify_cfg.wifi_ssid[0] == '\0' &&
      verify_cfg.wifi_password[0] == '\0';
  memset(&verify_cfg, 0, sizeof(verify_cfg));
  return cleared;
}

bool iterate_kit_platform_wipe_provisioning(void) {
  const esp_partition_t *partition = esp_partition_find_first(
      (esp_partition_type_t)ITERATE_KIT_PARTITION_TYPE,
      (esp_partition_subtype_t)ITERATE_KIT_PARTITION_SUBTYPE,
      iterate_kit_partition_label);
  iterate_kit_platform_refresh_token_disable_writes();
  const bool token_ok = iterate_kit_platform_refresh_token_clear();
  const bool volume_ok = clear_and_verify_nvs_volume();
  const bool partition_ok = partition != NULL &&
      esp_partition_erase_range(partition, 0U, partition->size) == ESP_OK &&
      verify_partition_erased(partition);
  return token_ok && volume_ok && partition_ok;
}
