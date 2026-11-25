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
  /* 12 Crockford Base32 chars grouped as xxxx-xxxx-xxxx (14 chars + NUL, 60 bits). */
  ITERATE_KIT_SETUP_CODE_CAPACITY = 15,
  ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES = 12,
  /* 16 hardware-random bytes as 32 lowercase hex chars + NUL (128 bits). */
  ITERATE_KIT_SESSION_TOKEN_CAPACITY = 33,
  ITERATE_KIT_SESSION_ENTROPY_BYTES = 16,
  /* 16 hardware-random bytes as 32 lowercase hex chars + NUL (128 bits). */
  ITERATE_KIT_CSRF_TOKEN_CAPACITY = 33,
  ITERATE_KIT_CSRF_ENTROPY_BYTES = 16,
  ITERATE_KIT_PORTAL_MAX_SESSIONS = 4,
  ITERATE_KIT_WIFI_QR_CAPACITY = 160,
  ITERATE_KIT_PORTAL_UNLOCK_MS = 180000,
  ITERATE_KIT_PORTAL_PHYSICAL_CLAIM_MS = 30000,
  ITERATE_KIT_PORTAL_MAX_FREE_FAILURES = 4,
  ITERATE_KIT_PORTAL_LOCKOUT_BASE_MS = 5000,
};

/**
 * Which boot lifecycle the stored configuration selects.
 *
 *   SETUP_AP      No Wi-Fi credentials stored (fresh preloaded board, or after
 *                 a Wi-Fi reset / full wipe). Starts the WPA2 SoftAP and
 *                 captive portal so a phone can supply Wi-Fi and Mind
 *                 credentials over WPA2 without an app.
 *   LAN_UNLINKED  Wi-Fi is stored, but no iterate mind/project credential yet.
 *                 Joins Wi-Fi STA, advertises `iterate-<id>.local` over mDNS
 *                 and serves the status/settings page (with a button to enter
 *                 encrypted Setup Mode to link a mind).
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
  ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED,
  ITERATE_KIT_PORTAL_AUTH_NO_SESSION,
  ITERATE_KIT_PORTAL_AUTH_BAD_CSRF,
  ITERATE_KIT_PORTAL_AUTH_LOCKED_OUT,
  ITERATE_KIT_PORTAL_AUTH_REQUIRED,
  ITERATE_KIT_PORTAL_AUTH_BAD_CODE,
  ITERATE_KIT_PORTAL_AUTH_NO_CLAIM,
};

struct iterate_kit_portal_session {
  bool active;
  char session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char csrf_token[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  uint64_t last_seen_ms;
  bool unlocked;
  uint64_t unlocked_until_ms;
};

/**
 * Per-client session, Host-header (DNS-rebinding), CSRF, and write-lock guard
 * for the local web server.
 *
 * Every browser receives its own 128-bit session cookie (`kit_sid`) and its own
 * 128-bit CSRF token. Unlocking on LAN (by submitting the 60-bit setup code or
 * by claiming a 30-second one-time physical-button window) rotates that client's
 * session and CSRF tokens and grants administrative writes ONLY to that client.
 * Nothing extends an unlocked window without re-verifying the setup code or
 * pressing the physical button again.
 */
struct iterate_kit_portal_guard {
  char short_id[ITERATE_KIT_SHORT_ID_CAPACITY];
  char setup_code[ITERATE_KIT_SETUP_CODE_CAPACITY];
  bool is_setup_ap;
  bool writes_disabled;
  bool physical_claim_available;
  uint64_t physical_claim_until_ms;
  uint32_t failed_attempts;
  uint64_t lockout_until_ms;
  struct iterate_kit_portal_session sessions[ITERATE_KIT_PORTAL_MAX_SESSIONS];
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
 * Format 12 hardware-random bytes into a 14-character setup code
 * `xxxx-xxxx-xxxx` from the 32-character Crockford Base32 alphabet (60 bits of
 * entropy, >= 8 chars so it serves directly as the SoftAP's WPA2-PSK passphrase
 * while resisting offline handshake cracking).
 */
void iterate_kit_setup_code_format(
    const uint8_t entropy[ITERATE_KIT_SETUP_CODE_ENTROPY_BYTES],
    char out[ITERATE_KIT_SETUP_CODE_CAPACITY]);

/** True when `code` has exact canonical form `xxxx-xxxx-xxxx` from the setup alphabet. */
bool iterate_kit_setup_code_valid(const char *code);

/**
 * Constant-time setup-code comparison, ignoring case, spaces, and hyphens so
 * `K7M49P2X3V8Q` and `k7m4-9p2x-3v8q` match equivalently.
 */
bool iterate_kit_setup_code_equal(const char *candidate, const char *expected);

/** Format 16 hardware-random bytes as 32 lowercase hex chars for a session cookie. */
void iterate_kit_session_token_format(
    const uint8_t entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    char out[ITERATE_KIT_SESSION_TOKEN_CAPACITY]);

/** Format 16 hardware-random bytes as 32 lowercase hex chars for per-session CSRF. */
void iterate_kit_csrf_token_format(
    const uint8_t entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    char out[ITERATE_KIT_CSRF_TOKEN_CAPACITY]);

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
    bool is_setup_ap);

