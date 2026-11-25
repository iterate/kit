#include "iterate/kit/web_portal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool iterate_kit_web_action_allowed(
    enum iterate_kit_boot_mode mode, enum iterate_kit_web_action action) {
  switch (action) {
    case ITERATE_KIT_WEB_ACTION_WIFI:
    case ITERATE_KIT_WEB_ACTION_MIND:
      /* Credentials (Wi-Fi password, OAuth refresh token, project API key) are
       * accepted ONLY on the board's own WPA2-PSK SoftAP, never over plain-HTTP LAN. */
      return mode == ITERATE_KIT_BOOT_MODE_SETUP_AP;
    case ITERATE_KIT_WEB_ACTION_SETTINGS:
    case ITERATE_KIT_WEB_ACTION_REBOOT:
      return true;
    case ITERATE_KIT_WEB_ACTION_ENTER_SETUP:
    case ITERATE_KIT_WEB_ACTION_RESET_WIFI:
      return mode != ITERATE_KIT_BOOT_MODE_SETUP_AP;
  }
  return false;
}

bool iterate_kit_web_cookie_get(
    const char *cookie_header,
    const char *name,
    char *destination,
    size_t capacity) {
  size_t name_len;
  const char *cursor;
  if (destination != NULL && capacity > 0U) destination[0] = '\0';
  if (cookie_header == NULL || name == NULL || name[0] == '\0' ||
      destination == NULL || capacity == 0U) {
    return false;
  }
  name_len = strlen(name);
  cursor = cookie_header;
  while (*cursor != '\0') {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ';') ++cursor;
    if (*cursor == '\0') break;
    const char *pair_start = cursor;
    while (*cursor != '\0' && *cursor != ';') ++cursor;
    const char *pair_end = cursor;
    const char *eq = memchr(pair_start, '=', (size_t)(pair_end - pair_start));
    if (eq != NULL) {
      const size_t key_len = (size_t)(eq - pair_start);
      if (key_len == name_len && memcmp(pair_start, name, name_len) == 0) {
        const char *val_start = eq + 1;
        const char *val_end = pair_end;
        while (val_start < val_end && (*val_start == ' ' || *val_start == '\t')) {
          ++val_start;
        }
        while (val_end > val_start && (val_end[-1] == ' ' || val_end[-1] == '\t')) {
          --val_end;
        }
        const size_t val_len = (size_t)(val_end - val_start);
        if (val_len >= capacity) return false;
        memcpy(destination, val_start, val_len);
        destination[val_len] = '\0';
        return true;
      }
    }
  }
  return false;
}

bool iterate_kit_web_is_captive_probe(const char *path) {
  static const char *const probes[] = {
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/generate_204",
    "/gen_204",
    "/connecttest.txt",
    "/ncsi.txt",
    "/redirect",
    "/fwlink",
    "/canonical.html",
    "/success.txt",
    "/kindle-wifi/wifistub.html",
  };
  if (path == NULL) return false;
  for (size_t index = 0U; index < sizeof(probes) / sizeof(probes[0]); ++index) {
    const size_t len = strlen(probes[index]);
    if (strncmp(path, probes[index], len) == 0 &&
        (path[len] == '\0' || path[len] == '?')) {
      return true;
    }
  }
  return false;
}

static bool decode_urlencoded(
    const char *src, size_t src_len, char *dst, size_t dst_cap,
    enum iterate_kit_web_field_status *status) {
  size_t out = 0U;
  for (size_t index = 0U; index < src_len; ++index) {
    char c = src[index];
    if (c == '+') {
      c = ' ';
    } else if (c == '%') {
      if (index + 2U >= src_len) {
        *status = ITERATE_KIT_WEB_FIELD_INVALID;
        return false;
      }
      const int hi = hex_value(src[index + 1U]);
      const int lo = hex_value(src[index + 2U]);
      if (hi < 0 || lo < 0) {
        *status = ITERATE_KIT_WEB_FIELD_INVALID;
        return false;
      }
      const uint8_t byte = (uint8_t)((hi << 4) | lo);
      if (byte == 0U) {
        *status = ITERATE_KIT_WEB_FIELD_INVALID;
        return false;
      }
      c = (char)byte;
      index += 2U;
    } else if (c == '\0') {
      *status = ITERATE_KIT_WEB_FIELD_INVALID;
      return false;
    }
    if (out + 1U >= dst_cap) {
      *status = ITERATE_KIT_WEB_FIELD_TOO_LONG;
      return false;
    }
    dst[out++] = c;
  }
  dst[out] = '\0';
  *status = ITERATE_KIT_WEB_FIELD_OK;
  return true;
}

