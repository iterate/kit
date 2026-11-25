#include "iterate/kit/web_portal.h"

#include <assert.h>
#include <string.h>

static void detects_captive_probes_and_decodes_forms(void) {
  char out[64];
  assert(iterate_kit_web_is_captive_probe("/hotspot-detect.html"));
  assert(iterate_kit_web_is_captive_probe("/generate_204?foo=1"));
  assert(iterate_kit_web_is_captive_probe("/connecttest.txt"));
  assert(!iterate_kit_web_is_captive_probe("/"));
  assert(!iterate_kit_web_is_captive_probe("/api/status"));

  const char *body = "ssid=My+Home+Wi%2DFi%21&password=super%20secret%261&empty=";
  assert(iterate_kit_web_form_get_field(body, strlen(body), "ssid", out, sizeof(out)) ==
         ITERATE_KIT_WEB_FIELD_OK);
  assert(strcmp(out, "My Home Wi-Fi!") == 0);
  assert(iterate_kit_web_form_get_field(body, strlen(body), "password", out, sizeof(out)) ==
         ITERATE_KIT_WEB_FIELD_OK);
  assert(strcmp(out, "super secret&1") == 0);
  assert(iterate_kit_web_form_get_field(body, strlen(body), "empty", out, sizeof(out)) ==
         ITERATE_KIT_WEB_FIELD_OK);
  assert(strcmp(out, "") == 0);
  assert(iterate_kit_web_form_get_field(body, strlen(body), "nope", out, sizeof(out)) ==
         ITERATE_KIT_WEB_FIELD_MISSING);

  /* Rejects embedded NUL and broken percent escape. */
  const char *bad_nul = "ssid=abc%00def";
  assert(iterate_kit_web_form_get_field(bad_nul, strlen(bad_nul), "ssid", out, sizeof(out)) ==
         ITERATE_KIT_WEB_FIELD_INVALID);
  assert(out[0] == '\0');
  const char *bad_pct = "ssid=abc%2";
  assert(iterate_kit_web_form_get_field(bad_pct, strlen(bad_pct), "ssid", out, sizeof(out)) ==
         ITERATE_KIT_WEB_FIELD_INVALID);

  /* HTML escaping protects against XSS. */
  assert(iterate_kit_web_html_escape("<script>\"a&b'</script>", out, sizeof(out)) > 0U);
  assert(strcmp(out, "&lt;script&gt;&quot;a&amp;b&#39;&lt;/script&gt;") == 0);
}

static void applies_wifi_mind_and_settings_forms_safely(void) {
  struct iterate_kit_configuration cfg;
  struct iterate_kit_configuration_presence presence;
  struct iterate_kit_portal_action_result res;
  memset(&cfg, 0, sizeof(cfg));
  memset(&presence, 0, sizeof(presence));

  /* Short WPA password rejected without mutating config. */
  const char *bad_wifi = "ssid=LabNet&password=short";
  iterate_kit_web_apply_wifi_form(bad_wifi, strlen(bad_wifi), &cfg, &presence, &res);
  assert(!res.ok && !res.config_changed && !presence.has_wifi);

  /* Valid Wi-Fi saved. */
  const char *good_wifi = "ssid=LabNet&password=correct+horse+battery";
  iterate_kit_web_apply_wifi_form(good_wifi, strlen(good_wifi), &cfg, &presence, &res);
  assert(res.ok && res.config_changed && res.reboot_needed && presence.has_wifi);
  assert(strcmp(cfg.wifi_ssid, "LabNet") == 0);
  assert(strcmp(cfg.wifi_password, "correct horse battery") == 0);

  /* Open network allowed. */
  const char *open_wifi = "ssid=GuestNet&password=";
  iterate_kit_web_apply_wifi_form(open_wifi, strlen(open_wifi), &cfg, &presence, &res);
  assert(res.ok && strcmp(cfg.wifi_ssid, "GuestNet") == 0 && cfg.wifi_password[0] == '\0');

  /* Mind form normalizes bare domain and sets clear_refresh_token. */
  const char *mind_form =
      "mode=mind&mind_url=example.family"
      "&client_id=https%3A%2F%2Fk.iterate.com%2Fdevices%2Fhavpe%2Fclients%2F1.json"
      "&refresh_token=usr_1%3Agrt_1%3Asecret_tok_99";
  iterate_kit_web_apply_mind_form(mind_form, strlen(mind_form), &cfg, &presence, &res);
  assert(res.ok && res.config_changed && res.clear_refresh_token && res.reboot_needed);
  assert(presence.has_platform && cfg.platform == ITERATE_KIT_CONFIGURATION_PLATFORM_MIND);
  assert(strcmp(cfg.os_base_url, "https://iterate.example.family") == 0);
  assert(strcmp(cfg.oauth_client_id, "https://k.iterate.com/devices/havpe/clients/1.json") == 0);
  assert(strcmp(cfg.oauth_refresh_token, "usr_1:grt_1:secret_tok_99") == 0);

  /* Invalid mind update leaves existing valid mind intact. */
  const char *bad_mind = "mode=mind&mind_url=bad&client_id=ftp://x&refresh_token=abc";
  iterate_kit_web_apply_mind_form(bad_mind, strlen(bad_mind), &cfg, &presence, &res);
  assert(!res.ok && !res.config_changed);
  assert(strcmp(cfg.os_base_url, "https://iterate.example.family") == 0);

  /* Settings form: volume-only applies live with no reboot. */
  const char *vol_only = "volume=65";
  iterate_kit_web_apply_settings_form(vol_only, strlen(vol_only), &cfg, &presence, &res);
  assert(res.ok && res.apply_volume && res.new_volume == 65U && !res.config_changed && !res.reboot_needed);

  /* Status voice change saves and requests reboot. */
  const char *voice_form = "status_voice=daisy-bell&volume=80";
  iterate_kit_web_apply_settings_form(voice_form, strlen(voice_form), &cfg, &presence, &res);
  assert(res.ok && res.config_changed && res.reboot_needed && res.apply_volume && res.new_volume == 80U);
  assert(cfg.status_voice == ITERATE_KIT_STATUS_VOICE_DAISY_BELL);

  /* Invalid volume rejected. */
  const char *bad_vol = "volume=150";
  iterate_kit_web_apply_settings_form(bad_vol, strlen(bad_vol), &cfg, &presence, &res);
  assert(!res.ok);
}

