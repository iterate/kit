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

const char *iterate_kit_claim_color_name(enum iterate_kit_claim_color color) {
  switch (color) {
    case ITERATE_KIT_CLAIM_COLOR_BLUE:
      return "Blue";
    case ITERATE_KIT_CLAIM_COLOR_GREEN:
      return "Green";
    case ITERATE_KIT_CLAIM_COLOR_AMBER:
      return "Amber";
    case ITERATE_KIT_CLAIM_COLOR_MAGENTA:
      return "Magenta";
  }
  return "Unknown";
}

const char *iterate_kit_claim_position_name(enum iterate_kit_claim_position position) {
  switch (position) {
    case ITERATE_KIT_CLAIM_POS_TOP:
      return "Top (12 o'clock)";
    case ITERATE_KIT_CLAIM_POS_RIGHT:
      return "Right (3 o'clock)";
    case ITERATE_KIT_CLAIM_POS_BOTTOM:
      return "Bottom (6 o'clock)";
    case ITERATE_KIT_CLAIM_POS_LEFT:
      return "Left (9 o'clock)";
  }
  return "Unknown";
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
    const uint8_t entropy[ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES],
    char out[ITERATE_KIT_SETUP_CODE_CAPACITY]) {
  if (out == NULL) return;
  if (entropy == NULL) {
    memset(out, 0, ITERATE_KIT_SETUP_CODE_CAPACITY);
    return;
  }
  size_t dst = 0U;
  for (size_t index = 0U; index < ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES; ++index) {
    if (index == 4U || index == 8U) out[dst++] = '-';
    out[dst++] = setup_alphabet[entropy[index] & 31U];
  }
  out[dst] = '\0';
}

static bool in_setup_alphabet(char c) {
  for (size_t index = 0U; index < sizeof(setup_alphabet) - 1U; ++index) {
    if (setup_alphabet[index] == c) return true;
  }
  return false;
}

bool iterate_kit_setup_code_valid(const char *code) {
  if (code == NULL || strlen(code) != 14U || code[4] != '-' || code[9] != '-') {
    return false;
  }
  for (size_t index = 0U; index < 14U; ++index) {
    if (index == 4U || index == 9U) continue;
    if (!in_setup_alphabet(code[index])) return false;
  }
  return true;
}