enum iterate_kit_web_field_status iterate_kit_web_form_get_field(
    const char *body,
    size_t body_length,
    const char *field_name,
    char *destination,
    size_t capacity) {
  size_t name_len;
  size_t cursor = 0U;
  if (destination != NULL && capacity > 0U) destination[0] = '\0';
  if (field_name == NULL || field_name[0] == '\0' ||
      destination == NULL || capacity == 0U) {
    return ITERATE_KIT_WEB_FIELD_INVALID;
  }
  if (body == NULL || body_length == 0U) {
    return ITERATE_KIT_WEB_FIELD_MISSING;
  }
  name_len = strlen(field_name);
  while (cursor <= body_length) {
    const char *pair_start = body + cursor;
    size_t pair_len = 0U;
    while (cursor + pair_len < body_length && pair_start[pair_len] != '&') {
      ++pair_len;
    }
    if (pair_len > 0U) {
      const char *eq = memchr(pair_start, '=', pair_len);
      const size_t key_len = eq != NULL ? (size_t)(eq - pair_start) : pair_len;
      if (key_len == name_len && memcmp(pair_start, field_name, name_len) == 0) {
        const char *val = eq != NULL ? eq + 1 : "";
        const size_t val_len = eq != NULL ? pair_len - key_len - 1U : 0U;
        enum iterate_kit_web_field_status status = ITERATE_KIT_WEB_FIELD_INVALID;
        if (!decode_urlencoded(val, val_len, destination, capacity, &status)) {
          destination[0] = '\0';
        }
        return status;
      }
    }
    if (cursor + pair_len >= body_length) break;
    cursor += pair_len + 1U;
  }
  return ITERATE_KIT_WEB_FIELD_MISSING;
}

size_t iterate_kit_web_html_escape(
    const char *input, char *destination, size_t capacity) {
  size_t out = 0U;
  if (destination == NULL || capacity == 0U) return 0U;
  destination[0] = '\0';
  if (input == NULL) return 0U;
  for (const char *cursor = input; *cursor != '\0'; ++cursor) {
    const char *rep = NULL;
    switch (*cursor) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      case '\'': rep = "&#39;"; break;
      default: break;
    }
    if (rep != NULL) {
      const size_t len = strlen(rep);
      if (out + len >= capacity) break;
      memcpy(destination + out, rep, len);
      out += len;
    } else {
      if (out + 1U >= capacity) break;
      destination[out++] = *cursor;
    }
  }
  destination[out] = '\0';
  return out;
}

static void trim_whitespace(char *text) {
  size_t start = 0U;
  size_t len = strlen(text);
  while (start < len &&
         (text[start] == ' ' || text[start] == '\t' ||
          text[start] == '\r' || text[start] == '\n')) {
    ++start;
  }
  while (len > start &&
         (text[len - 1U] == ' ' || text[len - 1U] == '\t' ||
          text[len - 1U] == '\r' || text[len - 1U] == '\n')) {
    --len;
  }
  if (start > 0U && len > start) {
    memmove(text, text + start, len - start);
  }
  text[len - start] = '\0';
}

static void set_result(
    struct iterate_kit_portal_action_result *result, bool ok, const char *message) {
  memset(result, 0, sizeof(*result));
  result->ok = ok;
  (void)snprintf(result->message, sizeof(result->message), "%s", message);
}

void iterate_kit_web_apply_wifi_form(
    const char *body,
    size_t body_length,
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence,
    struct iterate_kit_portal_action_result *result) {
  char ssid[ITERATE_KIT_WIFI_SSID_CAPACITY];
  char password[ITERATE_KIT_WIFI_PASSWORD_CAPACITY];
  enum iterate_kit_web_field_status status;

  if (result == NULL) return;
  if (configuration == NULL || presence == NULL) {
    set_result(result, false, "Internal error: missing configuration storage.");
    return;
  }
  status = iterate_kit_web_form_get_field(body, body_length, "ssid", ssid, sizeof(ssid));
  if (status == ITERATE_KIT_WEB_FIELD_TOO_LONG) {
    set_result(result, false, "Wi-Fi network name is longer than 32 bytes.");
    return;
  }
  if (status != ITERATE_KIT_WEB_FIELD_OK || ssid[0] == '\0') {
    set_result(result, false, "Enter a 2.4 GHz Wi-Fi network name.");
    return;
  }
  status = iterate_kit_web_form_get_field(
      body, body_length, "password", password, sizeof(password));
  if (status == ITERATE_KIT_WEB_FIELD_MISSING) {
    password[0] = '\0';
  } else if (status == ITERATE_KIT_WEB_FIELD_TOO_LONG) {
    set_result(result, false, "Wi-Fi password is longer than 64 bytes.");
    return;
  } else if (status != ITERATE_KIT_WEB_FIELD_OK) {
    set_result(result, false, "Wi-Fi password contains an invalid character.");
    return;
  }
  if (!iterate_kit_configuration_valid_wifi_password(password)) {
    set_result(
        result, false,
        "Wi-Fi password must be empty (open network), 8-63 characters, or 64 hex digits.");
    return;
  }

  memcpy(configuration->wifi_ssid, ssid, sizeof(configuration->wifi_ssid));
  memcpy(configuration->wifi_password, password, sizeof(configuration->wifi_password));
  presence->has_wifi = true;
  memset(password, 0, sizeof(password));

  set_result(result, true, "Wi-Fi saved. Restarting to join your network.");
  result->config_changed = true;
  result->reboot_needed = true;
}

