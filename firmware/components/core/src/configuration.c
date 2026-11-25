#include "iterate/kit/configuration.h"

#include <stdbool.h>
#include <string.h>

/*
 * Kit's one encoder (src/firmware/config-image.ts) writes this small versioned
 * TLV image, from the flashing page and from its CLI. Firmware treats flash as
 * untrusted: interrupted writes, stale tools, and accidental
 * corruption must yield one classified boot error, never a partially usable
 * mixture of credentials. Decoding is allocation-free and transactional from
 * the caller's perspective: every failure clears the entire destination.
 *
 * CRC32 detects transfer/storage corruption; it is not a MAC and does not make
 * the key safe against someone who can read or rewrite flash. On the old
 * platform the key field carries a project-scoped personal access token minted
 * by `grants.mint` for the person who set the board up (itx_mount.h); on a
 * mind, a refresh token of that person's grant (configuration.h).
 */
enum {
  CONFIGURATION_FIELD_HEADER_SIZE = 3,
  CONFIGURATION_FIELD_WIFI_SSID = 1,
  CONFIGURATION_FIELD_WIFI_PASSWORD = 2,
  CONFIGURATION_FIELD_OS_BASE_URL = 3,
  CONFIGURATION_FIELD_PROJECT_ID = 4,
  CONFIGURATION_FIELD_PROJECT_API_KEY = 5,
  CONFIGURATION_FIELD_STATUS_VOICE = 6,
  CONFIGURATION_FIELD_OAUTH_CLIENT_ID = 7,
  CONFIGURATION_FIELD_OAUTH_REFRESH_TOKEN = 8,
};

/* Indexed by enum iterate_kit_status_voice; Kit's encoder (src/firmware/config-image.ts) writes these. */
static const char *const status_voice_names[] = {
  "greensleeves", "daisy-bell", "auld-lang-syne", "lass-of-aughrim", "spoken", "off",
};

const char *iterate_kit_status_voice_name(enum iterate_kit_status_voice voice) {
  return (size_t)voice < sizeof(status_voice_names) / sizeof(status_voice_names[0])
             ? status_voice_names[voice]
             : "unknown";
}

/* A name this firmware does not know is the default, not an error (see the enum). */
static uint8_t decode_status_voice(const uint8_t *value, size_t length) {
  for (size_t i = 0; i < sizeof(status_voice_names) / sizeof(status_voice_names[0]); i++) {
    if (strlen(status_voice_names[i]) == length && memcmp(status_voice_names[i], value, length) == 0) {
      return (uint8_t)i;
    }
  }
  return ITERATE_KIT_STATUS_VOICE_GREENSLEEVES;
}

static const uint8_t configuration_magic_prefix[] = {
  'I', 'T', 'E', 'R', 'K', 'I', 'T',
};

static uint32_t read_u32_le(const uint8_t *bytes) {
  return (uint32_t)bytes[0] |
      ((uint32_t)bytes[1] << 8U) |
      ((uint32_t)bytes[2] << 16U) |
      ((uint32_t)bytes[3] << 24U);
}

static uint16_t read_u16_le(const uint8_t *bytes) {
  return (uint16_t)((uint16_t)bytes[0] |
      ((uint16_t)bytes[1] << 8U));
}

static void write_u32_le(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)(value & 0xffU);
  bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
  bytes[2] = (uint8_t)((value >> 16U) & 0xffU);
  bytes[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static void write_u16_le(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value & 0xffU);
  bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
}

static uint32_t configuration_crc32(
    const uint8_t *bytes, size_t length) {
  /*
   * Use the standard reflected CRC-32 polynomial explicitly rather than
   * depending on a platform peripheral/library. This keeps the image format
   * byte-for-byte identical in browser/CLI host tests and on every ESP target;
   * configuration images are tiny enough that a table would waste flash.
   */
  uint32_t crc = UINT32_C(0xffffffff);
  size_t index;
  for (index = 0U; index < length; ++index) {
    unsigned int bit;
    crc ^= bytes[index];
    for (bit = 0U; bit < 8U; ++bit) {
      const uint32_t least_significant_bit = crc & UINT32_C(1);
      crc = (crc >> 1U) ^
          (least_significant_bit != 0U
              ? UINT32_C(0xedb88320)
              : UINT32_C(0));
    }
  }
  return crc ^ UINT32_C(0xffffffff);
}

