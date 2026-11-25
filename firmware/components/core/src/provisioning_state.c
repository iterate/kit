#include "iterate/kit/provisioning_state.h"

#include <stdio.h>
#include <string.h>

/* 32-character Crockford Base32 lowercase alphabet (excludes i, l, o, u). */
static const char setup_alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
static const char hex_digits[] = "0123456789abcdef";

enum iterate_kit_boot_mode iterate_kit_provisioning_boot_mode(
    const struct iterate_kit_configuration_presence *presence) {
  if (presence == NULL || !presence->has_wifi) {
    return ITERATE_KIT_BOOT_MODE_SETUP_AP;
  }
  if (!presence->has_platform) {
    return ITERATE_KIT_BOOT_MODE_LAN_UNLINKED;
  }
  return ITERATE_KIT_BOOT_MODE_NORMAL;
}

const char *iterate_kit_boot_mode_name(enum iterate_kit_boot_mode mode) {
  switch (mode) {
    case ITERATE_KIT_BOOT_MODE_SETUP_AP:
      return "setup";
    case ITERATE_KIT_BOOT_MODE_LAN_UNLINKED:
      return "awaiting_mind";
    case ITERATE_KIT_BOOT_MODE_NORMAL:
      return "normal";
  }
  return "unknown";
}

void iterate_kit_device_short_id(
    const uint8_t mac[6], char out[ITERATE_KIT_SHORT_ID_CAPACITY]) {
  if (out == NULL) return;
  if (mac == NULL) {
    memcpy(out, "0000", 5U);
    return;
  }
  out[0] = hex_digits[(mac[4] >> 4U) & 0x0fU];
  out[1] = hex_digits[mac[4] & 0x0fU];
  out[2] = hex_digits[(mac[5] >> 4U) & 0x0fU];
  out[3] = hex_digits[mac[5] & 0x0fU];
  out[4] = '\0';
}

bool iterate_kit_device_hostname(
    const char short_id[ITERATE_KIT_SHORT_ID_CAPACITY],
    char *out,
    size_t capacity) {
  int written;
  if (out == NULL || capacity == 0U) return false;
  out[0] = '\0';
  if (short_id == NULL || strlen(short_id) != 4U) return false;
  written = snprintf(out, capacity, "iterate-%s", short_id);
  if (written < 0 || (size_t)written >= capacity) {
    out[0] = '\0';
    return false;
  }
  return true;
}

void iterate_kit_setup_code_format(
    const uint8_t entropy[8], char out[ITERATE_KIT_SETUP_CODE_CAPACITY]) {
  if (out == NULL) return;
  if (entropy == NULL) {
    memset(out, 0, ITERATE_KIT_SETUP_CODE_CAPACITY);
    return;
  }
  for (size_t index = 0U; index < 4U; ++index) {
    out[index] = setup_alphabet[entropy[index] & 31U];
  }
  out[4] = '-';
  for (size_t index = 0U; index < 4U; ++index) {
    out[5U + index] = setup_alphabet[entropy[4U + index] & 31U];
  }
  out[9] = '\0';
}

static bool in_setup_alphabet(char c) {
  for (size_t index = 0U; index < sizeof(setup_alphabet) - 1U; ++index) {
    if (setup_alphabet[index] == c) return true;
  }
  return false;
}

bool iterate_kit_setup_code_valid(const char *code) {
  if (code == NULL || strlen(code) != 9U || code[4] != '-') return false;
  for (size_t index = 0U; index < 9U; ++index) {
    if (index == 4U) continue;
    if (!in_setup_alphabet(code[index])) return false;
  }
  return true;
}

static size_t normalize_code_chars(const char *input, char out[8]) {
  size_t count = 0U;
  memset(out, 0, 8U);
  if (input == NULL) return 0U;
  for (const char *cursor = input; *cursor != '\0'; ++cursor) {
    char c = *cursor;
    if (c == '-' || c == ' ' || c == '\t') continue;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (count >= 8U) return 9U;
    out[count++] = c;
  }
  return count;
}

static bool constant_time_equal(const char *a, const char *b, size_t length) {
  uint8_t diff = 0U;
  for (size_t index = 0U; index < length; ++index) {
    diff |= (uint8_t)((uint8_t)a[index] ^ (uint8_t)b[index]);
  }
  return diff == 0U;
}