void iterate_kit_web_apply_mind_form(
    const char *body,
    size_t body_length,
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence,
    struct iterate_kit_portal_action_result *result) {
  char mode[16];
  struct iterate_kit_configuration candidate;
  struct iterate_kit_configuration_presence candidate_presence;
  uint8_t check_buf[1024];

  if (result == NULL) return;
  if (configuration == NULL || presence == NULL) {
    set_result(result, false, "Internal error: missing configuration storage.");
    return;
  }
  candidate = *configuration;
  candidate_presence = *presence;

  if (iterate_kit_web_form_get_field(body, body_length, "mode", mode, sizeof(mode)) !=
      ITERATE_KIT_WEB_FIELD_OK) {
    (void)snprintf(mode, sizeof(mode), "mind");
  }

  if (strcmp(mode, "project") == 0) {
    char raw_url[ITERATE_KIT_OS_BASE_URL_CAPACITY];
    char normalized_url[ITERATE_KIT_OS_BASE_URL_CAPACITY];
    char project_id[ITERATE_KIT_PROJECT_ID_CAPACITY];
    char api_key[ITERATE_KIT_PROJECT_API_KEY_CAPACITY];
    if (iterate_kit_web_form_get_field(
            body, body_length, "project_url", raw_url, sizeof(raw_url)) !=
            ITERATE_KIT_WEB_FIELD_OK ||
        iterate_kit_configuration_normalize_mind_origin(
            raw_url, normalized_url, sizeof(normalized_url)) != ITERATE_KIT_CONFIGURATION_OK) {
      set_result(result, false, "Enter a valid platform origin (for example https://os.example.com).");
      return;
    }
    if (iterate_kit_web_form_get_field(
            body, body_length, "project_id", project_id, sizeof(project_id)) !=
            ITERATE_KIT_WEB_FIELD_OK) {
      set_result(result, false, "Enter a valid project ID.");
      return;
    }
    trim_whitespace(project_id);
    if (iterate_kit_web_form_get_field(
            body, body_length, "project_api_key", api_key, sizeof(api_key)) !=
            ITERATE_KIT_WEB_FIELD_OK) {
      set_result(result, false, "Enter a project API key.");
      return;
    }
    trim_whitespace(api_key);
    memset(candidate.oauth_client_id, 0, sizeof(candidate.oauth_client_id));
    memset(candidate.oauth_refresh_token, 0, sizeof(candidate.oauth_refresh_token));
    (void)snprintf(candidate.os_base_url, sizeof(candidate.os_base_url), "%s", normalized_url);
    (void)snprintf(candidate.project_id, sizeof(candidate.project_id), "%s", project_id);
    (void)snprintf(candidate.project_api_key, sizeof(candidate.project_api_key), "%s", api_key);
    candidate.platform = ITERATE_KIT_CONFIGURATION_PLATFORM_PROJECT;
    candidate_presence.has_platform = true;
    memset(api_key, 0, sizeof(api_key));
  } else {
    char raw_url[ITERATE_KIT_OS_BASE_URL_CAPACITY];
    char normalized_url[ITERATE_KIT_OS_BASE_URL_CAPACITY];
    char client_id[ITERATE_KIT_OAUTH_CLIENT_ID_CAPACITY];
    char refresh_token[ITERATE_KIT_OAUTH_TOKEN_CAPACITY];
    enum iterate_kit_web_field_status status;

    status = iterate_kit_web_form_get_field(
        body, body_length, "mind_url", raw_url, sizeof(raw_url));
    if (status != ITERATE_KIT_WEB_FIELD_OK ||
        iterate_kit_configuration_normalize_mind_origin(
            raw_url, normalized_url, sizeof(normalized_url)) != ITERATE_KIT_CONFIGURATION_OK) {
      set_result(
          result, false,
          "Enter a valid mind address (for example example.com or https://iterate.example.com).");
      return;
    }
    status = iterate_kit_web_form_get_field(
        body, body_length, "client_id", client_id, sizeof(client_id));
    if (status == ITERATE_KIT_WEB_FIELD_TOO_LONG) {
      set_result(result, false, "OAuth client ID is longer than 192 bytes.");
      return;
    }
    if (status != ITERATE_KIT_WEB_FIELD_OK) {
      set_result(result, false, "Enter the OAuth client ID URL.");
      return;
    }
    trim_whitespace(client_id);
    status = iterate_kit_web_form_get_field(
        body, body_length, "refresh_token", refresh_token, sizeof(refresh_token));
    if (status == ITERATE_KIT_WEB_FIELD_TOO_LONG) {
      set_result(result, false, "Refresh token is longer than 160 bytes.");
      return;
    }
    if (status != ITERATE_KIT_WEB_FIELD_OK) {
      set_result(result, false, "Enter the OAuth refresh token.");
      return;
    }
    trim_whitespace(refresh_token);

    memset(candidate.project_id, 0, sizeof(candidate.project_id));
    memset(candidate.project_api_key, 0, sizeof(candidate.project_api_key));
    (void)snprintf(candidate.os_base_url, sizeof(candidate.os_base_url), "%s", normalized_url);
    (void)snprintf(candidate.oauth_client_id, sizeof(candidate.oauth_client_id), "%s", client_id);
    (void)snprintf(
        candidate.oauth_refresh_token, sizeof(candidate.oauth_refresh_token), "%s", refresh_token);
    candidate.platform = ITERATE_KIT_CONFIGURATION_PLATFORM_MIND;
    candidate_presence.has_platform = true;
    memset(refresh_token, 0, sizeof(refresh_token));
  }

  /* Validate the full encoded image before mutating caller state. */
  if (iterate_kit_configuration_encode(
          &candidate, &candidate_presence, check_buf, sizeof(check_buf), NULL) !=
      ITERATE_KIT_CONFIGURATION_OK) {
    memset(&candidate, 0, sizeof(candidate));
    memset(check_buf, 0, sizeof(check_buf));
    set_result(
        result, false,
        "Mind credential is invalid (check origin, client ID URL, and token).");
    return;
  }
  memset(check_buf, 0, sizeof(check_buf));

  *configuration = candidate;
  *presence = candidate_presence;
  memset(&candidate, 0, sizeof(candidate));

  if (presence->has_wifi) {
    set_result(result, true, "Mind connection saved. Restarting to connect.");
    result->reboot_needed = true;
  } else {
    set_result(result, true, "Mind connection saved. Now enter your 2.4 GHz Wi-Fi below to connect.");
    result->reboot_needed = false;
  }
  result->config_changed = true;
  result->clear_refresh_token = true;
}