static void rendered_html_and_json_never_leak_secrets_and_escape_xss(void) {
  struct iterate_kit_configuration cfg;
  struct iterate_kit_configuration_presence presence = {true, true, true};
  struct iterate_kit_portal_status status;
  struct iterate_kit_wifi_scan_entry scans[1];
  static char html[16384];
  static char json[1024];

  memset(&cfg, 0, sizeof(cfg));
  memset(&status, 0, sizeof(status));
  memcpy(cfg.wifi_ssid, "<b>Evil\"Net</b>", sizeof("<b>Evil\"Net</b>"));
  memcpy(cfg.wifi_password, "SUPER_SECRET_WIFI_PASS_123", sizeof("SUPER_SECRET_WIFI_PASS_123"));
  memcpy(cfg.os_base_url, "https://iterate.example.com", sizeof("https://iterate.example.com"));
  memcpy(cfg.oauth_client_id, "https://k.iterate.com/devices/havpe/clients/1.json",
         sizeof("https://k.iterate.com/devices/havpe/clients/1.json"));
  memcpy(cfg.oauth_refresh_token, "usr_1:grt_1:NEVER_SHOW_THIS_TOKEN",
         sizeof("usr_1:grt_1:NEVER_SHOW_THIS_TOKEN"));
  cfg.platform = ITERATE_KIT_CONFIGURATION_PLATFORM_MIND;
  cfg.status_voice = ITERATE_KIT_STATUS_VOICE_SPOKEN;

  status.mode = ITERATE_KIT_BOOT_MODE_NORMAL;
  memcpy(status.short_id, "a1b2", 5U);
  memcpy(status.hostname, "iterate-a1b2", sizeof("iterate-a1b2"));
  memcpy(status.firmware_version, "003114-2026-11-25-abcdef0", sizeof("003114-2026-11-25-abcdef0"));
  memcpy(status.ip_address, "192.168.1.42", sizeof("192.168.1.42"));
  status.wifi_connected = true;
  status.has_rssi = true;
  status.wifi_rssi = -58;
  memcpy(status.connection_state, "ready", sizeof("ready"));
  status.volume_percent = 75U;

  const size_t html_len = iterate_kit_web_render_page(
      &cfg, &presence, &status, "0123456789abcdef", html, sizeof(html));
  assert(html_len > 500U);
  assert(strstr(html, "iterate-a1b2.local") != NULL);
  assert(strstr(html, "-58 dBm (Good)") != NULL);
  assert(strstr(html, "003114-2026-11-25-abcdef0") != NULL);
  assert(strstr(html, "https://iterate.example.com") != NULL);
  /* XSS escaped. */
  assert(strstr(html, "<b>Evil") == NULL);
  assert(strstr(html, "&lt;b&gt;Evil&quot;Net&lt;/b&gt;") != NULL);
  /* Secrets NEVER appear in HTML. */
  assert(strstr(html, "SUPER_SECRET_WIFI_PASS_123") == NULL);
  assert(strstr(html, "NEVER_SHOW_THIS_TOKEN") == NULL);

  /* JSON also never leaks secrets or CSRF. */
  const size_t json_len = iterate_kit_web_render_status_json(
      &cfg, &presence, &status, json, sizeof(json));
  assert(json_len > 50U);
  assert(strstr(json, "\"wifiRssi\":-58") != NULL);
  assert(strstr(json, "\"connectionState\":\"ready\"") != NULL);
  assert(strstr(json, "SUPER_SECRET_WIFI_PASS_123") == NULL);
  assert(strstr(json, "NEVER_SHOW_THIS_TOKEN") == NULL);
  assert(strstr(json, "0123456789abcdef") == NULL);

  /* Setup AP page renders scanned networks. */
  memset(&scans, 0, sizeof(scans));
  memcpy(scans[0].ssid, "CoffeeShop", sizeof("CoffeeShop"));
  scans[0].rssi = -62;
  scans[0].open = true;
  status.mode = ITERATE_KIT_BOOT_MODE_SETUP_AP;
  status.scan_results = scans;
  status.scan_count = 1U;
  const size_t ap_len = iterate_kit_web_render_page(
      &cfg, &presence, &status, "0123456789abcdef", html, sizeof(html));
  assert(ap_len > 500U);
  assert(strstr(html, "CoffeeShop") != NULL);
  assert(strstr(html, "Save Wi-Fi") != NULL);
  assert(strstr(html, "SUPER_SECRET_WIFI_PASS_123") == NULL);
}

int main(void) {
  detects_captive_probes_and_decodes_forms();
  applies_wifi_mind_and_settings_forms_safely();
  rendered_html_and_json_never_leak_secrets_and_escape_xss();
  return 0;
}