static enum iterate_kit_configuration_error fail(
    struct iterate_kit_configuration *configuration,
    enum iterate_kit_configuration_error error) {
  if (configuration != NULL) {
    /*
     * Credentials are an all-or-nothing set. Leaving fields decoded before a
     * bad trailing TLV would invite callers to accidentally connect with a
     * stale/partial identity and makes postmortems depend on parser position.
     */
    memset(configuration, 0, sizeof(*configuration));
  }
  return error;
}

static enum iterate_kit_configuration_error copy_field(
    char *destination,
    size_t capacity,
    const uint8_t *value,
    size_t length,
    bool allow_empty) {
  if ((!allow_empty && length == 0U) ||
      memchr(value, '\0', length) != NULL) {
    /*
     * Embedded NULs would make validation and later C-string consumers see
     * different values. Open Wi-Fi is the sole deliberately empty known field;
     * every identity/endpoint must remain non-empty.
     */
    return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
  }
  if (length >= capacity) {
    return ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG;
  }
  memcpy(destination, value, length);
  destination[length] = '\0';
  return ITERATE_KIT_CONFIGURATION_OK;
}

static bool valid_base_url(const char *value) {
  const char *host;
  const char *cursor;
  if (strncmp(value, "https://", sizeof("https://") - 1U) == 0) {
    host = value + sizeof("https://") - 1U;
  } else if (strncmp(
                 value, "http://", sizeof("http://") - 1U) == 0) {
    host = value + sizeof("http://") - 1U;
  } else {
    return false;
  }
  if (*host == '\0') {
    return false;
  }
  for (cursor = host; *cursor != '\0'; ++cursor) {
    const unsigned char character = (unsigned char)*cursor;
    if (character <= 0x20U ||
        character >= 0x7fU ||
        *cursor == '/' ||
        *cursor == '\\' ||
        *cursor == '@' ||
        *cursor == '?' ||
        *cursor == '#') {
      /*
       * Store only an origin-like base. Allowing paths, userinfo, fragments, or
       * escaped control characters would make the fixed `/api` endpoint
       * construction ambiguous and could redirect credentials unexpectedly.
       */
      return false;
    }
  }
  return true;
}

/*
 * The field holds the project's id (`prj_<hex>`, as the Kit firmware page
 * writes it), and `projects.get` also accepts the project's slug, so this
 * mirrors the server's one-path-segment rule `/^[A-Za-z0-9_-]+$/` rather than
 * insisting on a `prj_` prefix: a board that refuses its own blob at boot
 * never dials and looks dead.
 */
static bool valid_project_id(const char *value) {
  const char *cursor;
  if (value[0] == '\0') {
    return false;
  }
  for (cursor = value; *cursor != '\0'; ++cursor) {
    const bool alpha_numeric =
        (*cursor >= 'a' && *cursor <= 'z') ||
        (*cursor >= 'A' && *cursor <= 'Z') ||
        (*cursor >= '0' && *cursor <= '9');
    if (!alpha_numeric && *cursor != '_' && *cursor != '-') {
      return false;
    }
  }
  return true;
}