static size_t normalize_code_chars(
    const char *input, char out[ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES]) {
  size_t count = 0U;
  memset(out, 0, ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES);
  if (input == NULL) return 0U;
  for (const char *cursor = input; *cursor != '\0'; ++cursor) {
    char c = *cursor;
    if (c == '-' || c == ' ' || c == '\t') continue;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (count >= ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES) {
      return ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES + 1U;
    }
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
  char norm_a[ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES];
  char norm_b[ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES];
  const size_t len_a = normalize_code_chars(candidate, norm_a);
  const size_t len_b = normalize_code_chars(expected, norm_b);
  const bool chars_match =
      constant_time_equal(norm_a, norm_b, ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES);
  return len_a == ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES &&
      len_b == ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES && chars_match;
}

static void format_hex16(const uint8_t entropy[16], char out[33]) {
  if (out == NULL) return;
  if (entropy == NULL) {
    memset(out, 0, 33U);
    return;
  }
  for (size_t index = 0U; index < 16U; ++index) {
    out[index * 2U] = hex_digits[(entropy[index] >> 4U) & 0x0fU];
    out[index * 2U + 1U] = hex_digits[entropy[index] & 0x0fU];
  }
  out[32] = '\0';
}

void iterate_kit_session_token_format(
    const uint8_t entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    char out[ITERATE_KIT_SESSION_TOKEN_CAPACITY]) {
  format_hex16(entropy, out);
}

void iterate_kit_csrf_token_format(
    const uint8_t entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    char out[ITERATE_KIT_CSRF_TOKEN_CAPACITY]) {
  format_hex16(entropy, out);
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
    bool is_setup_ap) {
  if (guard == NULL) return;
  memset(guard, 0, sizeof(*guard));
  if (short_id != NULL) {
    (void)snprintf(guard->short_id, sizeof(guard->short_id), "%s", short_id);
  }
  guard->is_setup_ap = is_setup_ap;
}

void iterate_kit_portal_guard_cancel_claim(struct iterate_kit_portal_guard *guard) {
  if (guard == NULL) return;
  guard->claim_state = ITERATE_KIT_CLAIM_NONE;
  guard->claim_expires_at_ms = 0U;
  memset(guard->pending_session_token, 0, sizeof(guard->pending_session_token));
  memset(&guard->pending_pattern, 0, sizeof(guard->pending_pattern));
}

void iterate_kit_portal_guard_disable_writes(struct iterate_kit_portal_guard *guard) {
  if (guard == NULL) return;
  guard->writes_disabled = true;
  iterate_kit_portal_guard_cancel_claim(guard);
}

bool iterate_kit_portal_guard_writes_disabled(
    const struct iterate_kit_portal_guard *guard) {
  return guard == NULL || guard->writes_disabled;
}

static void expire_state(struct iterate_kit_portal_guard *guard, uint64_t now_ms) {
  if (guard->claim_state != ITERATE_KIT_CLAIM_NONE && now_ms >= guard->claim_expires_at_ms) {
    iterate_kit_portal_guard_cancel_claim(guard);
  }
  for (size_t i = 0U; i < ITERATE_KIT_PORTAL_MAX_SESSIONS; ++i) {
    struct iterate_kit_portal_session *s = &guard->sessions[i];
    if (s->active && s->unlocked && now_ms >= s->unlocked_until_ms) {
      s->unlocked = false;
      s->unlocked_until_ms = 0U;
    }
  }
}

static struct iterate_kit_portal_session *find_session_mut(
    struct iterate_kit_portal_guard *guard, const char *session_token) {
  struct iterate_kit_portal_session *found = NULL;
  if (guard == NULL || session_token == NULL) return NULL;
  const size_t token_len = strlen(session_token);
  if (token_len != ITERATE_KIT_SESSION_TOKEN_CAPACITY - 1U) return NULL;
  for (size_t i = 0U; i < ITERATE_KIT_PORTAL_MAX_SESSIONS; ++i) {
    struct iterate_kit_portal_session *s = &guard->sessions[i];
    if (s->active &&
        constant_time_equal(session_token, s->session_token, token_len)) {
      found = s;
    }
  }
  return found;
}

static const struct iterate_kit_portal_session *find_session_const(
    const struct iterate_kit_portal_guard *guard, const char *session_token) {
  const struct iterate_kit_portal_session *found = NULL;
  if (guard == NULL || session_token == NULL) return NULL;
  const size_t token_len = strlen(session_token);
  if (token_len != ITERATE_KIT_SESSION_TOKEN_CAPACITY - 1U) return NULL;
  for (size_t i = 0U; i < ITERATE_KIT_PORTAL_MAX_SESSIONS; ++i) {
    const struct iterate_kit_portal_session *s = &guard->sessions[i];
    if (s->active &&
        constant_time_equal(session_token, s->session_token, token_len)) {
      found = s;
    }
  }
  return found;
}

static bool session_is_pending(
    const struct iterate_kit_portal_guard *guard,
    const struct iterate_kit_portal_session *session) {
  if (guard->claim_state != ITERATE_KIT_CLAIM_PENDING_CONFIRM ||
      !session->active ||
      guard->pending_session_token[0] == '\0') {
    return false;
  }
  return constant_time_equal(
      session->session_token,
      guard->pending_session_token,
      ITERATE_KIT_SESSION_TOKEN_CAPACITY - 1U);
}

static bool csrf_matches_session(
    const struct iterate_kit_portal_session *session, const char *csrf_submitted) {
  if (session == NULL || csrf_submitted == NULL) return false;
  const size_t expected_len = strlen(session->csrf_token);
  return expected_len == ITERATE_KIT_CSRF_TOKEN_CAPACITY - 1U &&
      strlen(csrf_submitted) == expected_len &&
      constant_time_equal(csrf_submitted, session->csrf_token, expected_len);
}

bool iterate_kit_portal_guard_session_exists(
    const struct iterate_kit_portal_guard *guard, const char *session_token) {
  return find_session_const(guard, session_token) != NULL;
}

bool iterate_kit_portal_guard_ensure_session(
    struct iterate_kit_portal_guard *guard,
    const char *cookie_session_token,
    const uint8_t new_session_entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    const uint8_t new_csrf_entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    uint64_t now_ms,
    char out_session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY],
    char out_csrf_token[ITERATE_KIT_CSRF_TOKEN_CAPACITY],
    bool *out_created_new) {
  struct iterate_kit_portal_session *slot;
  if (out_created_new != NULL) *out_created_new = false;
  if (guard == NULL || new_session_entropy == NULL || new_csrf_entropy == NULL ||
      out_session_token == NULL || out_csrf_token == NULL) {
    return false;
  }

  expire_state(guard, now_ms);
  slot = find_session_mut(guard, cookie_session_token);
  if (slot != NULL) {
    slot->last_seen_ms = now_ms;
    memcpy(out_session_token, slot->session_token, ITERATE_KIT_SESSION_TOKEN_CAPACITY);
    memcpy(out_csrf_token, slot->csrf_token, ITERATE_KIT_CSRF_TOKEN_CAPACITY);
    return true;
  }

  /* Eviction priority: inactive -> oldest locked & non-pending -> oldest locked -> oldest. */
  struct iterate_kit_portal_session *victim = NULL;
  for (size_t i = 0U; i < ITERATE_KIT_PORTAL_MAX_SESSIONS; ++i) {
    if (!guard->sessions[i].active) {
      victim = &guard->sessions[i];
      break;
    }
  }
  if (victim == NULL) {
    for (size_t i = 0U; i < ITERATE_KIT_PORTAL_MAX_SESSIONS; ++i) {
      struct iterate_kit_portal_session *s = &guard->sessions[i];
      if (!s->unlocked && !session_is_pending(guard, s) &&
          (victim == NULL || s->last_seen_ms < victim->last_seen_ms)) {
        victim = s;
      }
    }
  }
  if (victim == NULL) {
    for (size_t i = 0U; i < ITERATE_KIT_PORTAL_MAX_SESSIONS; ++i) {
      struct iterate_kit_portal_session *s = &guard->sessions[i];
      if (!s->unlocked && (victim == NULL || s->last_seen_ms < victim->last_seen_ms)) {
        victim = s;
      }
    }
  }
  if (victim == NULL) {
    victim = &guard->sessions[0];
    for (size_t i = 1U; i < ITERATE_KIT_PORTAL_MAX_SESSIONS; ++i) {
      if (guard->sessions[i].last_seen_ms < victim->last_seen_ms) {
        victim = &guard->sessions[i];
      }
    }
  }
  if (session_is_pending(guard, victim)) {
    iterate_kit_portal_guard_cancel_claim(guard);
  }

  memset(victim, 0, sizeof(*victim));
  victim->active = true;
  iterate_kit_session_token_format(new_session_entropy, victim->session_token);
  iterate_kit_csrf_token_format(new_csrf_entropy, victim->csrf_token);
  victim->last_seen_ms = now_ms;
  victim->unlocked = false;
  victim->unlocked_until_ms = 0U;

  memcpy(out_session_token, victim->session_token, ITERATE_KIT_SESSION_TOKEN_CAPACITY);
  memcpy(out_csrf_token, victim->csrf_token, ITERATE_KIT_CSRF_TOKEN_CAPACITY);
  if (out_created_new != NULL) *out_created_new = true;
  return true;
}

