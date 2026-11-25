#include "iterate/kit/provisioning_state.h"

#include <assert.h>
#include <string.h>

static void fill_seq(uint8_t out[16], uint8_t base) {
  for (size_t i = 0U; i < 16U; ++i) {
    out[i] = (uint8_t)(base + (uint8_t)i);
  }
}

static void selects_boot_mode_from_configuration_presence(void) {
  struct iterate_kit_configuration_presence p = {0};
  assert(iterate_kit_provisioning_boot_mode(NULL) == ITERATE_KIT_BOOT_MODE_SETUP_AP);
  assert(iterate_kit_provisioning_boot_mode(&p) == ITERATE_KIT_BOOT_MODE_SETUP_AP);

  /* Preserved mind with cleared Wi-Fi still enters Setup AP mode. */
  p.has_platform = true;
  assert(iterate_kit_provisioning_boot_mode(&p) == ITERATE_KIT_BOOT_MODE_SETUP_AP);

  /* Wi-Fi present without mind enters LAN Awaiting-Mind mode. */
  p.has_wifi = true;
  p.has_platform = false;
  assert(iterate_kit_provisioning_boot_mode(&p) == ITERATE_KIT_BOOT_MODE_LAN_UNLINKED);

  /* Both Wi-Fi and mind enters Normal mode. */
  p.has_platform = true;
  assert(iterate_kit_provisioning_boot_mode(&p) == ITERATE_KIT_BOOT_MODE_NORMAL);

  /* One-boot RTC Setup Mode flag overrides Normal mode only on valid SW reset sentinel. */
  const uint64_t magic = UINT64_C(0x4954455253455455);
  assert(!iterate_kit_setup_once_rtc_valid(false, magic, ~magic, magic));
  assert(!iterate_kit_setup_once_rtc_valid(true, magic, magic, magic));
  assert(!iterate_kit_setup_once_rtc_valid(true, 0U, ~UINT64_C(0), magic));
  assert(iterate_kit_setup_once_rtc_valid(true, magic, ~magic, magic));
  assert(iterate_kit_provisioning_select_boot_mode(&p, false) == ITERATE_KIT_BOOT_MODE_NORMAL);
  assert(iterate_kit_provisioning_select_boot_mode(&p, true) == ITERATE_KIT_BOOT_MODE_SETUP_AP);
}

static void derives_identity_setup_code_and_qr(void) {
  const uint8_t mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0x09, 0xaf};
  const uint8_t entropy[16] = {0, 1, 2, 3, 10, 11, 12, 13, 28, 29, 30, 31, 0xaa, 0xbb, 0xcc, 0xdd};
  char short_id[ITERATE_KIT_SHORT_ID_CAPACITY];
  char hostname[ITERATE_KIT_HOSTNAME_CAPACITY];
  char code[ITERATE_KIT_SETUP_CODE_CAPACITY];
  char sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char csrf[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char qr[ITERATE_KIT_WIFI_QR_CAPACITY];

  iterate_kit_device_short_id(mac, short_id);
  assert(strcmp(short_id, "09af") == 0);
  assert(iterate_kit_device_hostname(short_id, hostname, sizeof(hostname)));
  assert(strcmp(hostname, "iterate-09af") == 0);

  iterate_kit_setup_code_format(entropy, code);
  assert(iterate_kit_setup_code_valid(code));
  assert(strlen(code) == 14U && code[4] == '-' && code[9] == '-');
  assert(strcmp(code, "0123-abcd-wxyz") == 0);
  assert(iterate_kit_setup_code_equal(code, code));
  assert(iterate_kit_setup_code_equal("0123ABCDWXYZ", code));
  assert(iterate_kit_setup_code_equal(" 0123 - abcd - wxyz ", code));
  assert(!iterate_kit_setup_code_equal("0123-abcd-wxy2", code));
  assert(!iterate_kit_setup_code_equal("0123-wxyz", code));
  assert(!iterate_kit_setup_code_valid("0123-wxyz"));

  iterate_kit_session_token_format(entropy, sid);
  assert(strcmp(sid, "000102030a0b0c0d1c1d1e1faabbccdd") == 0);
  iterate_kit_csrf_token_format(entropy, csrf);
  assert(strcmp(csrf, "000102030a0b0c0d1c1d1e1faabbccdd") == 0);

  assert(iterate_kit_wifi_qr_format("iterate-09af", code, qr, sizeof(qr)));
  assert(strcmp(qr, "WIFI:T:WPA;S:iterate-09af;P:0123-abcd-wxyz;;") == 0);
  assert(iterate_kit_wifi_qr_format("a;b:c\\d\"e,f", "p;w:1", qr, sizeof(qr)));
  assert(strcmp(qr, "WIFI:T:WPA;S:a\\;b\\:c\\\\d\\\"e\\,f;P:p\\;w\\:1;;") == 0);
  assert(iterate_kit_wifi_qr_format("open-net", "", qr, sizeof(qr)));
  assert(strcmp(qr, "WIFI:T:nopass;S:open-net;;") == 0);
}

