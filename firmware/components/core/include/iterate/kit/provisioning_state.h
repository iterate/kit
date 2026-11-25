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
  /* 12 Crockford Base32 chars grouped as xxxx-xxxx-xxxx (14 chars + NUL, 60 bits).
   * Used ONLY as the SoftAP's WPA2-PSK passphrase; never sent over plain HTTP LAN. */
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
  ITERATE_KIT_PORTAL_CLAIM_TTL_MS = 30000,
};

/**
 * Which boot lifecycle the stored configuration selects.
 *
 *   SETUP_AP      No Wi-Fi credentials stored (fresh preloaded board, or after
 *                 a Wi-Fi reset / full wipe), or one-boot Setup Mode entered
 *                 via physical gesture. Starts the WPA2 SoftAP and captive
 *                 portal so a phone can supply Wi-Fi and Mind credentials over
 *                 WPA2 without an app.
 *   LAN_UNLINKED  Wi-Fi is stored, but no iterate mind/project credential yet.
 *                 Joins Wi-Fi STA, advertises `iterate-<id>.local` over mDNS
 *                 and serves the read-only status + non-secret settings page.
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
  ITERATE_KIT_PORTAL_AUTH_REQUIRED,
  ITERATE_KIT_PORTAL_AUTH_NO_CLAIM,
  ITERATE_KIT_PORTAL_AUTH_CLAIM_BUSY,
};

/**
 * Two-step physical visual-confirmation state machine for pairing a LAN browser:
 *
 *   NONE            No pairing active; LAN browsers cannot light the ring.
 *   OPEN            Tap 1 (Mute ON + short tap) opened a 30 s window for one
 *                   browser to request a claim.
 *   PENDING_CONFIRM One browser submitted `POST /claim-request`. The board
 *                   displays a random (colour, quadrant) pattern on the 12-LED
 *                   ring and shows the same pattern ONLY to that claiming
 *                   browser. That browser STILL has NO write access until Tap 2
 *                   confirms it; flipping Mute OFF, holding any gesture,
 *                   cancelling, or letting 30 s expire cancels cleanly.
 */
enum iterate_kit_claim_state {
  ITERATE_KIT_CLAIM_NONE = 0,
  ITERATE_KIT_CLAIM_OPEN,
  ITERATE_KIT_CLAIM_PENDING_CONFIRM,
};

enum iterate_kit_claim_color {
  ITERATE_KIT_CLAIM_COLOR_BLUE = 0,
  ITERATE_KIT_CLAIM_COLOR_GREEN,
  ITERATE_KIT_CLAIM_COLOR_AMBER,
  ITERATE_KIT_CLAIM_COLOR_MAGENTA,
};

enum iterate_kit_claim_position {
  ITERATE_KIT_CLAIM_POS_TOP = 0,
  ITERATE_KIT_CLAIM_POS_RIGHT,
  ITERATE_KIT_CLAIM_POS_BOTTOM,
  ITERATE_KIT_CLAIM_POS_LEFT,
};

struct iterate_kit_claim_pattern {
  enum iterate_kit_claim_color color;
  enum iterate_kit_claim_position position;
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
 * Per-client session, Host-header (DNS-rebinding), CSRF, visual pairing claim,
 * and write-lock guard for the local web server.
 *
 * The permanent 60-bit setup code is NEVER accepted on LAN and is not held here;
 * it is exclusively the SoftAP WPA2-PSK passphrase. On LAN, write access (which
 * is restricted to non-secret settings: status voice and speaker volume) is
 * granted ONLY to a browser whose visual ring pattern the owner confirmed on
 * the physical board with a second tap.
 */
struct iterate_kit_portal_guard {
  char short_id[ITERATE_KIT_SHORT_ID_CAPACITY];
  bool is_setup_ap;
  bool writes_disabled;
  enum iterate_kit_claim_state claim_state;
  uint64_t claim_expires_at_ms;
  char pending_session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  struct iterate_kit_claim_pattern pending_pattern;
  struct iterate_kit_portal_session sessions[ITERATE_KIT_PORTAL_MAX_SESSIONS];
};

/** Decide which boot mode a stored configuration selects. */
enum iterate_kit_boot_mode iterate_kit_provisioning_boot_mode(
    const struct iterate_kit_configuration_presence *presence);

const char *iterate_kit_boot_mode_name(enum iterate_kit_boot_mode mode);

const char *iterate_kit_claim_color_name(enum iterate_kit_claim_color color);
const char *iterate_kit_claim_position_name(enum iterate_kit_claim_position position);

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
 * entropy, used exclusively as the SoftAP WPA2-PSK passphrase).
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
    bool is_setup_ap);