void iterate_kit_web_apply_settings_form(
    const char *body,
    size_t body_length,
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence,
    struct iterate_kit_portal_action_result *result) {
  char voice_str[32];
  char vol_str[8];
  bool voice_changed = false;
  bool volume_present = false;
  uint8_t volume_value = 0U;

  if (result == NULL) return;
  if (configuration == NULL || presence == NULL) {
    set_result(result, false, "Internal error: missing configuration storage.");
    return;
  }

  enum iterate_kit_web_field_status voice_status =
      iterate_kit_web_form_get_field(
          body, body_length, "status_voice", voice_str, sizeof(voice_str));
  enum iterate_kit_status_voice parsed_voice =
      (enum iterate_kit_status_voice)configuration->status_voice;
  if (voice_status == ITERATE_KIT_WEB_FIELD_OK) {
    if (!iterate_kit_configuration_parse_status_voice(voice_str, &parsed_voice)) {
      set_result(result, false, "Unknown status voice choice.");
      return;
    }
    voice_changed = !presence->has_status_voice ||
        configuration->status_voice != (uint8_t)parsed_voice;
  } else if (voice_status != ITERATE_KIT_WEB_FIELD_MISSING) {
    set_result(result, false, "Invalid status voice value.");
    return;
  }

  enum iterate_kit_web_field_status vol_status =
      iterate_kit_web_form_get_field(body, body_length, "volume", vol_str, sizeof(vol_str));
  if (vol_status == ITERATE_KIT_WEB_FIELD_OK && vol_str[0] != '\0') {
    unsigned int parsed = 0U;
    for (size_t i = 0U; vol_str[i] != '\0'; ++i) {
      if (vol_str[i] < '0' || vol_str[i] > '9' || i >= 3U) {
        set_result(result, false, "Volume must be a number from 0 to 100.");
        return;
      }
      parsed = parsed * 10U + (unsigned int)(vol_str[i] - '0');
    }
    if (parsed > 100U) {
      set_result(result, false, "Volume must be between 0 and 100.");
      return;
    }
    volume_present = true;
    volume_value = (uint8_t)parsed;
  } else if (vol_status != ITERATE_KIT_WEB_FIELD_MISSING &&
             vol_status != ITERATE_KIT_WEB_FIELD_OK) {
    set_result(result, false, "Invalid volume value.");
    return;
  }

  if (voice_status == ITERATE_KIT_WEB_FIELD_MISSING && !volume_present) {
    set_result(result, false, "No settings were submitted.");
    return;
  }

  if (voice_status == ITERATE_KIT_WEB_FIELD_OK) {
    configuration->status_voice = (uint8_t)parsed_voice;
    presence->has_status_voice = true;
  }

  set_result(
      result, true,
      voice_changed ? "Settings saved. Restarting to apply status voice."
                    : "Settings saved.");
  result->config_changed = voice_changed;
  result->reboot_needed = voice_changed;
  result->apply_volume = volume_present;
  result->new_volume = volume_value;
}

