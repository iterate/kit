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
  /* Case, hyphens and spaces are tolerated when typing the code. */
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
  /* Escapes special QR characters. */
  assert(iterate_kit_wifi_qr_format("a;b:c\\d\"e,f", "p;w:1", qr, sizeof(qr)));
  assert(strcmp(qr, "WIFI:T:WPA;S:a\\;b\\:c\\\\d\\\"e\\,f;P:p\\;w\\:1;;") == 0);
  /* Open network uses nopass. */
  assert(iterate_kit_wifi_qr_format("open-net", "", qr, sizeof(qr)));
  assert(strcmp(qr, "WIFI:T:nopass;S:open-net;;") == 0);
}

static void enforces_per_client_sessions_csrf_rotation_and_no_unauthenticated_extension(void) {
  struct iterate_kit_portal_guard guard;
  uint8_t s1[16], c1[16], s2[16], c2[16], rs1[16], rc1[16], rs2[16], rc2[16];
  char sid_a[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char csrf_a[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char sid_b[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char csrf_b[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char new_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char dummy_csrf[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
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
  iterate_kit_portal_guard_init(&guard, "a1b2", "k7m4-9p2x-3v8q", true);
  assert(iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1", "192.168.4.1"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "ITERATE-A1B2.local:80", "192.168.4.1"));
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "evil.example", "192.168.4.1"));
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1:8080", "192.168.4.1"));
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, "", s1, c1, 1000U, sid_a, csrf_a, &created));
  assert(created);
  assert(iterate_kit_portal_guard_authorize_post(&guard, "", csrf_a, 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_NO_SESSION);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, "wrong", 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_BAD_CSRF);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);

  /* LAN mode: two distinct clients get distinct cookies and distinct CSRF tokens. */
  iterate_kit_portal_guard_init(&guard, "a1b2", "k7m4-9p2x-3v8q", false);
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1", "192.168.1.42"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "192.168.1.42", "192.168.1.42"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "iterate-a1b2.local.", "192.168.1.42"));

  assert(iterate_kit_portal_guard_ensure_session(
      &guard, "", s1, c1, 1000U, sid_a, csrf_a, &created) && created);
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, "", s2, c2, 1000U, sid_b, csrf_b, &created) && created);
  assert(strcmp(sid_a, sid_b) != 0);
  assert(strcmp(csrf_a, csrf_b) != 0);

  /* Client A cannot use Client B's CSRF token and vice versa. */
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_b, 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_BAD_CSRF);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* 4 free failures, 5th triggers 5s lockout. */
  for (int i = 0; i < 5; ++i) {
    assert(iterate_kit_portal_guard_unlock_with_code(
               &guard, sid_a, csrf_a, "2222-2222-2222", rs1, rc1, 1000U, new_sid) ==
           ITERATE_KIT_PORTAL_AUTH_BAD_CODE);
  }
  assert(iterate_kit_portal_guard_unlock_with_code(
             &guard, sid_a, csrf_a, "k7m4-9p2x-3v8q", rs1, rc1, 5999U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_LOCKED_OUT);

  /* After lockout, Client A unlocks with setup code -> session & CSRF rotate. */
  assert(iterate_kit_portal_guard_unlock_with_code(
             &guard, sid_a, csrf_a, "K7M49P2X3V8Q", rs1, rc1, 6000U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(strcmp(new_sid, sid_a) != 0);
  /* Old session cookie and old CSRF are invalid after rotation. */
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 6000U) ==
         ITERATE_KIT_PORTAL_AUTH_NO_SESSION);
  assert(iterate_kit_portal_guard_authorize_post(&guard, new_sid, csrf_a, 6000U) ==
         ITERATE_KIT_PORTAL_AUTH_BAD_CSRF);

  /* Client A fetches its rotated CSRF on the redirected GET /. */
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, new_sid, s1, c1, 6100U, sid_a, csrf_a, &created) && !created);
  assert(strcmp(sid_a, new_sid) == 0);
  assert(iterate_kit_portal_guard_session_is_unlocked(&guard, sid_a, 6100U));
  assert(iterate_kit_portal_guard_session_unlock_remaining_s(&guard, sid_a, 6000U) == 180U);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 6100U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);

  /* Client B is STILL LOCKED while Client A is unlocked, and cannot piggyback! */
  assert(!iterate_kit_portal_guard_session_is_unlocked(&guard, sid_b, 6100U));
  assert(iterate_kit_portal_guard_session_unlock_remaining_s(&guard, sid_b, 6100U) == 0U);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_b, csrf_b, 6100U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_b, csrf_a, 6100U) ==
         ITERATE_KIT_PORTAL_AUTH_BAD_CSRF);

  /* Nothing extends Client A's window without re-entering the valid setup code. */
  assert(iterate_kit_portal_guard_unlock_with_code(
             &guard, sid_a, csrf_a, "", rs2, rc2, 100000U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);
  assert(iterate_kit_portal_guard_unlock_with_code(
             &guard, sid_a, csrf_a, "9999-9999-9999", rs2, rc2, 100000U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_BAD_CODE);
  /* Ordinary POST at t=150000 does NOT extend the 186000 deadline. */
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 150000U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 185999U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 186000U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* Unauthenticated GET spam cannot evict an actively unlocked session. */
  assert(iterate_kit_portal_guard_unlock_with_code(
             &guard, sid_a, csrf_a, "k7m4-9p2x-3v8q", rs2, rc2, 200000U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, new_sid, s1, c1, 200010U, sid_a, csrf_a, &created) && !created);
  for (uint8_t i = 0U; i < 10U; ++i) {
    uint8_t sp_s[16], sp_c[16];
    char sp_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
    fill_seq(sp_s, (uint8_t)(0x90U + i));
    fill_seq(sp_c, (uint8_t)(0xa0U + i));
    assert(iterate_kit_portal_guard_ensure_session(
        &guard, "", sp_s, sp_c, 200100U + i, sp_sid, dummy_csrf, &created) && created);
  }
  assert(iterate_kit_portal_guard_session_is_unlocked(&guard, sid_a, 200500U));
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 200500U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
}

static void enforces_one_time_physical_claim_and_write_disable_lock(void) {
  struct iterate_kit_portal_guard guard;
  uint8_t s1[16], c1[16], s2[16], c2[16], rs1[16], rc1[16], rs2[16], rc2[16];
  char sid_a[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char csrf_a[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char sid_b[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char csrf_b[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char new_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  bool created = false;

  fill_seq(s1, 0x01U);
  fill_seq(c1, 0x11U);
  fill_seq(s2, 0x21U);
  fill_seq(c2, 0x31U);
  fill_seq(rs1, 0x41U);
  fill_seq(rc1, 0x51U);
  fill_seq(rs2, 0x61U);
  fill_seq(rc2, 0x71U);

  iterate_kit_portal_guard_init(&guard, "a1b2", "k7m4-9p2x-3v8q", false);
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, "", s1, c1, 1000U, sid_a, csrf_a, &created) && created);
  assert(iterate_kit_portal_guard_ensure_session(
      &guard, "", s2, c2, 1000U, sid_b, csrf_b, &created) && created);

  /* Claim fails before physical button tap. */
  assert(iterate_kit_portal_guard_claim_physical_unlock(
             &guard, sid_a, csrf_a, rs1, rc1, 1000U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_NO_CLAIM);

  /* Physical tap arms a 30 s claim window — does NOT unlock any client yet! */
  iterate_kit_portal_guard_arm_physical_claim(&guard, 2000U);
  assert(iterate_kit_portal_guard_physical_claim_remaining_s(&guard, 2000U) == 30U);
  assert(!iterate_kit_portal_guard_session_is_unlocked(&guard, sid_a, 2000U));
  assert(!iterate_kit_portal_guard_session_is_unlocked(&guard, sid_b, 2000U));

  /* If unclaimed for 30 s, the claim expires. */
  assert(iterate_kit_portal_guard_physical_claim_remaining_s(&guard, 32000U) == 0U);
  assert(iterate_kit_portal_guard_claim_physical_unlock(
             &guard, sid_a, csrf_a, rs1, rc1, 32000U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_NO_CLAIM);

  /* Arm again -> Client A claims -> exclusive 3-min unlock for A; B gets NO_CLAIM. */
  char new_sid_b[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  iterate_kit_portal_guard_arm_physical_claim(&guard, 40000U);
  assert(iterate_kit_portal_guard_claim_physical_unlock(
             &guard, sid_a, csrf_a, rs1, rc1, 45000U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_physical_claim_remaining_s(&guard, 45000U) == 0U);
  assert(iterate_kit_portal_guard_claim_physical_unlock(
             &guard, sid_b, csrf_b, rs2, rc2, 45001U, new_sid_b) ==
         ITERATE_KIT_PORTAL_AUTH_NO_CLAIM);
  assert(new_sid_b[0] == '\0');
  assert(!iterate_kit_portal_guard_session_is_unlocked(&guard, sid_b, 45001U));

  assert(iterate_kit_portal_guard_ensure_session(
      &guard, new_sid, s1, c1, 45100U, sid_a, csrf_a, &created) && !created);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 45100U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);

  /* Disabling writes (before physical reset/wipe) rejects ALL writes, unlocks, and claims. */
  iterate_kit_portal_guard_disable_writes(&guard);
  assert(iterate_kit_portal_guard_authorize_post(&guard, sid_a, csrf_a, 45200U) ==
         ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED);
  assert(iterate_kit_portal_guard_unlock_with_code(
             &guard, sid_b, csrf_b, "k7m4-9p2x-3v8q", rs2, rc2, 45200U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED);
  iterate_kit_portal_guard_arm_physical_claim(&guard, 45200U);
  assert(iterate_kit_portal_guard_claim_physical_unlock(
             &guard, sid_b, csrf_b, rs2, rc2, 45200U, new_sid) ==
         ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED);
}

int main(void) {
  selects_boot_mode_from_configuration_presence();
  derives_identity_setup_code_and_qr();
  enforces_per_client_sessions_csrf_rotation_and_no_unauthenticated_extension();
  enforces_one_time_physical_claim_and_write_disable_lock();
  return 0;
}