static void enforces_visual_claim_confirmation_and_client_isolation(void) {
  struct iterate_kit_portal_guard guard;
  uint8_t s1[16], c1[16], s2[16], c2[16], rs1[16], rc1[16], rs2[16], rc2[16];
  char sid_a[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char csrf_a[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char sid_b[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char csrf_b[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char rot_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char rot_sid_b[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  struct iterate_kit_claim_pattern pat_a, pat_ring, pat_check;
  bool created = false;

  fill_seq(s1, 0x10U);
  fill_seq(c1, 0x20U);
  fill_seq(s2, 0x30U);
  fill_seq(c2, 0x40U);
  fill_seq(rs1, 0x50U);
  fill_seq(rc1, 0x60U);
  fill_seq(rs2, 0x70U);
  fill_seq(rc2, 0x80U);

  /* Setup AP mode: WPA2 authenticates link; per-client session + CSRF still required. */
  iterate_kit_portal_guard_init(&guard, "a1b2", true);
  assert(iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1", "192.168.4.1"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "ITERATE-A1B2.local:80", "192.168.4.1"));
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "evil.example", "192.168.4.1"));
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1:8080", "192.168.4.1"));
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, "", s1, c1, 1000U, sid_a, csrf_a, &created) && created);
  assert(iterate_kit_portal_guard_session_exists(&guard, sid_a));
  assert(!iterate_kit_portal_guard_session_exists(&guard, "nonexistent"));
  assert(iterate_kit_portal_guard_authorize_post(&guard, "", csrf_a, 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_NO_SESSION);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, "wrong", 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_BAD_CSRF);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);

  /* LAN mode: two browsers get distinct cookies and distinct CSRF tokens. */
  iterate_kit_portal_guard_init(&guard, "a1b2", false);
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1", "192.168.1.42"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "192.168.1.42", "192.168.1.42"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "iterate-a1b2.local.", "192.168.1.42"));

  assert(iterate_kit_portal_guard_ensure_session(
      &guard, "", s1, c1, 1000U, sid_a, csrf_a, &created) && created);
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, "", s2, c2, 1000U, sid_b, csrf_b, &created) && created);
  assert(strcmp(sid_a, sid_b) != 0);
  assert(strcmp(csrf_a, csrf_b) != 0);

  /* Before physical Tap 1, requesting a claim fails (cannot light up the ring). */
  assert(iterate_kit_portal_guard_request_claim(
             &guard, sid_a, csrf_a, 0x07U, rs1, rc1, 1000U, rot_sid, &pat_a) ==
         ITERATE_KIT_PORTAL_AUTH_NO_CLAIM);
  assert(!iterate_kit_portal_guard_active_claim_pattern(&guard, 1000U, &pat_ring));

  /* Tap 1 opens a 30 s window; still no write access for anyone. */
  iterate_kit_portal_guard_open_claim(&guard, 2000U);
  assert(iterate_kit_portal_guard_claim_state(&guard, 2000U) == ITERATE_KIT_CLAIM_OPEN);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 2000U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* Attacker (Client B) races Tap 1 and claims first -> gets PENDING_CONFIRM pattern,
   * but STILL HAS ZERO WRITE ACCESS! */
  assert(iterate_kit_portal_guard_request_claim(
             &guard, sid_b, csrf_b, 0x06U /* AMBER + RIGHT */, rs2, rc2, 3000U,
             rot_sid_b, &pat_a) == ITERATE_KIT_PORTAL_AUTH_OK);
  assert(pat_a.color == ITERATE_KIT_CLAIM_COLOR_AMBER);
  assert(pat_a.position == ITERATE_KIT_CLAIM_POS_RIGHT);
  /* Client B fetches its rotated CSRF after redirect. */
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, rot_sid_b, s2, c2, 3100U, sid_b, csrf_b, &created) && !created);
  /* CRITICAL RULE: an unconfirmed claim gives NO write access! */
  assert(!iterate_kit_portal_guard_session_is_unlocked(&guard, sid_b, 3100U));
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_b, csrf_b, 3100U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);
  /* Ring shows B's pattern; owner (Client A) sees CLAIM_BUSY and NEVER sees B's pattern. */
  assert(iterate_kit_portal_guard_active_claim_pattern(&guard, 3100U, &pat_ring));
  assert(pat_ring.color == ITERATE_KIT_CLAIM_COLOR_AMBER &&
         pat_ring.position == ITERATE_KIT_CLAIM_POS_RIGHT);
  assert(iterate_kit_portal_guard_session_has_pending_claim(&guard, sid_b, 3100U, &pat_check));
  assert(!iterate_kit_portal_guard_session_has_pending_claim(&guard, sid_a, 3100U, &pat_check));
  assert(iterate_kit_portal_guard_request_claim(
             &guard, sid_a, csrf_a, 0x01U, rs1, rc1, 3200U, rot_sid, &pat_a) ==
         ITERATE_KIT_PORTAL_AUTH_CLAIM_BUSY);

  /* Owner sees mismatched ring and flips Mute OFF -> cancels attacker's claim cleanly. */
  iterate_kit_portal_guard_cancel_claim(&guard);
  assert(iterate_kit_portal_guard_claim_state(&guard, 3300U) == ITERATE_KIT_CLAIM_NONE);
  assert(!iterate_kit_portal_guard_confirm_claim(&guard, 3300U));
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_b, csrf_b, 3300U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* Unconfirmed claim also expires cleanly after 30 s with ZERO write access. */
  iterate_kit_portal_guard_open_claim(&guard, 10000U);
  assert(iterate_kit_portal_guard_request_claim(
             &guard, sid_a, csrf_a, 0x0dU /* GREEN + LEFT */, rs1, rc1, 11000U,
             rot_sid, &pat_a) == ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, rot_sid, s1, c1, 11100U, sid_a, csrf_a, &created) && !created);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 40999U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);
  assert(!iterate_kit_portal_guard_confirm_claim(&guard, 41000U));
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 41000U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* Owner's browser-initiated cancel works only for the claiming browser. */
  uint8_t rs3[16], rc3[16];
  fill_seq(rs3, 0xb0U);
  fill_seq(rc3, 0xc0U);
  iterate_kit_portal_guard_open_claim(&guard, 50000U);
  assert(iterate_kit_portal_guard_request_claim(
             &guard, sid_a, csrf_a, 0x03U /* MAGENTA + TOP */, rs3, rc3, 51000U,
             rot_sid, &pat_a) == ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, rot_sid, s1, c1, 51100U, sid_a, csrf_a, &created) && !created);
  assert(iterate_kit_portal_guard_cancel_claim_by_session(&guard, sid_b, csrf_b, 51200U) ==
         ITERATE_KIT_PORTAL_AUTH_NO_CLAIM);
  assert(iterate_kit_portal_guard_cancel_claim_by_session(&guard, sid_a, csrf_a, 51200U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_claim_state(&guard, 51200U) == ITERATE_KIT_CLAIM_NONE);

  /* Full 2-tap visual pairing: Tap 1 -> Client A requests -> ring shows pattern ->
   * Tap 2 confirms -> ONLY Client A gets 3-min write access (never extended on POST). */
  iterate_kit_portal_guard_open_claim(&guard, 60000U);
  assert(iterate_kit_portal_guard_request_claim(
             &guard, sid_a, csrf_a, 0x07U /* MAGENTA + RIGHT */, rs1, rc1, 61000U,
             rot_sid, &pat_a) == ITERATE_KIT_PORTAL_AUTH_OK);
  assert(pat_a.color == ITERATE_KIT_CLAIM_COLOR_MAGENTA &&
         pat_a.position == ITERATE_KIT_CLAIM_POS_RIGHT);
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, rot_sid, s1, c1, 61100U, sid_a, csrf_a, &created) && !created);
  /* Unauthenticated GET spam cannot evict the pending-claim session before Tap 2. */
  for (uint8_t i = 0U; i < 8U; ++i) {
    uint8_t sp_s[16], sp_c[16];
    char sp_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
    char sp_csrf[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
    fill_seq(sp_s, (uint8_t)(0x90U + i));
    fill_seq(sp_c, (uint8_t)(0xa0U + i));
    assert(iterate_kit_portal_guard_ensure_session(
        &guard, "", sp_s, sp_c, 61200U + i, sp_sid, sp_csrf, &created) && created);
  }
  assert(iterate_kit_portal_guard_session_has_pending_claim(&guard, sid_a, 62000U, &pat_check));
  /* Still locked before Tap 2. */
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 62000U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* Tap 2 confirms! */
  assert(iterate_kit_portal_guard_confirm_claim(&guard, 63000U));
  assert(iterate_kit_portal_guard_claim_state(&guard, 63000U) == ITERATE_KIT_CLAIM_NONE);
  assert(iterate_kit_portal_guard_session_is_unlocked(&guard, sid_a, 63000U));
  assert(iterate_kit_portal_guard_session_unlock_remaining_s(&guard, sid_a, 63000U) == 180U);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 63000U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  /* Other browser is still locked. */
  assert(!iterate_kit_portal_guard_session_is_unlocked(&guard, sid_b, 63000U));
  /* Ordinary POSTs do not extend the 180 s deadline. */
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 242999U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 243000U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* Disabling writes cancels any active claim and rejects all POSTs & claims. */
  iterate_kit_portal_guard_open_claim(&guard, 300000U);
  iterate_kit_portal_guard_disable_writes(&guard);
  assert(iterate_kit_portal_guard_writes_disabled(&guard));
  assert(iterate_kit_portal_guard_claim_state(&guard, 300000U) == ITERATE_KIT_CLAIM_NONE);
  assert(iterate_kit_portal_guard_request_claim(
             &guard, sid_a, csrf_a, 0x01U, rs2, rc2, 300000U, rot_sid, &pat_a) ==
         ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 300000U) ==
         ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED);
}

int main(void) {
  selects_boot_mode_from_configuration_presence();
  derives_identity_setup_code_and_qr();
  enforces_visual_claim_confirmation_and_client_isolation();
  return 0;
}