/* --- bounded string builder ----------------------------------------------- */

struct writer {
  char *buf;
  size_t cap;
  size_t len;
  bool overflow;
};

static void w_puts(struct writer *w, const char *s) {
  if (w->overflow || s == NULL) return;
  const size_t n = strlen(s);
  if (n >= w->cap - w->len) {
    w->overflow = true;
    return;
  }
  memcpy(w->buf + w->len, s, n);
  w->len += n;
  w->buf[w->len] = '\0';
}

static void w_printf(struct writer *w, const char *fmt, ...) {
  if (w->overflow) return;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= w->cap - w->len) {
    w->overflow = true;
    return;
  }
  w->len += (size_t)n;
}

static void w_html(struct writer *w, const char *s) {
  if (w->overflow || s == NULL) return;
  for (const char *c = s; *c != '\0'; ++c) {
    switch (*c) {
      case '&': w_puts(w, "&amp;"); break;
      case '<': w_puts(w, "&lt;"); break;
      case '>': w_puts(w, "&gt;"); break;
      case '"': w_puts(w, "&quot;"); break;
      case '\'': w_puts(w, "&#39;"); break;
      default:
        if (w->len + 1U >= w->cap) {
          w->overflow = true;
          return;
        }
        w->buf[w->len++] = *c;
        w->buf[w->len] = '\0';
        break;
    }
  }
}

static void w_json(struct writer *w, const char *s) {
  if (w->overflow || s == NULL) return;
  for (const unsigned char *c = (const unsigned char *)s; *c != '\0'; ++c) {
    if (*c == '"' || *c == '\\') {
      w_printf(w, "\\%c", (char)*c);
    } else if (*c < 0x20U) {
      w_printf(w, "\\u%04x", (unsigned)*c);
    } else {
      if (w->len + 1U >= w->cap) {
        w->overflow = true;
        return;
      }
      w->buf[w->len++] = (char)*c;
      w->buf[w->len] = '\0';
    }
  }
}

static const char *rssi_label(int8_t rssi) {
  if (rssi >= -55) return "Excellent";
  if (rssi >= -67) return "Good";
  if (rssi >= -75) return "Fair";
  return "Weak";
}