static enum iterate_kit_configuration_error build_websocket_url(
    const char *os_base_url,
    const char *suffix,
    char *destination,
    size_t destination_capacity) {
  static const char secure_http_prefix[] = "https://";
  static const char insecure_http_prefix[] = "http://";
  static const char secure_websocket_prefix[] = "wss://";
  static const char insecure_websocket_prefix[] = "ws://";
  const char *host;
  const char *websocket_prefix;
  size_t websocket_prefix_size;
  size_t host_size;
  size_t suffix_size;
  size_t required_size;

  if (destination != NULL && destination_capacity > 0U) {
    /*
     * Clearing first is a deliberate failure contract: callers can log/use the
     * error without also remembering whether a previous successful URL remains
     * in this reusable boot buffer.
     */
    destination[0] = '\0';
  }
  if (os_base_url == NULL ||
      suffix == NULL ||
      destination == NULL ||
      destination_capacity == 0U ||
      os_base_url == destination) {
    return ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT;
  }
  if (!valid_base_url(os_base_url)) {
    return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
  }
  if (strncmp(
          os_base_url,
          secure_http_prefix,
          sizeof(secure_http_prefix) - 1U) == 0) {
    host = os_base_url + sizeof(secure_http_prefix) - 1U;
    websocket_prefix = secure_websocket_prefix;
    websocket_prefix_size =
        sizeof(secure_websocket_prefix) - 1U;
  } else {
    host = os_base_url + sizeof(insecure_http_prefix) - 1U;
    websocket_prefix = insecure_websocket_prefix;
    websocket_prefix_size =
        sizeof(insecure_websocket_prefix) - 1U;
  }
  host_size = strlen(host);
  suffix_size = strlen(suffix);
  if (host_size >
      SIZE_MAX - websocket_prefix_size - suffix_size - 1U) {
    /* Preserve the bound even on hosts where size_t is narrower/unusual. */
    return ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG;
  }
  required_size =
      websocket_prefix_size + host_size + suffix_size + 1U;
  if (required_size > destination_capacity) {
    return ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG;
  }

  memcpy(
      destination, websocket_prefix, websocket_prefix_size);
  memcpy(
      destination + websocket_prefix_size, host, host_size);
  memcpy(
      destination + websocket_prefix_size + host_size,
      suffix,
      suffix_size + 1U);
  return ITERATE_KIT_CONFIGURATION_OK;
}

enum iterate_kit_configuration_error
iterate_kit_configuration_build_itx_websocket_url(
    const char *os_base_url,
    char *destination,
    size_t destination_capacity) {
  return build_websocket_url(os_base_url, "/api", destination, destination_capacity);
}