bool iterate_kit_setup_code_equal(const char *candidate, const char *expected) {
  char norm_a[8];
  char norm_b[8];
  const size_t len_a = normalize_code_chars(candidate, norm_a);
  const size_t len_b = normalize_code_chars(expected, norm_b);
  const bool chars_match = constant_time_equal(norm_a, norm_b, 8U);
  return len_a == 8U && len_b == 8U && chars_match;
}

void iterate_kit_csrf_token_format(
    const uint8_t entropy[8], char out[ITERATE_KIT_CSRF_TOKEN_CAPACITY]) {
  if (out == NULL) return;
  if (entropy == NULL) {
    memset(out, 0, ITERATE_KIT_CSRF_TOKEN_CAPACITY);
    return;
  }
  for (size_t index = 0U; index < 8U; ++index) {
    out[index * 2U] = hex_digits[(entropy[index] >> 4U) & 0x0fU];
    out[index * 2U + 1U] = hex_digits[entropy[index] & 0x0fU];
  }
  out[16] = '\0';
}

static bool append_qr_escaped(
    char *out, size_t capacity, size_t *offset, const char *value) {
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    const char c = *cursor;
    const bool needs_escape =
        c == '\\' || c == ';' || c == ',' || c == ':' || c == '"';
    if (needs_escape) {
      if (*offset + 2U >= capacity) return false;
      out[(*offset)++] = '\\';
      out[(*offset)++] = c;
    } else {
      if (*offset + 1U >= capacity) return false;
      out[(*offset)++] = c;
    }
  }
  out[*offset] = '\0';
  return true;
}

static bool append_literal(
    char *out, size_t capacity, size_t *offset, const char *text) {
  const size_t len = strlen(text);
  if (*offset + len >= capacity) return false;
  memcpy(out + *offset, text, len + 1U);
  *offset += len;
  return true;
}

bool iterate_kit_wifi_qr_format(
    const char *ssid, const char *password, char *out, size_t capacity) {
  size_t offset = 0U;
  if (out == NULL || capacity == 0U) return false;
  out[0] = '\0';
  if (ssid == NULL || ssid[0] == '\0') return false;
  const bool has_password = password != NULL && password[0] != '\0';
  if (!append_literal(out, capacity, &offset, has_password ? "WIFI:T:WPA;S:" : "WIFI:T:nopass;S:") ||
      !append_qr_escaped(out, capacity, &offset, ssid)) {
    out[0] = '\0';
    return false;
  }
  if (has_password) {
    if (!append_literal(out, capacity, &offset, ";P:") ||
        !append_qr_escaped(out, capacity, &offset, password)) {
      out[0] = '\0';
      return false;
    }
  }
  if (!append_literal(out, capacity, &offset, ";;")) {
    out[0] = '\0';
    return false;
  }
  return true;
}

void iterate_kit_portal_guard_init(
    struct iterate_kit_portal_guard *guard,
    const char short_id[ITERATE_KIT_SHORT_ID_CAPACITY],
    const char setup_code[ITERATE_KIT_SETUP_CODE_CAPACITY],
    const char csrf_token[ITERATE_KIT_CSRF_TOKEN_CAPACITY],
    bool is_setup_ap) {
  if (guard == NULL) return;
  memset(guard, 0, sizeof(*guard));
  if (short_id != NULL) {
    (void)snprintf(guard->short_id, sizeof(guard->short_id), "%s", short_id);
  }
  if (setup_code != NULL) {
    (void)snprintf(guard->setup_code, sizeof(guard->setup_code), "%s", setup_code);
  }
  if (csrf_token != NULL) {
    (void)snprintf(guard->csrf_token, sizeof(guard->csrf_token), "%s", csrf_token);
  }
  guard->is_setup_ap = is_setup_ap;
}

void iterate_kit_portal_guard_unlock_physical(
    struct iterate_kit_portal_guard *guard, uint64_t now_ms) {
  if (guard == NULL) return;
  guard->unlocked_until_ms = now_ms + (uint64_t)ITERATE_KIT_PORTAL_UNLOCK_MS;
  guard->failed_attempts = 0U;
  guard->lockout_until_ms = 0U;
}

enum iterate_kit_portal_auth_result iterate_kit_portal_guard_unlock_with_code(
    struct iterate_kit_portal_guard *guard,
    const char *csrf_submitted,
    const char *code_submitted,
    uint64_t now_ms) {
  const enum iterate_kit_portal_auth_result result =
      iterate_kit_portal_guard_authorize_post(
          guard, csrf_submitted, code_submitted, now_ms);
  if (result == ITERATE_KIT_PORTAL_AUTH_OK && guard != NULL && !guard->is_setup_ap) {
    iterate_kit_portal_guard_unlock_physical(guard, now_ms);
  }
  return result;
}