size_t iterate_kit_web_render_page(
    const struct iterate_kit_configuration *configuration,
    const struct iterate_kit_configuration_presence *presence,
    const struct iterate_kit_portal_status *status,
    const char *csrf_token,
    char *out,
    size_t capacity) {
  static const struct {
    enum iterate_kit_status_voice value;
    const char *label;
  } voices[] = {
    {ITERATE_KIT_STATUS_VOICE_GREENSLEEVES, "Greensleeves"},
    {ITERATE_KIT_STATUS_VOICE_DAISY_BELL, "Daisy Bell"},
    {ITERATE_KIT_STATUS_VOICE_AULD_LANG_SYNE, "Auld Lang Syne"},
    {ITERATE_KIT_STATUS_VOICE_LASS_OF_AUGHRIM, "The Lass of Aughrim"},
    {ITERATE_KIT_STATUS_VOICE_SPOKEN, "Spoken"},
    {ITERATE_KIT_STATUS_VOICE_OFF, "Off"},
  };
  struct writer w = {out, capacity, 0U, false};
  if (configuration == NULL || presence == NULL || status == NULL ||
      csrf_token == NULL || out == NULL || capacity < 2U) {
    if (out != NULL && capacity > 0U) out[0] = '\0';
    return 0U;
  }
  out[0] = '\0';

  const bool setup_ap = status->mode == ITERATE_KIT_BOOT_MODE_SETUP_AP;
  const bool lan_unlocked = setup_ap || status->unlocked_remaining_s > 0U;

  w_puts(&w,
      "<!doctype html><html lang=en><head><meta charset=utf-8>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>");
  w_html(&w, status->hostname[0] ? status->hostname : "iterate");
  w_puts(&w,
      "</title><style>"
      "body{font-family:system-ui,-apple-system,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#f6f7f9;color:#111}"
      "h1{font-size:1.25rem;margin:0 0 4px}h2{font-size:1rem;margin:0 0 10px}"
      ".sub{color:#555;font-size:.875rem;margin:0 0 14px}"
      ".card{background:#fff;border:1px solid #e2e5ea;border-radius:10px;padding:14px;margin:0 0 12px}"
      ".row{display:flex;justify-content:space-between;gap:8px;padding:5px 0;border-bottom:1px solid #f0f2f5;font-size:.9rem}"
      ".row:last-child{border-bottom:0}.k{color:#555}.v{font-weight:600;text-align:right;word-break:break-word}"
      "label{display:block;font-size:.85rem;font-weight:600;margin:8px 0 3px}"
      "input,select,button{width:100%;box-sizing:border-box;padding:9px 10px;border:1px solid #ccd2da;border-radius:7px;font-size:.95rem}"
      "button{background:#111;color:#fff;border:0;font-weight:600;margin-top:10px;cursor:pointer}"
      "button.sec{background:#eef1f5;color:#111;border:1px solid #d5dbe3}"
      "button.warn{background:#b91c1c;color:#fff}"
      ".msg{padding:10px 12px;border-radius:8px;margin:0 0 12px;font-size:.9rem}"
      ".ok{background:#e7f6ec;color:#14532d;border:1px solid #b7e4c7}"
      ".err{background:#fdecec;color:#7f1d1d;border:1px solid #f5c2c2}"
      ".ap{display:flex;justify-content:space-between;align-items:center;padding:7px 9px;margin:4px 0;border:1px solid #e2e5ea;border-radius:7px;background:#fafbfc;color:#111;font-weight:400}"
      ".hint{font-size:.8rem;color:#555;margin:6px 0 0}"
      "</style></head><body>");

  w_puts(&w, "<h1>");
  w_html(&w, status->hostname[0] ? status->hostname : "iterate");
  w_puts(&w, "</h1><p class=sub>");
  if (setup_ap) {
    w_puts(&w, "Encrypted Setup Mode (WPA2 SoftAP) &middot; Configure Wi-Fi and Mind");
  } else {
    w_html(&w, status->hostname);
    w_puts(&w, ".local &middot; Firmware ");
    w_html(&w, status->firmware_version[0] ? status->firmware_version : "dev");
  }
  w_puts(&w, "</p>");

  if (status->banner_message != NULL && status->banner_message[0] != '\0') {
    w_printf(&w, "<div class=\"msg %s\">", status->banner_is_error ? "err" : "ok");
    w_html(&w, status->banner_message);
    w_puts(&w, "</div>");
  }

  /* --- Status card --- */
  w_puts(&w, "<div class=card><h2>Status</h2>");
  w_puts(&w, "<div class=row><span class=k>Connection</span><span class=v>");
  if (status->key_refused) {
    w_puts(&w, "Key refused (reconnect mind in Setup Mode)");
  } else if (status->connection_state[0] != '\0') {
    w_html(&w, status->connection_state);
  } else {
    w_html(&w, iterate_kit_boot_mode_name(status->mode));
  }
  w_puts(&w, "</span></div>");

  w_puts(&w, "<div class=row><span class=k>Wi-Fi</span><span class=v>");
  if (presence->has_wifi) {
    w_html(&w, configuration->wifi_ssid);
    w_puts(&w, status->wifi_connected ? " (connected)" : " (saved)");
  } else {
    w_puts(&w, "Not configured");
  }
  w_puts(&w, "</span></div>");

  if (status->has_rssi) {
    w_printf(
        &w, "<div class=row><span class=k>Signal</span><span class=v>%d dBm (%s)</span></div>",
        (int)status->wifi_rssi, rssi_label(status->wifi_rssi));
  }
  if (status->ip_address[0] != '\0') {
    w_puts(&w, "<div class=row><span class=k>IP</span><span class=v>");
    w_html(&w, status->ip_address);
    w_puts(&w, "</span></div>");
  }

  w_puts(&w, "<div class=row><span class=k>Mind</span><span class=v>");
  if (presence->has_platform) {
    w_html(&w, configuration->os_base_url);
    if (configuration->platform == ITERATE_KIT_CONFIGURATION_PLATFORM_PROJECT &&
        configuration->project_id[0] != '\0') {
      w_puts(&w, " / ");
      w_html(&w, configuration->project_id);
    }
  } else {
    w_puts(&w, "Not linked");
  }
  w_puts(&w, "</span></div>");

  w_puts(&w, "<div class=row><span class=k>Firmware</span><span class=v>");
  w_html(&w, status->firmware_version[0] ? status->firmware_version : "dev");
  w_puts(&w, "</span></div></div>");

  /* --- LAN authorization banner / per-browser unlock or physical claim --- */
  if (!setup_ap) {
    w_puts(&w, "<div class=card><h2>Changes</h2>");
    if (lan_unlocked) {
      w_printf(
          &w,
          "<div class=\"msg ok\">Unlocked in this browser for %u s. You can save changes below.</div>",
          (unsigned)status->unlocked_remaining_s);
    } else {
      if (status->physical_claim_remaining_s > 0U) {
        w_puts(&w,
            "<form method=post action=/claim-unlock>"
            "<input type=hidden name=csrf value=\"");
        w_html(&w, csrf_token);
        w_printf(
            &w,
            "\"><button type=submit>Claim physical unlock (%u s left)</button>"
            "<p class=hint>Button tap detected on the puck. Claim now to unlock this browser for 3 minutes.</p>"
            "</form>",
            (unsigned)status->physical_claim_remaining_s);
      }
      w_puts(&w,
          "<form method=post action=/unlock>"
          "<input type=hidden name=csrf value=\"");
      w_html(&w, csrf_token);
      w_puts(&w,
          "\"><label for=sc>Setup code</label>"
          "<input id=sc name=setup_code autocomplete=off placeholder=\"xxxx-xxxx-xxxx\" required>"
          "<button type=submit>Unlock this browser (3 min)</button>"
          "<p class=hint>Find the code on the card, or slide Mute ON, tap the centre button on the puck, and refresh to claim.</p>"
          "</form>");
    }
    w_puts(&w, "</div>");
  }

  /* --- Credentials cards: rendered ONLY on the encrypted WPA2 SoftAP --- */
  if (setup_ap) {
    w_puts(&w, "<div class=card><h2>Wi-Fi (2.4 GHz)</h2>");
    if (status->scan_results != NULL && status->scan_count > 0U) {
      w_puts(&w, "<p class=hint>Tap a nearby network to fill its name:</p>");
      for (size_t index = 0U; index < status->scan_count; ++index) {
        const struct iterate_kit_wifi_scan_entry *ap = &status->scan_results[index];
        w_puts(&w, "<button type=button class=ap data-ssid=\"");
        w_html(&w, ap->ssid);
        w_puts(&w, "\" onclick=\"document.getElementById('ssid').value=this.dataset.ssid\"><span>");
        w_html(&w, ap->ssid);
        w_printf(
            &w, "</span><span>%d dBm%s</span></button>",
            (int)ap->rssi, ap->open ? " open" : "");
      }
    }
    w_puts(&w, "<form method=post action=/wifi><input type=hidden name=csrf value=\"");
    w_html(&w, csrf_token);
    w_puts(&w, "\"><label for=ssid>Network name (SSID)</label><input id=ssid name=ssid maxlength=32 required value=\"");
    if (presence->has_wifi) w_html(&w, configuration->wifi_ssid);
    w_puts(&w,
        "\"><label for=pw>Password (leave empty for open Wi-Fi)</label>"
        "<input id=pw name=password type=password maxlength=64 autocomplete=off>"
        "<button type=submit>Save Wi-Fi &amp; Connect</button>"
        "<p class=hint>After connecting, open <b>http://");
    w_html(&w, status->hostname);
    w_puts(&w, ".local/</b> from your phone on the same Wi-Fi to check status or adjust settings.</p>"
        "</form>"
        "<form method=get action=/><button class=sec type=submit>Rescan networks</button></form>"
        "</div>");

    w_puts(&w, "<div class=card><h2>Iterate Mind</h2>"
        "<form method=post action=/mind><input type=hidden name=csrf value=\"");
    w_html(&w, csrf_token);
    w_puts(&w,
        "\"><input type=hidden name=mode value=mind>"
        "<label for=mu>Mind address</label>"
        "<input id=mu name=mind_url maxlength=128 required placeholder=\"example.com\" value=\"");
    if (presence->has_platform) w_html(&w, configuration->os_base_url);
    w_puts(&w,
        "\"><label for=cid>OAuth client ID</label>"
        "<input id=cid name=client_id maxlength=192 required placeholder=\"https://k.iterate.com/devices/...\" value=\"");
    if (presence->has_platform &&
        configuration->platform == ITERATE_KIT_CONFIGURATION_PLATFORM_MIND) {
      w_html(&w, configuration->oauth_client_id);
    }
    w_puts(&w,
        "\"><label for=rt>OAuth refresh token</label>"
        "<input id=rt name=refresh_token type=password maxlength=160 required autocomplete=off placeholder=\"");
    w_puts(&w,
        presence->has_platform &&
                configuration->platform == ITERATE_KIT_CONFIGURATION_PLATFORM_MIND
            ? "Saved (hidden) — paste new token to replace"
            : "user:grant:secret");
    w_puts(&w,
        "\"><button type=submit>Save Mind</button>"
        "<p class=hint>Accepted only here over encrypted WPA2 SoftAP (never on plain-HTTP LAN). Stored secrets are never shown back.</p>"
        "</form>");
    if (presence->has_wifi) {
      w_puts(&w, "<form method=post action=/reboot><input type=hidden name=csrf value=\"");
      w_html(&w, csrf_token);
      w_puts(&w, "\"><button class=sec type=submit>Exit Setup Mode &amp; Return to Wi-Fi</button></form>");
    }
    w_puts(&w, "</div>");
  }

  /* --- Basic settings card (non-secret: Status voice + Volume) --- */
  w_puts(&w, "<div class=card><h2>Settings</h2>"
      "<form method=post action=/settings><input type=hidden name=csrf value=\"");
  w_html(&w, csrf_token);
  w_puts(&w, "\"><label for=sv>Status voice</label><select id=sv name=status_voice>");
  for (size_t index = 0U; index < sizeof(voices) / sizeof(voices[0]); ++index) {
    const char *vname = iterate_kit_status_voice_name(voices[index].value);
    const bool selected = configuration->status_voice == (uint8_t)voices[index].value;
    w_printf(&w, "<option value=\"%s\"%s>%s</option>",
             vname, selected ? " selected" : "", voices[index].label);
  }
  w_printf(&w,
      "</select><label for=vol>Speaker volume (%u%%)</label>"
      "<input id=vol name=volume type=number min=0 max=100 value=\"%u\">"
      "<button type=submit>Save settings</button></form></div>",
      (unsigned)status->volume_percent, (unsigned)status->volume_percent);

  /* --- LAN Mind/Wi-Fi transition & Recovery actions (no secret forms on LAN) --- */
  if (!setup_ap) {
    w_puts(&w, "<div class=card><h2>Wi-Fi, Mind &amp; Recovery</h2>"
        "<p class=hint>To keep Wi-Fi passwords and Mind refresh tokens off plain-HTTP LAN, connect or replace them in encrypted Setup Mode (WPA2 SoftAP <b>");
    w_html(&w, status->hostname);
    w_puts(&w, "</b>).</p>"
        "<form method=post action=/enter-setup><input type=hidden name=csrf value=\"");
    w_html(&w, csrf_token);
    w_puts(&w,
        "\"><button type=submit>Enter Setup Mode (link Mind or change Wi-Fi)</button></form>"
        "<form method=post action=/reset-wifi><input type=hidden name=csrf value=\"");
    w_html(&w, csrf_token);
    w_puts(&w,
        "\"><button class=warn type=submit>Reset Wi-Fi (keeps mind)</button></form>"
        "<form method=post action=/reboot><input type=hidden name=csrf value=\"");
    w_html(&w, csrf_token);
    w_puts(&w,
        "\"><button class=sec type=submit>Reboot device</button></form>"
        "<p class=hint>Physical recovery: hold centre button 5 s (unmuted) to reset Wi-Fi; slide Mute ON and hold 10 s for a full wipe.</p>"
        "</div>");
  }

  w_puts(&w, "</body></html>");
  if (w.overflow) {
    out[0] = '\0';
    return 0U;
  }
  return w.len;
}