static enum iterate_kit_configuration_error decode_internal(
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence,
    const uint8_t *image,
    size_t image_size,
    bool require_complete) {
  uint32_t expected_crc;
  uint32_t payload_size_u32;
  const uint8_t *payload;
  size_t payload_size;
  size_t offset = 0U;
  uint32_t seen_fields = 0U;
  const uint32_t wifi_fields =
      (UINT32_C(1) << CONFIGURATION_FIELD_WIFI_SSID) |
      (UINT32_C(1) << CONFIGURATION_FIELD_WIFI_PASSWORD);
  const uint32_t project_fields =
      (UINT32_C(1) << CONFIGURATION_FIELD_PROJECT_ID) |
      (UINT32_C(1) << CONFIGURATION_FIELD_PROJECT_API_KEY);
  const uint32_t mind_fields =
      (UINT32_C(1) << CONFIGURATION_FIELD_OAUTH_CLIENT_ID) |
      (UINT32_C(1) << CONFIGURATION_FIELD_OAUTH_REFRESH_TOKEN);
  const uint32_t any_platform_fields =
      (UINT32_C(1) << CONFIGURATION_FIELD_OS_BASE_URL) |
      project_fields |
      mind_fields;

  if (presence != NULL) {
    memset(presence, 0, sizeof(*presence));
  }
  if (configuration == NULL || image == NULL) {
    return fail(
        configuration,
        ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT);
  }
  memset(configuration, 0, sizeof(*configuration));
  if (image_size < ITERATE_KIT_CONFIGURATION_HEADER_SIZE) {
    return fail(
        configuration, ITERATE_KIT_CONFIGURATION_TRUNCATED);
  }
  if (memcmp(
          image,
          configuration_magic_prefix,
          sizeof(configuration_magic_prefix)) != 0) {
    return fail(
        configuration, ITERATE_KIT_CONFIGURATION_INVALID_MAGIC);
  }
  if (image[sizeof(configuration_magic_prefix)] != '1') {
    return fail(
        configuration,
        ITERATE_KIT_CONFIGURATION_UNSUPPORTED_VERSION);
  }

  payload_size_u32 = read_u32_le(image + 8U);
  payload_size = (size_t)payload_size_u32;
  if (payload_size >
      image_size - ITERATE_KIT_CONFIGURATION_HEADER_SIZE) {
    return fail(
        configuration, ITERATE_KIT_CONFIGURATION_TRUNCATED);
  }
  payload = image + ITERATE_KIT_CONFIGURATION_HEADER_SIZE;
  /*
   * Verify the declared payload before interpreting a single field. That
   * ordering ensures random corruption cannot steer parser branches or leave
   * briefly accepted credentials, and permits erased partition padding after
   * the payload without including it in integrity checks.
   */
  expected_crc = read_u32_le(image + 12U);
  if (configuration_crc32(payload, payload_size) != expected_crc) {
    return fail(
        configuration,
        ITERATE_KIT_CONFIGURATION_CHECKSUM_MISMATCH);
  }

  while (offset < payload_size) {
    uint8_t field;
    uint16_t field_size_u16;
    size_t field_size;
    const uint8_t *value;
    char *destination = NULL;
    size_t capacity = 0U;
    bool allow_empty = false;
    enum iterate_kit_configuration_error copy_error;

    if (payload_size - offset < CONFIGURATION_FIELD_HEADER_SIZE) {
      return fail(
          configuration,
          ITERATE_KIT_CONFIGURATION_MALFORMED_FIELD);
    }
    field = payload[offset];
    field_size_u16 = read_u16_le(payload + offset + 1U);
    field_size = (size_t)field_size_u16;
    offset += CONFIGURATION_FIELD_HEADER_SIZE;
    if (field_size > payload_size - offset) {
      return fail(
          configuration,
          ITERATE_KIT_CONFIGURATION_MALFORMED_FIELD);
    }
    value = payload + offset;
    offset += field_size;

    switch (field) {
      case CONFIGURATION_FIELD_WIFI_SSID:
        destination = configuration->wifi_ssid;
        capacity = sizeof(configuration->wifi_ssid);
        break;
      case CONFIGURATION_FIELD_WIFI_PASSWORD:
        destination = configuration->wifi_password;
        capacity = sizeof(configuration->wifi_password);
        allow_empty = true;
        break;
      case CONFIGURATION_FIELD_OS_BASE_URL:
        destination = configuration->os_base_url;
        capacity = sizeof(configuration->os_base_url);
        break;
      case CONFIGURATION_FIELD_PROJECT_ID:
        destination = configuration->project_id;
        capacity = sizeof(configuration->project_id);
        break;
      case CONFIGURATION_FIELD_PROJECT_API_KEY:
        destination = configuration->project_api_key;
        capacity = sizeof(configuration->project_api_key);
        break;
      case CONFIGURATION_FIELD_OAUTH_CLIENT_ID:
        destination = configuration->oauth_client_id;
        capacity = sizeof(configuration->oauth_client_id);
        break;
      case CONFIGURATION_FIELD_OAUTH_REFRESH_TOKEN:
        destination = configuration->oauth_refresh_token;
        capacity = sizeof(configuration->oauth_refresh_token);
        break;
      case CONFIGURATION_FIELD_STATUS_VOICE:
        break;
      default:
        /*
         * Unknown tags are length-delimited and integrity-protected, so older
         * firmware can safely ignore fields introduced by newer flash tools.
         * Known duplicates remain errors because "first wins" versus "last
         * wins" would create inconsistent credential interpretation.
         */
        continue;
    }

    if ((seen_fields & (UINT32_C(1) << field)) != 0U) {
      return fail(
          configuration,
          ITERATE_KIT_CONFIGURATION_DUPLICATE_FIELD);
    }
    seen_fields |= UINT32_C(1) << field;
    if (field == CONFIGURATION_FIELD_STATUS_VOICE) {
      configuration->status_voice = decode_status_voice(value, field_size);
      continue;
    }
    copy_error = copy_field(
        destination, capacity, value, field_size, allow_empty);
    if (copy_error != ITERATE_KIT_CONFIGURATION_OK) {
      return fail(configuration, copy_error);
    }
  }

  const bool has_ssid =
      (seen_fields & (UINT32_C(1) << CONFIGURATION_FIELD_WIFI_SSID)) != 0U;
  const bool has_wifi_all = (seen_fields & wifi_fields) == wifi_fields;
  if (require_complete) {
    if (!has_wifi_all ||
        (seen_fields & (UINT32_C(1) << CONFIGURATION_FIELD_OS_BASE_URL)) == 0U) {
      return fail(configuration, ITERATE_KIT_CONFIGURATION_MISSING_FIELD);
    }
  } else {
    if (!has_ssid &&
        (seen_fields & (UINT32_C(1) << CONFIGURATION_FIELD_WIFI_PASSWORD)) != 0U &&
        configuration->wifi_password[0] != '\0') {
      return fail(configuration, ITERATE_KIT_CONFIGURATION_MISSING_FIELD);
    }
  }

  /*
   * ONE PLATFORM, WHOLE. A half set is a missing field; both sets would leave
   * the board to guess which credential the image meant, so that is refused.
   */
  const bool any_platform = (seen_fields & any_platform_fields) != 0U;
  if (any_platform || require_complete) {
    if ((seen_fields & (UINT32_C(1) << CONFIGURATION_FIELD_OS_BASE_URL)) == 0U) {
      return fail(configuration, ITERATE_KIT_CONFIGURATION_MISSING_FIELD);
    }
    if ((seen_fields & project_fields) != 0U &&
        (seen_fields & mind_fields) != 0U) {
      return fail(
          configuration,
          ITERATE_KIT_CONFIGURATION_INVALID_VALUE);
    }
    if ((seen_fields & mind_fields) == mind_fields) {
      configuration->platform = ITERATE_KIT_CONFIGURATION_PLATFORM_MIND;
    } else if ((seen_fields & project_fields) == project_fields) {
      configuration->platform = ITERATE_KIT_CONFIGURATION_PLATFORM_PROJECT;
    } else {
      return fail(
          configuration,
          ITERATE_KIT_CONFIGURATION_MISSING_FIELD);
    }
    if (!valid_base_url(configuration->os_base_url) ||
        (configuration->platform ==
             ITERATE_KIT_CONFIGURATION_PLATFORM_PROJECT &&
         !valid_project_id(configuration->project_id))) {
      return fail(
          configuration,
          ITERATE_KIT_CONFIGURATION_INVALID_VALUE);
    }
  }

  if (presence != NULL) {
    presence->has_wifi = has_ssid;
    presence->has_platform = any_platform;
    presence->has_status_voice =
        (seen_fields & (UINT32_C(1) << CONFIGURATION_FIELD_STATUS_VOICE)) != 0U;
  }
  return ITERATE_KIT_CONFIGURATION_OK;
}

