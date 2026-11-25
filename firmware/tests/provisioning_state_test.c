#include "iterate/kit/provisioning_state.h"

#include <assert.h>
#include <string.h>

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
  const uint8_t entropy[8] = {0, 1, 2, 3, 28, 29, 30, 31};
  char short_id[ITERATE_KIT_SHORT_ID_CAPACITY];
  char hostname[ITERATE_KIT_HOSTNAME_CAPACITY];
  char code[ITERATE_KIT_SETUP_CODE_CAPACITY];
  char csrf[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char qr[ITERATE_KIT_WIFI_QR_CAPACITY];

  iterate_kit_device_short_id(mac, short_id);
  assert(strcmp(short_id, "09af") == 0);
  assert(iterate_kit_device_hostname(short_id, hostname, sizeof(hostname)));
  assert(strcmp(hostname, "iterate-09af") == 0);

  iterate_kit_setup_code_format(entropy, code);
  assert(iterate_kit_setup_code_valid(code));
  assert(strlen(code) == 9U && code[4] == '-');
  assert(iterate_kit_setup_code_equal(code, code));
  /* Case, hyphens and spaces are tolerated when typing the code. */
  assert(iterate_kit_setup_code_equal("0123WXYZ", code));
  assert(iterate_kit_setup_code_equal(" 0123 - wxyz ", code));
  assert(!iterate_kit_setup_code_equal("0123-wxy2", code));
  assert(!iterate_kit_setup_code_equal("short", code));

  iterate_kit_csrf_token_format(entropy, csrf);
  assert(strcmp(csrf, "000102031c1d1e1f") == 0);

  assert(iterate_kit_wifi_qr_format("iterate-09af", code, qr, sizeof(qr)));
  assert(strcmp(qr, "WIFI:T:WPA;S:iterate-09af;P:0123-wxyz;;") == 0);
  /* Escapes special QR characters. */
  assert(iterate_kit_wifi_qr_format("a;b:c\\d\"e,f", "p;w:1", qr, sizeof(qr)));
  assert(strcmp(qr, "WIFI:T:WPA;S:a\\;b\\:c\\\\d\\\"e\\,f;P:p\\;w\\:1;;") == 0);
  /* Open network uses nopass. */
  assert(iterate_kit_wifi_qr_format("open-net", "", qr, sizeof(qr)));
  assert(strcmp(qr, "WIFI:T:nopass;S:open-net;;") == 0);
}

static void enforces_host_csrf_unlock_and_backoff_guards(void) {
  struct iterate_kit_portal_guard guard;

  /* Setup AP mode: WPA2 already authenticates; CSRF still required. */
  iterate_kit_portal_guard_init(&guard, "a1b2", "k7m4-9p2x", "0123456789abcdef", true);
  assert(iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1", "192.168.4.1"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "ITERATE-A1B2.local:80", "192.168.4.1"));
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "evil.example", "192.168.4.1"));
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1:8080", "192.168.4.1"));
  assert(iterate_kit_portal_guard_authorize_post(&guard, "wrong", NULL, 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_BAD_CSRF);
  assert(iterate_kit_portal_guard_authorize_post(&guard, "0123456789abcdef", NULL, 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);

  /* LAN mode: requires setup code or physical unlock; rejects 192.168.4.1 unless current_ip. */
  iterate_kit_portal_guard_init(&guard, "a1b2", "k7m4-9p2x", "0123456789abcdef", false);
  assert(!iterate_kit_portal_guard_host_allowed(&guard, "192.168.4.1", "192.168.1.42"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "192.168.1.42", "192.168.1.42"));
  assert(iterate_kit_portal_guard_host_allowed(&guard, "iterate-a1b2.local.", "192.168.1.42"));

  assert(iterate_kit_portal_guard_authorize_post(&guard, "0123456789abcdef", "", 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* 4 free failures, 5th triggers 5s lockout. */
  for (int i = 0; i < 4; ++i) {
    assert(iterate_kit_portal_guard_authorize_post(
               &guard, "0123456789abcdef", "2222-2222", 1000U) ==
           ITERATE_KIT_PORTAL_AUTH_BAD_CODE);
  }
  assert(iterate_kit_portal_guard_authorize_post(
             &guard, "0123456789abcdef", "2222-2222", 1000U) ==
         ITERATE_KIT_PORTAL_AUTH_BAD_CODE);
  /* Even the right code is blocked during lockout. */
  assert(iterate_kit_portal_guard_authorize_post(
             &guard, "0123456789abcdef", "k7m4-9p2x", 5999U) ==
         ITERATE_KIT_PORTAL_AUTH_LOCKED_OUT);
  /* Once lockout expires, valid code succeeds and unlocks via unlock_with_code. */
  assert(iterate_kit_portal_guard_unlock_with_code(
             &guard, "0123456789abcdef", "K7M49P2X", 6000U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_is_unlocked(&guard, 6000U));
  assert(iterate_kit_portal_guard_unlock_remaining_s(&guard, 6000U) == 180U);
  /* Unlocked window allows POST without repeating setup code until expiry. */
  assert(iterate_kit_portal_guard_authorize_post(
             &guard, "0123456789abcdef", "", 185999U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
  assert(iterate_kit_portal_guard_authorize_post(
             &guard, "0123456789abcdef", "", 186000U) ==
         ITERATE_KIT_PORTAL_AUTH_REQUIRED);

  /* Physical button unlock clears lockout immediately. */
  for (int i = 0; i < 5; ++i) {
    (void)iterate_kit_portal_guard_authorize_post(
        &guard, "0123456789abcdef", "2222-2222", 200000U);
  }
  assert(iterate_kit_portal_guard_authorize_post(
             &guard, "0123456789abcdef", "k7m4-9p2x", 201000U) ==
         ITERATE_KIT_PORTAL_AUTH_LOCKED_OUT);
  iterate_kit_portal_guard_unlock_physical(&guard, 201000U);
  assert(iterate_kit_portal_guard_authorize_post(
             &guard, "0123456789abcdef", "", 201000U) ==
         ITERATE_KIT_PORTAL_AUTH_OK);
}

int main(void) {
  selects_boot_mode_from_configuration_presence();
  derives_identity_setup_code_and_qr();
  enforces_host_csrf_unlock_and_backoff_guards();
  return 0;
}
