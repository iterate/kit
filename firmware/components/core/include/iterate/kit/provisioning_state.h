#ifndef ITERATE_KIT_PROVISIONING_STATE_H
#define ITERATE_KIT_PROVISIONING_STATE_H

#include "iterate/kit/configuration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  ITERATE_KIT_SHORT_ID_CAPACITY = 5,
  ITERATE_KIT_HOSTNAME_CAPACITY = 16,
  ITERATE_KIT_SETUP_CODE_CAPACITY = 10,
  ITERATE_KIT_CSRF_TOKEN_CAPACITY = 17,
  ITERATE_KIT_WIFI_QR_CAPACITY = 160,
  ITERATE_KIT_PORTAL_UNLOCK_MS = 180000,
  ITERATE_KIT_PORTAL_MAX_FREE_FAILURES = 4,
  ITERATE_KIT_PORTAL_LOCKOUT_BASE_MS = 5000,
};

/**
 * Which boot lifecycle the stored configuration selects.
 *
 *   SETUP_AP      No Wi-Fi credentials stored (fresh preloaded board, or after
 *                 a Wi-Fi reset / full wipe). Starts the WPA2 SoftAP and
 *                 captive portal so a phone can supply Wi-Fi without an app.
 *   LAN_UNLINKED  Wi-Fi is stored, but no iterate mind/project credential yet.
 *                 Joins Wi-Fi STA, advertises `iterate-<id>.local` over mDNS
 *                 and serves the status/settings page so the owner can attach
 *                 a mind on the LAN.
 *   NORMAL        Both Wi-Fi and a platform credential are stored. Runs the
 *                 voice loop alongside mDNS and the LAN status/settings page.
 */
enum iterate_kit_boot_mode {
  ITERATE_KIT_BOOT_MODE_SETUP_AP = 0,
  ITERATE_KIT_BOOT_MODE_LAN_UNLINKED,
  ITERATE_KIT_BOOT_MODE_NORMAL,
};

enum iterate_kit_portal_auth_result {
  ITERATE_KIT_PORTAL_AUTH_OK = 0,
  ITERATE_KIT_PORTAL_AUTH_BAD_CSRF,
  ITERATE_KIT_PORTAL_AUTH_LOCKED_OUT,
  ITERATE_KIT_PORTAL_AUTH_REQUIRED,
  ITERATE_KIT_PORTAL_AUTH_BAD_CODE,
};

/**
 * Authorization, Host-header (DNS-rebinding) and CSRF guard for the local web
 * server. All state lives in caller-owned memory; no clock or platform calls.
 */
struct iterate_kit_portal_guard {
  char short_id[ITERATE_KIT_SHORT_ID_CAPACITY];
  char setup_code[ITERATE_KIT_SETUP_CODE_CAPACITY];
  char csrf_token[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  bool is_setup_ap;
  uint64_t unlocked_until_ms;
  uint32_t failed_attempts;
  uint64_t lockout_until_ms;
};

/** Decide which boot mode a stored configuration selects. */
enum iterate_kit_boot_mode iterate_kit_provisioning_boot_mode(
    const struct iterate_kit_configuration_presence *presence);

const char *iterate_kit_boot_mode_name(enum iterate_kit_boot_mode mode);

/** Four lowercase hex chars from the last two bytes of the device MAC. */
void iterate_kit_device_short_id(
    const uint8_t mac[6], char out[ITERATE_KIT_SHORT_ID_CAPACITY]);

/** `iterate-<short_id>` (mDNS hostname without `.local`, and SoftAP SSID). */
bool iterate_kit_device_hostname(
    const char short_id[ITERATE_KIT_SHORT_ID_CAPACITY],
    char *out,
    size_t capacity);

/**
 * Format 8 random bytes into a 9-character setup code `xxxx-xxxx` from an
 * unambiguous 32-character alphabet (40 bits of entropy, >= 8 chars so it also
 * serves directly as the SoftAP's WPA2-PSK passphrase).
 */
void iterate_kit_setup_code_format(
    const uint8_t entropy[8], char out[ITERATE_KIT_SETUP_CODE_CAPACITY]);

/** True when `code` has exact canonical form `xxxx-xxxx` from the setup alphabet. */
bool iterate_kit_setup_code_valid(const char *code);

/**
 * Constant-time setup-code comparison, ignoring case, spaces, and hyphens so
 * `K7M49P2X` and `k7m4-9p2x` match equivalently.
 */
bool iterate_kit_setup_code_equal(const char *candidate, const char *expected);

/** Format 8 random bytes as 16 lowercase hex chars for form CSRF protection. */
void iterate_kit_csrf_token_format(
    const uint8_t entropy[8], char out[ITERATE_KIT_CSRF_TOKEN_CAPACITY]);

/**
 * Format a ZXing / iOS / Android standard `WIFI:` QR payload with proper
 * escaping of `\ ; , : "`. Uses `T:WPA` when `password` is non-empty and
 * `T:nopass` when empty.
 */
bool iterate_kit_wifi_qr_format(
    const char *ssid, const char *password, char *out, size_t capacity);

void iterate_kit_portal_guard_init(
    struct iterate_kit_portal_guard *guard,
    const char short_id[ITERATE_KIT_SHORT_ID_CAPACITY],
    const char setup_code[ITERATE_KIT_SETUP_CODE_CAPACITY],
    const char csrf_token[ITERATE_KIT_CSRF_TOKEN_CAPACITY],
    bool is_setup_ap);

/** Grant temporary LAN administrative access after a physical button gesture. */
void iterate_kit_portal_guard_unlock_physical(
    struct iterate_kit_portal_guard *guard, uint64_t now_ms);

/**
 * Verify CSRF and setup code (with backoff on failures) and, on success, open
 * the same 3-minute unlock window as a physical button press.
 */
enum iterate_kit_portal_auth_result iterate_kit_portal_guard_unlock_with_code(
    struct iterate_kit_portal_guard *guard,
    const char *csrf_submitted,
    const char *code_submitted,
    uint64_t now_ms);

bool iterate_kit_portal_guard_is_unlocked(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms);

uint32_t iterate_kit_portal_guard_unlock_remaining_s(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms);

/**
 * Rejects unexpected `Host` headers to prevent DNS rebinding from untrusted
 * browser tabs. Accepts `iterate-<id>.local` (case-insensitive, optional `:80`),
 * `192.168.4.1` (when `is_setup_ap`), or `current_ip` (when non-empty).
 */
bool iterate_kit_portal_guard_host_allowed(
    const struct iterate_kit_portal_guard *guard,
    const char *host_header,
    const char *current_ip);

/**
 * Authorize a state-changing POST.
 * - Always verifies `csrf_submitted` in constant time.
 * - In Setup AP mode, the WPA2-PSK link already proves possession of the setup
 *   code, so a valid CSRF token suffices.
 * - In LAN mode, accepts either an active physical unlock window or a matching
 *   `code_submitted` (rate-limited with exponential backoff after failures).
 */
enum iterate_kit_portal_auth_result iterate_kit_portal_guard_authorize_post(
    struct iterate_kit_portal_guard *guard,
    const char *csrf_submitted,
    const char *code_submitted,
    uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