/** Update the guard's setup code once RF HWRNG is active without wiping state. */
void iterate_kit_portal_guard_set_setup_code(
    struct iterate_kit_portal_guard *guard,
    const char setup_code[ITERATE_KIT_SETUP_CODE_CAPACITY]);

/**
 * Permanently refuse all state-changing portal operations (used before physical
 * Wi-Fi reset / full wipe or web Wi-Fi reset so no concurrent POST can write
 * stale credentials back to flash).
 */
void iterate_kit_portal_guard_disable_writes(struct iterate_kit_portal_guard *guard);

/** Re-enable writes only if a web-triggered erase failed and flash state was reloaded. */
void iterate_kit_portal_guard_enable_writes(struct iterate_kit_portal_guard *guard);

/**
 * Look up an existing client session by `cookie_session_token`, or create a new
 * locked session slot using hardware-random `new_session_entropy` and
 * `new_csrf_entropy`. Prefers evicting inactive or oldest locked slots so
 * unauthenticated clients cannot evict an actively unlocked owner session.
 */
bool iterate_kit_portal_guard_ensure_session(
    struct iterate_kit_portal_guard *guard,
    const char *cookie_session_token,
    const uint8_t new_session_entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    const uint8_t new_csrf_entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    uint64_t now_ms,
    char out_session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY],
    char out_csrf_token[ITERATE_KIT_CSRF_TOKEN_CAPACITY],
    bool *out_created_new);

/**
 * Arm a short (30 s) one-time physical-presence claim window after Mute ON +
 * centre button tap, and clear any setup-code backoff lockout. Does NOT unlock
 * any client directly: the first browser that submits `POST /claim-unlock`
 * consumes the claim and receives an exclusive 3-minute unlocked session cookie.
 */
void iterate_kit_portal_guard_arm_physical_claim(
    struct iterate_kit_portal_guard *guard, uint64_t now_ms);

uint32_t iterate_kit_portal_guard_physical_claim_remaining_s(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms);

/**
 * Claim an armed physical-presence window for the calling client session.
 * Consumes the one-time window immediately and rotates the client's session
 * cookie and CSRF token.
 */
enum iterate_kit_portal_auth_result iterate_kit_portal_guard_claim_physical_unlock(
    struct iterate_kit_portal_guard *guard,
    const char *session_token,
    const char *csrf_submitted,
    const uint8_t rotated_session_entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    const uint8_t rotated_csrf_entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    uint64_t now_ms,
    char out_new_session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY]);

/**
 * Verify this client's session cookie, per-session CSRF token, and the 60-bit
 * setup code (always checked, even if the session is already unlocked — never
 * extends a window without the code). On success rotates the session and CSRF
 * tokens and grants this client a 3-minute unlock window.
 */
enum iterate_kit_portal_auth_result iterate_kit_portal_guard_unlock_with_code(
    struct iterate_kit_portal_guard *guard,
    const char *session_token,
    const char *csrf_submitted,
    const char *code_submitted,
    const uint8_t rotated_session_entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    const uint8_t rotated_csrf_entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    uint64_t now_ms,
    char out_new_session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY]);

bool iterate_kit_portal_guard_session_is_unlocked(
    const struct iterate_kit_portal_guard *guard,
    const char *session_token,
    uint64_t now_ms);

uint32_t iterate_kit_portal_guard_session_unlock_remaining_s(
    const struct iterate_kit_portal_guard *guard,
    const char *session_token,
    uint64_t now_ms);

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
 * - Fails immediately if `writes_disabled` is latched.
 * - Requires an active `session_token` cookie and a matching per-session
 *   `csrf_submitted` (constant-time).
 * - In Setup AP mode, the WPA2-PSK link already proves possession of the setup
 *   code, so a valid session + per-session CSRF authorizes.
 * - In LAN mode, requires THAT specific session to be unlocked (`now_ms <
 *   unlocked_until_ms`). Does NOT extend the unlock window.
 */
enum iterate_kit_portal_auth_result iterate_kit_portal_guard_authorize_post(
    struct iterate_kit_portal_guard *guard,
    const char *session_token,
    const char *csrf_submitted,
    uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
