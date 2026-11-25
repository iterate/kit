#ifndef ITERATE_KIT_WEB_PORTAL_H
#define ITERATE_KIT_WEB_PORTAL_H

#include "iterate/kit/configuration.h"
#include "iterate/kit/provisioning_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  ITERATE_KIT_WEB_SCAN_MAX = 16,
  ITERATE_KIT_WEB_MESSAGE_CAPACITY = 128,
};

struct iterate_kit_wifi_scan_entry {
  char ssid[ITERATE_KIT_WIFI_SSID_CAPACITY];
  int8_t rssi;
  bool open;
};

struct iterate_kit_portal_status {
  enum iterate_kit_boot_mode mode;
  char short_id[ITERATE_KIT_SHORT_ID_CAPACITY];
  char hostname[ITERATE_KIT_HOSTNAME_CAPACITY];
  char firmware_version[40];
  char ip_address[20];
  bool wifi_connected;
  bool has_rssi;
  int8_t wifi_rssi;
  char connection_state[32];
  bool key_refused;
  uint8_t volume_percent;
  uint32_t unlocked_remaining_s;
  const struct iterate_kit_wifi_scan_entry *scan_results;
  size_t scan_count;
  const char *banner_message;
  bool banner_is_error;
};

enum iterate_kit_web_field_status {
  ITERATE_KIT_WEB_FIELD_MISSING = 0,
  ITERATE_KIT_WEB_FIELD_OK,
  ITERATE_KIT_WEB_FIELD_TOO_LONG,
  ITERATE_KIT_WEB_FIELD_INVALID,
};

struct iterate_kit_portal_action_result {
  bool ok;
  bool config_changed;
  bool clear_refresh_token;
  bool apply_volume;
  uint8_t new_volume;
  bool reboot_needed;
  char message[ITERATE_KIT_WEB_MESSAGE_CAPACITY];
};

/**
 * True when `path` is a known OS captive-portal connectivity probe
 * (iOS/macOS, Android/ChromeOS, Windows, Firefox) that should receive a 302
 * redirect to the SoftAP portal root.
 */
bool iterate_kit_web_is_captive_probe(const char *path);

/**
 * Extracts and URL-decodes one field from an `application/x-www-form-urlencoded`
 * request body into `destination`. Rejects embedded NULs (`%00`) and malformed
 * percent escapes. Clears `destination` on any non-OK result.
 */
enum iterate_kit_web_field_status iterate_kit_web_form_get_field(
    const char *body,
    size_t body_length,
    const char *field_name,
    char *destination,
    size_t capacity);

/** HTML-escapes `& < > " '` into `destination`. Returns bytes written (excluding NUL). */
size_t iterate_kit_web_html_escape(
    const char *input, char *destination, size_t capacity);

/** Validates and applies a Wi-Fi form (`ssid`, `password`). */
void iterate_kit_web_apply_wifi_form(
    const char *body,
    size_t body_length,
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence,
    struct iterate_kit_portal_action_result *result);

/**
 * Validates and applies a Mind reconnect form (`mind_url`, `client_id`,
 * `refresh_token`) or legacy Project form (`mode=project`, `project_url`,
 * `project_id`, `project_api_key`). On error leaves `configuration` and
 * `presence` untouched.
 */
void iterate_kit_web_apply_mind_form(
    const char *body,
    size_t body_length,
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence,
    struct iterate_kit_portal_action_result *result);

/**
 * Validates and applies basic settings (`status_voice` and optional `volume`).
 * On error leaves `configuration` and `presence` untouched.
 */
void iterate_kit_web_apply_settings_form(
    const char *body,
    size_t body_length,
    struct iterate_kit_configuration *configuration,
    struct iterate_kit_configuration_presence *presence,
    struct iterate_kit_portal_action_result *result);

/**
 * Renders the single-page mobile-friendly HTML UI (both Setup AP and LAN
 * modes). Never emits stored secrets (Wi-Fi password, refresh token, API key,
 * or setup code). Returns bytes written (excluding NUL), or 0 on overflow.
 */
size_t iterate_kit_web_render_page(
    const struct iterate_kit_configuration *configuration,
    const struct iterate_kit_configuration_presence *presence,
    const struct iterate_kit_portal_status *status,
    const char *csrf_token,
    char *out,
    size_t capacity);

/**
 * Renders read-only `/api/status` JSON. Never emits any secret or CSRF token.
 * Returns bytes written (excluding NUL), or 0 on overflow.
 */
size_t iterate_kit_web_render_status_json(
    const struct iterate_kit_configuration *configuration,
    const struct iterate_kit_configuration_presence *presence,
    const struct iterate_kit_portal_status *status,
    char *out,
    size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