enum iterate_kit_configuration_error iterate_kit_configuration_decode(
    struct iterate_kit_configuration *configuration,
    const uint8_t *image,
    size_t image_size) {
  return decode_internal(configuration, NULL, image, image_size, true);
}

enum iterate_kit_configuration_error iterate_kit_configuration_decode_stored(
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence,
    const uint8_t *image,
    size_t image_size) {
  if (presence == NULL) {
    return fail(configuration, ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT);
  }
  return decode_internal(configuration, presence, image, image_size, false);
}

bool iterate_kit_configuration_valid_wifi_password(const char *password) {
  size_t length;
  if (password == NULL) return false;
  length = strlen(password);
  if (length == 0U || (length >= 8U && length <= 63U)) return true;
  if (length != 64U) return false;
  for (size_t index = 0U; index < length; ++index) {
    const char c = password[index];
    const bool hex =
        (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

bool iterate_kit_configuration_parse_status_voice(
    const char *name, enum iterate_kit_status_voice *voice) {
  if (name == NULL || voice == NULL) return false;
  for (size_t index = 0U;
       index < sizeof(status_voice_names) / sizeof(status_voice_names[0]);
       ++index) {
    if (strcmp(status_voice_names[index], name) == 0) {
      *voice = (enum iterate_kit_status_voice)index;
      return true;
    }
  }
  return false;
}

static bool valid_oauth_client_id(const char *value) {
  if (value == NULL) return false;
  if (strncmp(value, "https://", 8U) != 0 && strncmp(value, "http://", 7U) != 0) {
    return false;
  }
  const size_t length = strlen(value);
  if (length <= 8U) return false;
  for (size_t index = 0U; index < length; ++index) {
    const unsigned char c = (unsigned char)value[index];
    if (c <= 0x20U || c >= 0x7fU) return false;
  }
  return true;
}

static enum iterate_kit_configuration_error append_tlv(
    uint8_t *payload,
    size_t payload_capacity,
    size_t *offset,
    uint8_t tag,
    const char *value,
    size_t max_bytes,
    bool allow_empty) {
  const size_t length = strlen(value);
  if (!allow_empty && length == 0U) {
    return ITERATE_KIT_CONFIGURATION_MISSING_FIELD;
  }
  if (length > max_bytes) {
    return ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG;
  }
  if (CONFIGURATION_FIELD_HEADER_SIZE + length > payload_capacity - *offset) {
    return ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG;
  }
  payload[*offset] = tag;
  write_u16_le(payload + *offset + 1U, (uint16_t)length);
  if (length > 0U) {
    memcpy(payload + *offset + CONFIGURATION_FIELD_HEADER_SIZE, value, length);
  }
  *offset += CONFIGURATION_FIELD_HEADER_SIZE + length;
  return ITERATE_KIT_CONFIGURATION_OK;
}

enum iterate_kit_configuration_error iterate_kit_configuration_encode(
    const struct iterate_kit_configuration *configuration,
    const struct iterate_kit_configuration_presence *presence,
    uint8_t *image,
    size_t capacity,
    size_t *written_size) {
  uint8_t *payload;
  size_t payload_capacity;
  size_t payload_offset = 0U;
  enum iterate_kit_configuration_error error;

  if (written_size != NULL) *written_size = 0U;
  if (configuration == NULL || presence == NULL || image == NULL ||
      capacity <= ITERATE_KIT_CONFIGURATION_HEADER_SIZE) {
    return ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT;
  }

  payload = image + ITERATE_KIT_CONFIGURATION_HEADER_SIZE;
  payload_capacity = capacity - ITERATE_KIT_CONFIGURATION_HEADER_SIZE;

  if (presence->has_wifi) {
    if (!iterate_kit_configuration_valid_wifi_password(configuration->wifi_password)) {
      return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
    }
    error = append_tlv(
        payload, payload_capacity, &payload_offset,
        CONFIGURATION_FIELD_WIFI_SSID, configuration->wifi_ssid,
        ITERATE_KIT_WIFI_SSID_CAPACITY - 1U, false);
    if (error != ITERATE_KIT_CONFIGURATION_OK) return error;
    error = append_tlv(
        payload, payload_capacity, &payload_offset,
        CONFIGURATION_FIELD_WIFI_PASSWORD, configuration->wifi_password,
        ITERATE_KIT_WIFI_PASSWORD_CAPACITY - 1U, true);
    if (error != ITERATE_KIT_CONFIGURATION_OK) return error;
  }

  if (presence->has_platform) {
    if (!valid_base_url(configuration->os_base_url)) {
      return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
    }
    error = append_tlv(
        payload, payload_capacity, &payload_offset,
        CONFIGURATION_FIELD_OS_BASE_URL, configuration->os_base_url,
        ITERATE_KIT_OS_BASE_URL_CAPACITY - 1U, false);
    if (error != ITERATE_KIT_CONFIGURATION_OK) return error;

    if (configuration->platform == ITERATE_KIT_CONFIGURATION_PLATFORM_MIND) {
      if (!valid_oauth_client_id(configuration->oauth_client_id)) {
        return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
      }
      error = append_tlv(
          payload, payload_capacity, &payload_offset,
          CONFIGURATION_FIELD_OAUTH_CLIENT_ID, configuration->oauth_client_id,
          ITERATE_KIT_OAUTH_CLIENT_ID_CAPACITY - 1U, false);
      if (error != ITERATE_KIT_CONFIGURATION_OK) return error;
      error = append_tlv(
          payload, payload_capacity, &payload_offset,
          CONFIGURATION_FIELD_OAUTH_REFRESH_TOKEN, configuration->oauth_refresh_token,
          ITERATE_KIT_OAUTH_TOKEN_CAPACITY - 1U, false);
      if (error != ITERATE_KIT_CONFIGURATION_OK) return error;
    } else if (configuration->platform == ITERATE_KIT_CONFIGURATION_PLATFORM_PROJECT) {
      if (!valid_project_id(configuration->project_id)) {
        return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
      }
      error = append_tlv(
          payload, payload_capacity, &payload_offset,
          CONFIGURATION_FIELD_PROJECT_ID, configuration->project_id,
          ITERATE_KIT_PROJECT_ID_CAPACITY - 1U, false);
      if (error != ITERATE_KIT_CONFIGURATION_OK) return error;
      error = append_tlv(
          payload, payload_capacity, &payload_offset,
          CONFIGURATION_FIELD_PROJECT_API_KEY, configuration->project_api_key,
          ITERATE_KIT_PROJECT_API_KEY_CAPACITY - 1U, false);
      if (error != ITERATE_KIT_CONFIGURATION_OK) return error;
    } else {
      return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
    }
  }

  if (presence->has_status_voice) {
    if ((size_t)configuration->status_voice >=
        sizeof(status_voice_names) / sizeof(status_voice_names[0])) {
      return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
    }
    const char *voice_name =
        iterate_kit_status_voice_name((enum iterate_kit_status_voice)configuration->status_voice);
    error = append_tlv(
        payload, payload_capacity, &payload_offset,
        CONFIGURATION_FIELD_STATUS_VOICE, voice_name, 32U, false);
    if (error != ITERATE_KIT_CONFIGURATION_OK) return error;
  }

  memcpy(image, configuration_magic_prefix, sizeof(configuration_magic_prefix));
  image[sizeof(configuration_magic_prefix)] = '1';
  write_u32_le(image + 8U, (uint32_t)payload_offset);
  write_u32_le(image + 12U, configuration_crc32(payload, payload_offset));

  const size_t total = ITERATE_KIT_CONFIGURATION_HEADER_SIZE + payload_offset;
  if (total < capacity) {
    memset(image + total, 0xff, capacity - total);
  }
  if (written_size != NULL) *written_size = total;
  return ITERATE_KIT_CONFIGURATION_OK;
}

enum iterate_kit_configuration_error
iterate_kit_configuration_normalize_mind_origin(
    const char *typed, char *destination, size_t capacity) {
  char lower[ITERATE_KIT_OS_BASE_URL_CAPACITY];
  const char *start;
  const char *end;
  const char *scheme = "https://";
  bool explicit_scheme = false;
  size_t length;

  if (destination != NULL && capacity > 0U) destination[0] = '\0';
  if (typed == NULL || destination == NULL || capacity == 0U) {
    return ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT;
  }
  start = typed;
  while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') ++start;
  end = start + strlen(start);
  while (end > start &&
         (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
    --end;
  }
  if (end == start) return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;

  /* Strip one optional trailing slash. */
  if (end > start && end[-1] == '/') --end;
  length = (size_t)(end - start);
  if (length == 0U || length >= sizeof(lower)) {
    return length == 0U ? ITERATE_KIT_CONFIGURATION_INVALID_VALUE
                        : ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG;
  }
  for (size_t index = 0U; index < length; ++index) {
    char c = start[index];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    lower[index] = c;
  }
  lower[length] = '\0';

  const char *host = lower;
  if (strncmp(lower, "https://", 8U) == 0) {
    scheme = "https://";
    host = lower + 8U;
    explicit_scheme = true;
  } else if (strncmp(lower, "http://", 7U) == 0) {
    scheme = "http://";
    host = lower + 7U;
    explicit_scheme = true;
  } else if (strstr(lower, "://") != NULL) {
    return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
  }

  /* Strip optional trailing DNS dot. */
  size_t host_len = strlen(host);
  if (host_len > 0U && host[host_len - 1U] == '.') {
    lower[(size_t)(host - lower) + host_len - 1U] = '\0';
    --host_len;
  }
  if (host_len == 0U) return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;

  /* A bare mind domain like `example.com` maps to `https://iterate.example.com`;
   * an already-prefixed `iterate.example.com` or explicit scheme keeps its host. */
  const bool add_iterate_prefix =
      !explicit_scheme &&
      strncmp(host, "iterate.", 8U) != 0 &&
      strncmp(host, "localhost", 9U) != 0;
  if (!explicit_scheme && strchr(host, '.') == NULL) {
    return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
  }

  const char *prefix = add_iterate_prefix ? "iterate." : "";
  const size_t scheme_len = strlen(scheme);
  const size_t prefix_len = strlen(prefix);
  if (scheme_len + prefix_len + host_len + 1U > capacity) {
    return ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG;
  }
  memcpy(destination, scheme, scheme_len);
  memcpy(destination + scheme_len, prefix, prefix_len);
  memcpy(destination + scheme_len + prefix_len, host, host_len + 1U);
  if (!valid_base_url(destination)) {
    destination[0] = '\0';
    return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
  }
  return ITERATE_KIT_CONFIGURATION_OK;
}

void iterate_kit_configuration_clear_wifi(
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence) {
  if (configuration != NULL) {
    memset(configuration->wifi_ssid, 0, sizeof(configuration->wifi_ssid));
    memset(configuration->wifi_password, 0, sizeof(configuration->wifi_password));
  }
  if (presence != NULL) {
    presence->has_wifi = false;
  }
}