bool iterate_kit_portal_guard_is_unlocked(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms) {
  if (guard == NULL || guard->unlocked_until_ms == 0U) return false;
  return now_ms < guard->unlocked_until_ms;
}

uint32_t iterate_kit_portal_guard_unlock_remaining_s(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms) {
  if (!iterate_kit_portal_guard_is_unlocked(guard, now_ms)) return 0U;
  return (uint32_t)((guard->unlocked_until_ms - now_ms + 999U) / 1000U);
}

static bool ascii_ieq(const char *a, size_t a_len, const char *b) {
  const size_t b_len = strlen(b);
  if (a_len != b_len) return false;
  for (size_t index = 0U; index < a_len; ++index) {
    char x = a[index];
    char y = b[index];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

bool iterate_kit_portal_guard_host_allowed(
    const struct iterate_kit_portal_guard *guard,
    const char *host_header,
    const char *current_ip) {
  char expected_local[32];
  const char *colon;
  size_t host_len;
  if (guard == NULL || host_header == NULL || host_header[0] == '\0') return false;
  colon = strchr(host_header, ':');
  if (colon != NULL) {
    if (strcmp(colon, ":80") != 0) return false;
    host_len = (size_t)(colon - host_header);
  } else {
    host_len = strlen(host_header);
  }
  /* Strip optional trailing DNS dot. */
  if (host_len > 0U && host_header[host_len - 1U] == '.') --host_len;
  if (host_len == 0U) return false;

  if (guard->is_setup_ap && ascii_ieq(host_header, host_len, "192.168.4.1")) {
    return true;
  }
  if (guard->short_id[0] != '\0') {
    const int written = snprintf(
        expected_local, sizeof(expected_local), "iterate-%s.local", guard->short_id);
    if (written > 0 && (size_t)written < sizeof(expected_local) &&
        ascii_ieq(host_header, host_len, expected_local)) {
      return true;
    }
  }
  if (current_ip != NULL && current_ip[0] != '\0' &&
      ascii_ieq(host_header, host_len, current_ip)) {
    return true;
  }
  return false;
}

enum iterate_kit_portal_auth_result iterate_kit_portal_guard_authorize_post(
    struct iterate_kit_portal_guard *guard,
    const char *csrf_submitted,
    const char *code_submitted,
    uint64_t now_ms) {
  if (guard == NULL || csrf_submitted == NULL) {
    return ITERATE_KIT_PORTAL_AUTH_BAD_CSRF;
  }
  const size_t expected_csrf_len = strlen(guard->csrf_token);
  if (expected_csrf_len == 0U ||
      strlen(csrf_submitted) != expected_csrf_len ||
      !constant_time_equal(csrf_submitted, guard->csrf_token, expected_csrf_len)) {
    return ITERATE_KIT_PORTAL_AUTH_BAD_CSRF;
  }
  if (guard->is_setup_ap) {
    return ITERATE_KIT_PORTAL_AUTH_OK;
  }
  if (iterate_kit_portal_guard_is_unlocked(guard, now_ms)) {
    return ITERATE_KIT_PORTAL_AUTH_OK;
  }
  if (guard->lockout_until_ms != 0U && now_ms < guard->lockout_until_ms) {
    return ITERATE_KIT_PORTAL_AUTH_LOCKED_OUT;
  }
  if (code_submitted == NULL || code_submitted[0] == '\0') {
    return ITERATE_KIT_PORTAL_AUTH_REQUIRED;
  }
  if (iterate_kit_setup_code_equal(code_submitted, guard->setup_code)) {
    guard->failed_attempts = 0U;
    guard->lockout_until_ms = 0U;
    return ITERATE_KIT_PORTAL_AUTH_OK;
  }
  if (guard->failed_attempts < UINT32_MAX) {
    ++guard->failed_attempts;
  }
  if (guard->failed_attempts > ITERATE_KIT_PORTAL_MAX_FREE_FAILURES) {
    uint32_t shift =
        guard->failed_attempts - (uint32_t)ITERATE_KIT_PORTAL_MAX_FREE_FAILURES - 1U;
    if (shift > 5U) shift = 5U;
    guard->lockout_until_ms =
        now_ms + ((uint64_t)ITERATE_KIT_PORTAL_LOCKOUT_BASE_MS << shift);
  }
  return ITERATE_KIT_PORTAL_AUTH_BAD_CODE;
}