void iterate_kit_portal_guard_open_claim(
    struct iterate_kit_portal_guard *guard, uint64_t now_ms) {
  if (guard == NULL || guard->writes_disabled) return;
  iterate_kit_portal_guard_cancel_claim(guard);
  guard->claim_state = ITERATE_KIT_CLAIM_OPEN;
  guard->claim_expires_at_ms = now_ms + (uint64_t)ITERATE_KIT_PORTAL_CLAIM_TTL_MS;
}

enum iterate_kit_portal_auth_result iterate_kit_portal_guard_request_claim(
    struct iterate_kit_portal_guard *guard,
    const char *session_token,
    const char *csrf_submitted,
    uint8_t pattern_entropy_byte,
    const uint8_t rotated_session_entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    const uint8_t rotated_csrf_entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    uint64_t now_ms,
    char out_new_session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY],
    struct iterate_kit_claim_pattern *out_pattern) {
  if (out_new_session_token != NULL) out_new_session_token[0] = '\0';
  if (out_pattern != NULL) memset(out_pattern, 0, sizeof(*out_pattern));
  if (guard == NULL) return ITERATE_KIT_PORTAL_AUTH_NO_SESSION;
  if (guard->writes_disabled) return ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED;

  expire_state(guard, now_ms);
  struct iterate_kit_portal_session *slot = find_session_mut(guard, session_token);
  if (slot == NULL) return ITERATE_KIT_PORTAL_AUTH_NO_SESSION;
  if (!csrf_matches_session(slot, csrf_submitted)) {
    return ITERATE_KIT_PORTAL_AUTH_BAD_CSRF;
  }
  if (guard->claim_state == ITERATE_KIT_CLAIM_NONE) {
    return ITERATE_KIT_PORTAL_AUTH_NO_CLAIM;
  }
  if (guard->claim_state == ITERATE_KIT_CLAIM_PENDING_CONFIRM) {
    return ITERATE_KIT_PORTAL_AUTH_CLAIM_BUSY;
  }
  if (rotated_session_entropy == NULL || rotated_csrf_entropy == NULL ||
      out_new_session_token == NULL || out_pattern == NULL) {
    return ITERATE_KIT_PORTAL_AUTH_NO_SESSION;
  }

  /* Rotate session & CSRF tokens, bind pending claim to this slot, keep locked until Tap 2. */
  iterate_kit_session_token_format(rotated_session_entropy, slot->session_token);
  iterate_kit_csrf_token_format(rotated_csrf_entropy, slot->csrf_token);
  slot->last_seen_ms = now_ms;

  guard->pending_pattern.color =
      (enum iterate_kit_claim_color)(pattern_entropy_byte & 0x03U);
  guard->pending_pattern.position =
      (enum iterate_kit_claim_position)((pattern_entropy_byte >> 2U) & 0x03U);
  guard->claim_state = ITERATE_KIT_CLAIM_PENDING_CONFIRM;
  guard->claim_expires_at_ms = now_ms + (uint64_t)ITERATE_KIT_PORTAL_CLAIM_TTL_MS;
  memcpy(
      guard->pending_session_token, slot->session_token, ITERATE_KIT_SESSION_TOKEN_CAPACITY);

  memcpy(out_new_session_token, slot->session_token, ITERATE_KIT_SESSION_TOKEN_CAPACITY);
  *out_pattern = guard->pending_pattern;
  return ITERATE_KIT_PORTAL_AUTH_OK;
}