size_t iterate_kit_web_render_status_json(
    const struct iterate_kit_configuration *configuration,
    const struct iterate_kit_configuration_presence *presence,
    const struct iterate_kit_portal_status *status,
    char *out,
    size_t capacity) {
  struct writer w = {out, capacity, 0U, false};
  if (configuration == NULL || presence == NULL || status == NULL ||
      out == NULL || capacity < 2U) {
    if (out != NULL && capacity > 0U) out[0] = '\0';
    return 0U;
  }
  out[0] = '\0';
  w_puts(&w, "{\"mode\":\"");
  w_json(&w, iterate_kit_boot_mode_name(status->mode));
  w_puts(&w, "\",\"shortId\":\"");
  w_json(&w, status->short_id);
  w_puts(&w, "\",\"hostname\":\"");
  w_json(&w, status->hostname);
  w_puts(&w, "\",\"firmware\":\"");
  w_json(&w, status->firmware_version);
  w_puts(&w, "\",\"ip\":\"");
  w_json(&w, status->ip_address);
  w_puts(&w, "\",\"wifiConfigured\":");
  w_puts(&w, presence->has_wifi ? "true" : "false");
  w_puts(&w, ",\"wifiConnected\":");
  w_puts(&w, status->wifi_connected ? "true" : "false");
  w_puts(&w, ",\"wifiSsid\":\"");
  if (presence->has_wifi) w_json(&w, configuration->wifi_ssid);
  w_puts(&w, "\"");
  if (status->has_rssi) {
    w_printf(&w, ",\"wifiRssi\":%d", (int)status->wifi_rssi);
  } else {
    w_puts(&w, ",\"wifiRssi\":null");
  }
  w_puts(&w, ",\"mindConfigured\":");
  w_puts(&w, presence->has_platform ? "true" : "false");
  w_puts(&w, ",\"platform\":\"");
  w_puts(&w,
      !presence->has_platform ? "none"
      : configuration->platform == ITERATE_KIT_CONFIGURATION_PLATFORM_MIND ? "mind"
                                                                            : "project");
  w_puts(&w, "\",\"mindUrl\":\"");
  if (presence->has_platform) w_json(&w, configuration->os_base_url);
  w_puts(&w, "\",\"connectionState\":\"");
  w_json(&w, status->connection_state);
  w_puts(&w, "\",\"keyRefused\":");
  w_puts(&w, status->key_refused ? "true" : "false");
  w_puts(&w, ",\"statusVoice\":\"");
  w_json(&w, iterate_kit_status_voice_name(
      (enum iterate_kit_status_voice)configuration->status_voice));
  w_printf(&w, "\",\"volume\":%u,\"unlockedRemainingS\":%u}",
           (unsigned)status->volume_percent,
           (unsigned)status->unlocked_remaining_s);
  if (w.overflow) {
    out[0] = '\0';
    return 0U;
  }
  return w.len;
}
