#include "iterate/kit/configuration.h"
#include "configuration_fixture.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/*
 * The browser/CLI flasher writes this image with TypeScript and firmware reads
 * it with C, so neither implementation's round-trip test can prove wire
 * compatibility alone. Decode the checked-in cross-language golden image and
 * pin the complete credential set. The size gate is permanent device RAM,
 * not cosmetic struct size: growing it must be an explicit firmware budget
 * decision rather than accidental padding or field creep.
 */
static void decodes_the_typescript_golden_image(void) {
  struct iterate_kit_configuration configuration;
  const enum iterate_kit_configuration_error error =
      iterate_kit_configuration_decode(
          &configuration,
          iterate_kit_test_configuration_image,
          sizeof(iterate_kit_test_configuration_image));

  assert(error == ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(configuration.wifi_ssid, "studio") == 0);
  assert(strcmp(
      configuration.wifi_password,
      "correct horse battery staple") == 0);
  assert(strcmp(
      configuration.os_base_url,
      "https://os.iterate2.com") == 0);
  assert(strcmp(configuration.project_id, "prj_voice_lab") == 0);
  assert(strcmp(configuration.project_api_key, "itxk_secret") == 0);
  assert(configuration.platform == ITERATE_KIT_CONFIGURATION_PLATFORM_PROJECT);
  /* 424 bytes before the mind fields; they add a client id and a token. */
  assert(sizeof(configuration) <= 784U);
}

/*
 * A checksum failure can happen after several length-prefixed fields have
 * already parsed. Returning those partial values would mix old/corrupt Wi-Fi
 * and bearer credentials into a plausible connection attempt. Fail closed by
 * clearing every secret-bearing field, rather than relying on each caller to
 * remember which decoder errors may have written a prefix.
 */
static void classifies_corruption_without_partial_credentials(void) {
  uint8_t corrupted[sizeof(iterate_kit_test_configuration_image)];
  struct iterate_kit_configuration configuration;
  memcpy(
      corrupted,
      iterate_kit_test_configuration_image,
      sizeof(corrupted));
  corrupted[sizeof(corrupted) - 1U] ^= 1U;
  memset(&configuration, 0xa5, sizeof(configuration));

  assert(iterate_kit_configuration_decode(
      &configuration, corrupted, sizeof(corrupted)) ==
      ITERATE_KIT_CONFIGURATION_CHECKSUM_MISMATCH);
  assert(configuration.wifi_ssid[0] == '\0');
  assert(configuration.wifi_password[0] == '\0');
  assert(configuration.os_base_url[0] == '\0');
  assert(configuration.project_id[0] == '\0');
  assert(configuration.project_api_key[0] == '\0');
}

/*
 * A partly flashed image and a deliberately newer image require different
 * operator actions: retry flashing versus upgrade firmware. Preserve that
 * distinction at the decoder boundary instead of collapsing both into a
 * generic parse error that would make provisioning failures unactionable.
 */
static void rejects_truncated_and_wrong_version_images(void) {
  uint8_t wrong_magic[sizeof(iterate_kit_test_configuration_image)];
  struct iterate_kit_configuration configuration;
  memcpy(
      wrong_magic,
      iterate_kit_test_configuration_image,
      sizeof(wrong_magic));
  wrong_magic[7] = '2';

  assert(iterate_kit_configuration_decode(
      &configuration, iterate_kit_test_configuration_image, 15U) ==
      ITERATE_KIT_CONFIGURATION_TRUNCATED);
  assert(iterate_kit_configuration_decode(
      &configuration, wrong_magic, sizeof(wrong_magic)) ==
      ITERATE_KIT_CONFIGURATION_UNSUPPORTED_VERSION);
}

/*
 * The control connection is created during boot and reconnect storms, where a
 * heap-building URL helper would add fragmentation and another failure mode.
 * Convert only the transport scheme into a caller-sized fixed buffer and prove
 * both production TLS and local cleartext development forms.
 *
 * THE PATH IS `/api`: the OS's public endpoint, whose OAuth gate resolves the
 * `Authorization: Bearer` the transport sends on the upgrade (the blob's key,
 * a personal access token). The endpoint is four bytes longer than the base
 * URL.
 */