/**
 * Permanently refuse all state-changing portal operations and cancel any active
 * pairing claim (used before physical reset, full wipe, or reboot).
 */
void iterate_kit_portal_guard_disable_writes(struct iterate_kit_portal_guard *guard);

bool iterate_kit_portal_guard_writes_disabled(
    const struct iterate_kit_portal_guard *guard);

/** True if `session_token` matches an active session slot (for pre-body checks). */
bool iterate_kit_portal_guard_session_exists(
    const struct iterate_kit_portal_guard *guard,
    const char *session_token);

/**
 * Look up an existing client session by `cookie_session_token`, or create a new
 * locked session slot using hardware-random `new_session_entropy` and
 * `new_csrf_entropy`. Prefers evicting inactive or oldest non-pending locked
 * slots so unauthenticated clients cannot evict an unlocked or pending session.
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
 * Tap 1 (Mute ON + short tap): open a 30 s window for one browser to request a
 * visual confirmation claim. Does NOT grant write access.
 */
void iterate_kit_portal_guard_open_claim(
    struct iterate_kit_portal_guard *guard, uint64_t now_ms);

/**
 * Browser submits `POST /claim-request` during an OPEN window.
 * Generates a random (colour, quadrant) visual verification pattern, rotates
 * this client's session cookie and CSRF token, and moves to PENDING_CONFIRM.
 * The browser STILL has NO write access until `confirm_claim()` runs on Tap 2.
 */
enum iterate_kit_portal_auth_result iterate_kit_portal_guard_request_claim(
    struct iterate_kit_portal_guard *guard,
    const char *session_token,
    const char *csrf_submitted,
    uint8_t pattern_entropy_byte,
    const uint8_t rotated_session_entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    const uint8_t rotated_csrf_entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    uint64_t now_ms,
    char out_new_session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY],
    struct iterate_kit_claim_pattern *out_pattern);

/**
 * Tap 2 (Mute ON + short tap while PENDING_CONFIRM): promote ONLY the browser
 * whose visual pattern is currently displayed on the ring to a 3-minute
 * unlocked session, and clear the pending claim. Returns true if confirmed.
 */
bool iterate_kit_portal_guard_confirm_claim(
    struct iterate_kit_portal_guard *guard, uint64_t now_ms);

/** Cancel any OPEN or PENDING_CONFIRM claim (called on Mute OFF, holds, etc.). */
void iterate_kit_portal_guard_cancel_claim(struct iterate_kit_portal_guard *guard);

/** Browser-initiated cancellation (`POST /claim-cancel`) for the claiming session. */
enum iterate_kit_portal_auth_result iterate_kit_portal_guard_cancel_claim_by_session(
    struct iterate_kit_portal_guard *guard,
    const char *session_token,
    const char *csrf_submitted,
    uint64_t now_ms);

enum iterate_kit_claim_state iterate_kit_portal_guard_claim_state(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms);

uint32_t iterate_kit_portal_guard_claim_remaining_s(
    const struct iterate_kit_portal_guard *guard, uint64_t now_ms);

/**
 * Returns true and fills `out_pattern` when a claim is actively waiting for
 * physical confirmation on the LED ring (`PENDING_CONFIRM` and not expired).
 */
bool iterate_kit_portal_guard_active_claim_pattern(
    const struct iterate_kit_portal_guard *guard,
    uint64_t now_ms,
    struct iterate_kit_claim_pattern *out_pattern);

/**
 * Returns true and fills `out_pattern` ONLY when `session_token` is the exact
 * browser that holds the active `PENDING_CONFIRM` claim. Other browsers never
 * see the pattern.
 */
bool iterate_kit_portal_guard_session_has_pending_claim(
    const struct iterate_kit_portal_guard *guard,
    const char *session_token,
    uint64_t now_ms,
    struct iterate_kit_claim_pattern *out_pattern);

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
 * - In Setup AP mode, the WPA2-PSK link proves possession of the setup code.
 * - In LAN mode, requires THAT specific session to be physically confirmed and
 *   unlocked (`now_ms < unlocked_until_ms`). Unconfirmed claims are rejected.
 *   Never extends the unlock window.
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