bool iterate_kit_portal_guard_confirm_claim(
    struct iterate_kit_portal_guard *guard, uint64_t now_ms) {
  if (guard == NULL || guard->writes_disabled) return false;
  expire_state(guard, now_ms);
  if (guard->claim_state != ITERATE_KIT_CLAIM_PENDING_CONFIRM) return false;

  struct iterate_kit_portal_session *slot =
      find_session_mut(guard, guard->pending_session_token);
  iterate_kit_portal_guard_cancel_claim(guard);
  if (slot == NULL) return false;

  slot->unlocked = true;
  slot->unlocked_until_ms = now_ms + (uint64_t)ITERATE_KIT_PORTAL_UNLOCK_MS;
  slot->last_seen_ms = now_ms;
  return true;
}

enum iterate_kit_portal_auth_result iterate_kit_portal_guard_cancel_claim_by_session(
    struct iterate_kit_portal_guard *guard,
    const char *session_token,
    const char *csrf_submitted,
    uint64_t now_ms) {
  if (guard == NULL) return ITERATE_KIT_PORTAL_AUTH_NO_SESSION;
  if (guard->writes_disabled) return ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED;

  expire_state(guard, now_ms);
  struct iterate_kit_portal_session *slot = find_session_mut(guard, session_token);
  if (slot == NULL) return ITERATE_KIT_PORTAL_AUTH_NO_SESSION;
  if (!csrf_matches_session(slot, csrf_submitted)) {
    return ITERATE_KIT_PORTAL_AUTH_BAD_CSRF;
  }
  if (guard->claim_state == ITERATE_KIT_CLAIM_PENDING_CONFIRM &&
      session_is_pending(guard, slot)) {
    iterate_kit_portal_guard_cancel_claim(guard);
    return ITERATE_KIT_PORTAL_AUTH_OK;
  }
  return ITERATE_KIT_PORTAL_AUTH_NO_CLAIM;
}