static void builds_the_itx_websocket_endpoint_without_allocation(void) {
  char endpoint[ITERATE_KIT_ITX_WEBSOCKET_URL_CAPACITY];

  assert(iterate_kit_configuration_build_itx_websocket_url(
      "https://os.iterate.com", endpoint, sizeof(endpoint)) ==
      ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(endpoint, "wss://os.iterate.com/api") == 0);

  assert(iterate_kit_configuration_build_itx_websocket_url(
      "http://localhost:8787", endpoint, sizeof(endpoint)) ==
      ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(endpoint, "ws://localhost:8787/api") == 0);
}

/*
 * Truncating an endpoint would often leave a syntactically plausible hostname,
 * and preserving an old output buffer after validation failure could connect
 * with stale authority. Reject non-origin base URLs and insufficient capacity,
 * then zero the destination so callers cannot accidentally use either result.
 */
static void rejects_invalid_or_truncated_itx_websocket_endpoints(void) {
  char endpoint[8];
  memset(endpoint, 0xa5, sizeof(endpoint));

  assert(iterate_kit_configuration_build_itx_websocket_url(
      "https://os.iterate.com", endpoint, sizeof(endpoint)) ==
      ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG);
  assert(endpoint[0] == '\0');

  endpoint[0] = 'x';
  assert(iterate_kit_configuration_build_itx_websocket_url(
      "https://os.iterate.com/path", endpoint, sizeof(endpoint)) ==
      ITERATE_KIT_CONFIGURATION_INVALID_VALUE);
  assert(endpoint[0] == '\0');
}

/*
 * A BLOB A BOARD IS FLASHED WITH: a project reference with no `prj_` prefix,
 * and a key running to the field's full 128 bytes. Refusing either is a board
 * that reads its own partition, rejects it, never dials, and from outside
 * looks dead.
 */
static void decodes_the_flashed_image_with_an_unprefixed_project_and_a_full_length_key(void) {
  struct iterate_kit_configuration configuration;
  char key[ITERATE_KIT_PROJECT_API_KEY_CAPACITY];
  size_t index;
  const enum iterate_kit_configuration_error error =
      iterate_kit_configuration_decode(
          &configuration,
          iterate_kit_test_flashed_configuration_image,
          sizeof(iterate_kit_test_flashed_configuration_image));

  for (index = 0U; index + 1U < sizeof(key); ++index) key[index] = 'k';
  key[sizeof(key) - 1U] = '\0';

  assert(error == ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(configuration.os_base_url, "https://os.iterate2.com") == 0);
  assert(strcmp(configuration.project_id, "prj-voice") == 0);
  assert(strlen(configuration.project_api_key) ==
      ITERATE_KIT_PROJECT_API_KEY_CAPACITY - 1U);
  assert(strcmp(configuration.project_api_key, key) == 0);
}