enum iterate_kit_claim_state iterate_kit_portal_guard_claim_state(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms) {
  if (guard == NULL || guard->claim_state == ITERATE_KIT_CLAIM_NONE ||
      now_ms >= guard->claim_expires_at_ms) {
    return ITERATE_KIT_CLAIM_NONE;
  }
  return guard->claim_state;
}

uint32_t iterate_kit_portal_guard_claim_remaining_s(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms) {
  if (iterate_kit_portal_guard_claim_state(guard, now_ms) == ITERATE_KIT_CLAIM_NONE) {
    return 0U;
  }
  return (uint32_t)((guard->claim_expires_at_ms - now_ms + 999U) / 1000U);
}

bool iterate_kit_portal_guard_active_claim_pattern(
    const struct iterate_kit_portal_guard *guard,
    uint64_t now_ms,
    struct iterate_kit_claim_pattern *out_pattern) {
  if (iterate_kit_portal_guard_claim_state(guard, now_ms) !=
      ITERATE_KIT_CLAIM_PENDING_CONFIRM) {
    return false;
  }
  if (out_pattern != NULL) *out_pattern = guard->pending_pattern;
  return true;
}

bool iterate_kit_portal_guard_session_has_pending_claim(
    const struct iterate_kit_portal_guard *guard,
    const char *session_token,
    uint64_t now_ms,
    struct iterate_kit_claim_pattern *out_pattern) {
  if (iterate_kit_portal_guard_claim_state(guard, now_ms) !=
      ITERATE_KIT_CLAIM_PENDING_CONFIRM) {
    return false;
  }
  const struct iterate_kit_portal_session *slot = find_session_const(guard, session_token);
  if (slot == NULL || !session_is_pending(guard, slot)) return false;
  if (out_pattern != NULL) *out_pattern = guard->pending_pattern;
  return true;
}

bool iterate_kit_portal_guard_session_is_unlocked(
    const struct iterate_kit_portal_guard *guard,
    const char *session_token,
    uint64_t now_ms) {
  const struct iterate_kit_portal_session *slot = find_session_const(guard, session_token);
  return slot != NULL && slot->unlocked && now_ms < slot->unlocked_until_ms;
}

uint32_t iterate_kit_portal_guard_session_unlock_remaining_s(
    const struct iterate_kit_portal_guard *guard,
    const char *session_token,
    uint64_t now_ms) {
  const struct iterate_kit_portal_session *slot = find_session_const(guard, session_token);
  if (slot == NULL || !slot->unlocked || now_ms >= slot->unlocked_until_ms) {
    return 0U;
  }
  return (uint32_t)((slot->unlocked_until_ms - now_ms + 999U) / 1000U);
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
    const char *session_token,
    const char *csrf_submitted,
    uint64_t now_ms) {
  if (guard == NULL) return ITERATE_KIT_PORTAL_AUTH_NO_SESSION;
  if (guard->writes_disabled) return ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED;

  expire_state(guard, now_ms);
  struct iterate_kit_portal_session *slot = find_session_mut(guard, session_token);
  if (slot == NULL) return ITERATE_KIT_PORTAL_AUTH_NO_SESSION;
  if (!csrf_matches_session(slot, csrf_submitted)) {
    return ITERATE_KIT_PORTAL_AUTH_BAD_CSRF;
  }
  if (guard->is_setup_ap) {
    slot->last_seen_ms = now_ms;
    return ITERATE_KIT_PORTAL_AUTH_OK;
  }
  if (slot->unlocked && now_ms < slot->unlocked_until_ms) {
    /* Never extend unlocked_until_ms on ordinary POSTs. */
    slot->last_seen_ms = now_ms;
    return ITERATE_KIT_PORTAL_AUTH_OK;
  }
  return ITERATE_KIT_PORTAL_AUTH_REQUIRED;
}