/* The golden image with a status-voice field appended, its checksum redone. */
static size_t with_status_voice(uint8_t *out, size_t capacity, const char *name) {
  const size_t golden = sizeof(iterate_kit_test_configuration_image);
  const size_t name_length = strlen(name);
  const size_t size = golden + 3U + name_length;
  uint32_t payload_size;
  uint32_t crc = UINT32_C(0xffffffff);
  assert(size <= capacity);
  memcpy(out, iterate_kit_test_configuration_image, golden);
  out[golden] = 6U;
  out[golden + 1U] = (uint8_t)name_length;
  out[golden + 2U] = 0U;
  memcpy(out + golden + 3U, name, name_length);
  payload_size = (uint32_t)(size - ITERATE_KIT_CONFIGURATION_HEADER_SIZE);
  for (size_t i = ITERATE_KIT_CONFIGURATION_HEADER_SIZE; i < size; i++) {
    crc ^= out[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
  }
  crc ^= UINT32_C(0xffffffff);
  for (int i = 0; i < 4; i++) {
    out[8 + i] = (uint8_t)(payload_size >> (8 * i));
    out[12 + i] = (uint8_t)(crc >> (8 * i));
  }
  return size;
}

/*
 * The status voice is the one optional field: every image written before it
 * existed has none, and Kit may one day write a name this firmware has never
 * heard of. Neither may cost the board its Wi-Fi and key, so both sing the
 * default rather than failing the whole image.
 */
static void decodes_the_status_voice_and_defaults_what_it_does_not_know(void) {
  uint8_t image[256];
  struct iterate_kit_configuration configuration;

  assert(iterate_kit_configuration_decode(
            &configuration, iterate_kit_test_configuration_image,
            sizeof(iterate_kit_test_configuration_image)) == ITERATE_KIT_CONFIGURATION_OK);
  assert(configuration.status_voice == ITERATE_KIT_STATUS_VOICE_GREENSLEEVES);

  assert(iterate_kit_configuration_decode(
            &configuration, image, with_status_voice(image, sizeof(image), "daisy-bell")) ==
        ITERATE_KIT_CONFIGURATION_OK);
  assert(configuration.status_voice == ITERATE_KIT_STATUS_VOICE_DAISY_BELL);
  assert(strcmp(configuration.wifi_ssid, "studio") == 0);

  assert(iterate_kit_configuration_decode(
            &configuration, image, with_status_voice(image, sizeof(image), "off")) ==
        ITERATE_KIT_CONFIGURATION_OK);
  assert(configuration.status_voice == ITERATE_KIT_STATUS_VOICE_OFF);
  assert(strcmp(iterate_kit_status_voice_name(ITERATE_KIT_STATUS_VOICE_OFF), "off") == 0);

  assert(iterate_kit_configuration_decode(
            &configuration, image, with_status_voice(image, sizeof(image), "sea-shanty")) ==
        ITERATE_KIT_CONFIGURATION_OK);
  assert(configuration.status_voice == ITERATE_KIT_STATUS_VOICE_GREENSLEEVES);
  assert(strcmp(configuration.project_api_key, "itxk_secret") == 0);
}

static void decodes_the_typescript_mind_image(void) {
  struct iterate_kit_configuration configuration;
  assert(iterate_kit_configuration_decode(
      &configuration,
      iterate_kit_test_mind_configuration_image,
      sizeof(iterate_kit_test_mind_configuration_image)) ==
      ITERATE_KIT_CONFIGURATION_OK);
  assert(configuration.platform == ITERATE_KIT_CONFIGURATION_PLATFORM_MIND);
  assert(strcmp(configuration.os_base_url, "https://iterate.example.com") == 0);
  assert(strcmp(
      configuration.oauth_client_id,
      "https://k.iterate.com/devices/mac/clients/"
      "00000000-0000-4000-8000-000000000000.json") == 0);
  assert(strcmp(configuration.oauth_refresh_token, "user_1:grant_1:secret") == 0);
  assert(configuration.project_id[0] == '\0');
  assert(configuration.project_api_key[0] == '\0');
  assert(configuration.status_voice == ITERATE_KIT_STATUS_VOICE_SPOKEN);
}

/* Appends one TLV field and fixes the header's size and CRC. */
static size_t append_field(uint8_t *image, size_t size, uint8_t tag, const char *value) {
  const size_t length = strlen(value);
  uint32_t payload;
  uint32_t crc = UINT32_C(0xffffffff);
  image[size] = tag;
  image[size + 1U] = (uint8_t)(length & 0xffU);
  image[size + 2U] = (uint8_t)(length >> 8U);
  memcpy(image + size + 3U, value, length);
  size += 3U + length;
  payload = (uint32_t)(size - ITERATE_KIT_CONFIGURATION_HEADER_SIZE);
  image[8] = (uint8_t)payload;
  image[9] = (uint8_t)(payload >> 8U);
  image[10] = (uint8_t)(payload >> 16U);
  image[11] = (uint8_t)(payload >> 24U);
  for (size_t index = ITERATE_KIT_CONFIGURATION_HEADER_SIZE; index < size; ++index) {
    crc ^= image[index];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
  }
  crc ^= UINT32_C(0xffffffff);
  image[12] = (uint8_t)crc;
  image[13] = (uint8_t)(crc >> 8U);
  image[14] = (uint8_t)(crc >> 16U);
  image[15] = (uint8_t)(crc >> 24U);
  return size;
}

/*
 * ONE PLATFORM, WHOLE: a project's key beside a mind's refresh token would
 * leave the board to guess which credential to present, and half a mind
 * credential is a board that can never sign in.
 */
static void refuses_mixed_or_half_credentials(void) {
  uint8_t image[1024];
  struct iterate_kit_configuration configuration;
  size_t size = sizeof(iterate_kit_test_configuration_image);
  memcpy(image, iterate_kit_test_configuration_image, size);
  size = append_field(image, size, 8, "user_1:grant_1:secret");
  assert(iterate_kit_configuration_decode(&configuration, image, size) ==
         ITERATE_KIT_CONFIGURATION_INVALID_VALUE);

  memcpy(image, iterate_kit_test_mind_configuration_image,
         sizeof(iterate_kit_test_mind_configuration_image));
  /* the same image less its last two fields (refresh token, status voice) */
  size = ITERATE_KIT_CONFIGURATION_HEADER_SIZE;
  size = append_field(image, size, 1, "studio");
  size = append_field(image, size, 2, "");
  size = append_field(image, size, 3, "https://iterate.example.com");
  size = append_field(image, size, 7, "https://k.iterate.com/devices/mac/clients/x.json");
  assert(iterate_kit_configuration_decode(&configuration, image, size) ==
         ITERATE_KIT_CONFIGURATION_MISSING_FIELD);
  assert(configuration.oauth_client_id[0] == '\0');
}

static void encodes_byte_for_byte_with_typescript_golden_images(void) {
  struct iterate_kit_configuration cfg;
  struct iterate_kit_configuration_presence presence;
  uint8_t encoded[512];
  size_t written = 0U;

  /* Project golden image (no status-voice field). */
  assert(iterate_kit_configuration_decode_stored(
      &cfg, &presence, iterate_kit_test_configuration_image,
      sizeof(iterate_kit_test_configuration_image)) == ITERATE_KIT_CONFIGURATION_OK);
  assert(presence.has_wifi && presence.has_platform && !presence.has_status_voice);
  assert(iterate_kit_configuration_encode(
      &cfg, &presence, encoded, sizeof(encoded), &written) == ITERATE_KIT_CONFIGURATION_OK);
  assert(written == sizeof(iterate_kit_test_configuration_image));
  assert(memcmp(encoded, iterate_kit_test_configuration_image, written) == 0);
  for (size_t i = written; i < sizeof(encoded); ++i) assert(encoded[i] == 0xffU);

  /* Mind golden image (with status-voice = spoken). */
  assert(iterate_kit_configuration_decode_stored(
      &cfg, &presence, iterate_kit_test_mind_configuration_image,
      sizeof(iterate_kit_test_mind_configuration_image)) == ITERATE_KIT_CONFIGURATION_OK);
  assert(presence.has_wifi && presence.has_platform && presence.has_status_voice);
  assert(iterate_kit_configuration_encode(
      &cfg, &presence, encoded, sizeof(encoded), &written) == ITERATE_KIT_CONFIGURATION_OK);
  assert(written == sizeof(iterate_kit_test_mind_configuration_image));
  assert(memcmp(encoded, iterate_kit_test_mind_configuration_image, written) == 0);
}

static void round_trips_partial_wifi_only_and_wifi_reset_images(void) {
  struct iterate_kit_configuration cfg;
  struct iterate_kit_configuration_presence presence;
  struct iterate_kit_configuration decoded;
  struct iterate_kit_configuration_presence decoded_presence;
  uint8_t image[512];
  size_t written = 0U;

  /* Wi-Fi only (after SoftAP setup, before linking a mind). */
  memset(&cfg, 0, sizeof(cfg));
  memset(&presence, 0, sizeof(presence));
  memcpy(cfg.wifi_ssid, "home-net", sizeof("home-net"));
  memcpy(cfg.wifi_password, "passphrase123", sizeof("passphrase123"));
  cfg.status_voice = ITERATE_KIT_STATUS_VOICE_SPOKEN;
  presence.has_wifi = true;
  presence.has_status_voice = true;

  assert(iterate_kit_configuration_encode(&cfg, &presence, image, sizeof(image), &written) ==
         ITERATE_KIT_CONFIGURATION_OK);
  assert(iterate_kit_configuration_decode_stored(&decoded, &decoded_presence, image, sizeof(image)) ==
         ITERATE_KIT_CONFIGURATION_OK);
  assert(decoded_presence.has_wifi && !decoded_presence.has_platform &&
         decoded_presence.has_status_voice);
  assert(strcmp(decoded.wifi_ssid, "home-net") == 0);
  assert(strcmp(decoded.wifi_password, "passphrase123") == 0);
  assert(decoded.status_voice == ITERATE_KIT_STATUS_VOICE_SPOKEN);
  /* Strict decoder still refuses an image without a platform credential. */
  assert(iterate_kit_configuration_decode(&decoded, image, sizeof(image)) ==
         ITERATE_KIT_CONFIGURATION_MISSING_FIELD);

  /* Wi-Fi reset clears ONLY Wi-Fi while preserving mind credential & status voice. */
  assert(iterate_kit_configuration_decode_stored(
      &cfg, &presence, iterate_kit_test_mind_configuration_image,
      sizeof(iterate_kit_test_mind_configuration_image)) == ITERATE_KIT_CONFIGURATION_OK);
  iterate_kit_configuration_clear_wifi(&cfg, &presence);
  assert(!presence.has_wifi && presence.has_platform);
  assert(cfg.wifi_ssid[0] == '\0' && cfg.wifi_password[0] == '\0');
  assert(iterate_kit_configuration_encode(&cfg, &presence, image, sizeof(image), &written) ==
         ITERATE_KIT_CONFIGURATION_OK);
  assert(iterate_kit_configuration_decode_stored(&decoded, &decoded_presence, image, sizeof(image)) ==
         ITERATE_KIT_CONFIGURATION_OK);
  assert(!decoded_presence.has_wifi && decoded_presence.has_platform);
  assert(decoded.platform == ITERATE_KIT_CONFIGURATION_PLATFORM_MIND);
  assert(strcmp(decoded.os_base_url, "https://iterate.example.com") == 0);
  assert(strcmp(decoded.oauth_refresh_token, "user_1:grant_1:secret") == 0);
  assert(decoded.status_voice == ITERATE_KIT_STATUS_VOICE_SPOKEN);
}

static void validates_passwords_status_voices_and_mind_origins(void) {
  char origin[ITERATE_KIT_OS_BASE_URL_CAPACITY];
  enum iterate_kit_status_voice voice;

  assert(iterate_kit_configuration_valid_wifi_password(""));
  assert(iterate_kit_configuration_valid_wifi_password("12345678"));
  assert(!iterate_kit_configuration_valid_wifi_password("1234567"));
  assert(iterate_kit_configuration_valid_wifi_password(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  assert(!iterate_kit_configuration_valid_wifi_password(
      "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg"));

  assert(iterate_kit_configuration_parse_status_voice("spoken", &voice) &&
         voice == ITERATE_KIT_STATUS_VOICE_SPOKEN);
  assert(iterate_kit_configuration_parse_status_voice("off", &voice) &&
         voice == ITERATE_KIT_STATUS_VOICE_OFF);
  assert(!iterate_kit_configuration_parse_status_voice("sea-shanty", &voice));

  assert(iterate_kit_configuration_normalize_mind_origin(
      "  Example.COM  ", origin, sizeof(origin)) == ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(origin, "https://iterate.example.com") == 0);
  assert(iterate_kit_configuration_normalize_mind_origin(
      "iterate.example.com/", origin, sizeof(origin)) == ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(origin, "https://iterate.example.com") == 0);
  assert(iterate_kit_configuration_normalize_mind_origin(
      "https://iterate.example.com/", origin, sizeof(origin)) == ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(origin, "https://iterate.example.com") == 0);
  assert(iterate_kit_configuration_normalize_mind_origin(
      "http://localhost:8788", origin, sizeof(origin)) == ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(origin, "http://localhost:8788") == 0);
  assert(iterate_kit_configuration_normalize_mind_origin(
      "https://iterate.example.com/bad", origin, sizeof(origin)) ==
      ITERATE_KIT_CONFIGURATION_INVALID_VALUE);
  assert(origin[0] == '\0');
  assert(iterate_kit_configuration_normalize_mind_origin(
      "nodot", origin, sizeof(origin)) == ITERATE_KIT_CONFIGURATION_INVALID_VALUE);
}

int main(void) {
  decodes_the_typescript_mind_image();
  refuses_mixed_or_half_credentials();
  encodes_byte_for_byte_with_typescript_golden_images();
  round_trips_partial_wifi_only_and_wifi_reset_images();
  validates_passwords_status_voices_and_mind_origins();
  decodes_the_typescript_golden_image();
  decodes_the_status_voice_and_defaults_what_it_does_not_know();
  decodes_the_flashed_image_with_an_unprefixed_project_and_a_full_length_key();
  classifies_corruption_without_partial_credentials();
  rejects_truncated_and_wrong_version_images();
  builds_the_itx_websocket_endpoint_without_allocation();
  rejects_invalid_or_truncated_itx_websocket_endpoints();
  return 0;
}
