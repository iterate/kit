#include "havpe_portal.h"

#include "havpe_reset_gesture.h"
#include "iterate/kit/captive_dns.h"
#include "iterate/kit/platforms/board.h"
#include "iterate/kit/platforms/led_ring.h"
#include "iterate/kit/platforms/provisioning.h"
#include "iterate/kit/platforms/restart_note.h"
#include "iterate/kit/provisioning_state.h"
#include "iterate/kit/web_portal.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char tag[] = "havpe-portal";

enum {
  HAVPE_BUTTON_GPIO = 0,
  HAVPE_MUTE_GPIO = 3,
  HTML_BUFFER_BYTES = 16384,
  JSON_BUFFER_BYTES = 1024,
  /* Per-route request body caps (strictly bounded, never megabytes). */
  POST_BODY_TINY_MAX = 128,      /* /claim-request, /claim-cancel, /reboot (CSRF only) */
  POST_BODY_SETTINGS_MAX = 256,  /* /settings (CSRF + status_voice + volume) */
  POST_BODY_WIFI_MAX = 512,      /* /wifi (Setup AP only) */
  POST_BODY_MIND_MAX = 1024,     /* /mind (Setup AP only) */
  POST_BODY_DEADLINE_MS = 2000,  /* Hard wall-clock deadline to receive any POST body */
  COOKIE_HEADER_MAX_BYTES = 512,
  SET_COOKIE_MAX_BYTES = 128,
  REBOOT_GRACE_MS = 700,
  DNS_TASK_STACK_BYTES = 3072,
};

static const uint64_t SETUP_ONCE_MAGIC = UINT64_C(0x4954455253455455);

/* One-boot RTC flag set by the physical Enter Setup Mode gesture (Mute ON +
 * hold 3 s) to enter WPA2 Setup Mode once without erasing saved Wi-Fi or Mind
 * credentials. Valid only across a software restart (`ESP_RST_SW`) and cleared
 * immediately on boot. */
RTC_NOINIT_ATTR static struct {
  uint64_t magic;
  uint64_t inv_magic;
} setup_once_rtc;

/* PSRAM-resident page buffers accessed exclusively by the single httpd task. */
EXT_RAM_BSS_ATTR static char html_buffer[HTML_BUFFER_BYTES];
EXT_RAM_BSS_ATTR static char json_buffer[JSON_BUFFER_BYTES];

/* Lock-free latch set immediately when a physical reset/wipe/setup gesture
 * fires so any in-flight `read_post_body()` aborts without waiting for timeout. */
static bool portal_writes_latched;

static struct {
  /*
   * LOCKING RULE: `portal.lock` protects in-memory state and flash writes/erases.
   * It is NEVER held across network I/O (`httpd_req_recv` or `httpd_resp_send`),
   * so a stalled TCP client can NEVER block a physical reset, wipe, or gesture.
   */
  SemaphoreHandle_t lock;
  struct iterate_kit_configuration config;
  struct iterate_kit_configuration_presence presence;
  struct iterate_kit_portal_guard guard;
  enum iterate_kit_boot_mode mode;
  bool rf_entropy_ready;
  char short_id[ITERATE_KIT_SHORT_ID_CAPACITY];
  char hostname[ITERATE_KIT_HOSTNAME_CAPACITY];
  /* Loaded ONLY in Setup AP mode for the WPA2-PSK SoftAP passphrase; never
   * loaded or accepted over HTTP in LAN modes. */
  char setup_code[ITERATE_KIT_SETUP_CODE_CAPACITY];
  char ip_address[20];
  char connection_state[32];
  bool wifi_connected;
  bool has_rssi;
  int8_t wifi_rssi;
  bool key_refused;
  uint8_t volume_percent;
  struct iterate_kit_wifi_scan_entry scan_results[ITERATE_KIT_WEB_SCAN_MAX];
  size_t scan_count;
  char banner_session_token[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char banner_message[ITERATE_KIT_WEB_MESSAGE_CAPACITY];
  bool banner_is_error;
  bool reboot_pending;
  uint64_t reboot_at_ms;
  char reboot_note[48];
  bool lan_services_started;
  bool mdns_started;
  httpd_handle_t server;
  struct havpe_reset_gesture gesture;
  uint64_t last_net_poll_ms;
} portal;

static void arm_setup_ap_once_rtc(void) {
  setup_once_rtc.magic = SETUP_ONCE_MAGIC;
  setup_once_rtc.inv_magic = ~SETUP_ONCE_MAGIC;
}

bool havpe_portal_consume_setup_ap_request(void) {
  const uint64_t magic = setup_once_rtc.magic;
  const uint64_t inv_magic = setup_once_rtc.inv_magic;
  setup_once_rtc.magic = 0U;
  setup_once_rtc.inv_magic = 0U;
  return esp_reset_reason() == ESP_RST_SW &&
      magic == SETUP_ONCE_MAGIC &&
      inv_magic == ~SETUP_ONCE_MAGIC;
}

static uint64_t monotonic_ms(void) {
  return (uint64_t)(esp_timer_get_time() / 1000);
}

static void init_gpio_controls(void) {
  const gpio_config_t button_cfg = {
    .pin_bit_mask = UINT64_C(1) << HAVPE_BUTTON_GPIO,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  const gpio_config_t mute_cfg = {
    .pin_bit_mask = UINT64_C(1) << HAVPE_MUTE_GPIO,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  (void)gpio_config(&button_cfg);
  (void)gpio_config(&mute_cfg);
}

static bool read_button_pressed(void) {
  return gpio_get_level(HAVPE_BUTTON_GPIO) == 0;
}

static bool read_muted(void) {
  return gpio_get_level(HAVPE_MUTE_GPIO) != 0;
}

static uint8_t read_saved_volume(void) {
  nvs_handle_t handle;
  uint8_t kept = 100U;
  if (nvs_open("kit", NVS_READONLY, &handle) == ESP_OK) {
    if (nvs_get_u8(handle, "volume", &kept) != ESP_OK || kept > 100U) {
      kept = 100U;
    }
    nvs_close(handle);
  }
  return kept;
}

static void init_identity_base(
    enum iterate_kit_boot_mode mode,
    const struct iterate_kit_configuration *stored_cfg,
    const struct iterate_kit_configuration_presence *stored_presence) {
  uint8_t mac[6] = {0};

  __atomic_store_n(&portal_writes_latched, false, __ATOMIC_SEQ_CST);
  memset(&portal, 0, sizeof(portal));
  portal.lock = xSemaphoreCreateMutex();
  portal.mode = mode;
  if (stored_cfg != NULL) portal.config = *stored_cfg;
  if (stored_presence != NULL) portal.presence = *stored_presence;
  portal.volume_percent = read_saved_volume();

  (void)esp_efuse_mac_get_default(mac);
  iterate_kit_device_short_id(mac, portal.short_id);
  (void)iterate_kit_device_hostname(portal.short_id, portal.hostname, sizeof(portal.hostname));

  iterate_kit_portal_guard_init(
      &portal.guard, portal.short_id, mode == ITERATE_KIT_BOOT_MODE_SETUP_AP);

  ESP_LOGI(
      tag, "mode=%s id=%s hostname=%s.local fw=%s",
      iterate_kit_boot_mode_name(mode),
      portal.short_id,
      portal.hostname,
      esp_app_get_description()->version);
}

/*
 * HARDWARE RNG ENTROPY ORDERING.
 *
 * On ESP32-S3, `esp_random()` / `esp_fill_random()` return pseudo-random data
 * unless a hardware noise source is active: either Wi-Fi/BT RF is started
 * (`esp_wifi_start()`), or `bootloader_random_enable()` is active.
 *
 * We start Wi-Fi RF first rather than calling `bootloader_random_enable()`: on
 * ESP32-S3 (`bootloader_random_esp32s3.c`), `bootloader_random_enable()` does
 * not touch I2S (that conflict is original ESP32 only), but it reconfigures SAR
 * ADC units 1 & 2 and `regi2c_saradc`, which must never overlap with Wi-Fi RF
 * or any ADC user. Starting Wi-Fi RF first gives true RF noise to
 * `esp_fill_random()` in all three boot modes without touching SAR ADC or audio.
 *
 * Both the permanent SoftAP WPA2 passphrase (loaded/created ONLY in Setup AP
 * mode) and every per-client session cookie, per-session CSRF token, and visual
 * pairing pattern require `portal.rf_entropy_ready == true`, which is set ONLY
 * after `esp_wifi_start()` succeeds in each boot mode.
 */
static void init_setup_ap_passphrase_with_rf_active(void) {
  char qr[ITERATE_KIT_WIFI_QR_CAPACITY];
  if (!portal.rf_entropy_ready) {
    ESP_LOGE(tag, "refusing to initialize setup code before Wi-Fi RF HWRNG is active");
    return;
  }
  if (!iterate_kit_platform_setup_code_load_or_create(portal.setup_code)) {
    ESP_LOGE(tag, "failed to load/create setup code");
    return;
  }
  if (iterate_kit_wifi_qr_format(portal.hostname, portal.setup_code, qr, sizeof(qr))) {
    ESP_LOGI(tag, "setup_ap ssid=%s setup_code=%s", portal.hostname, portal.setup_code);
    ESP_LOGI(tag, "setup_qr %s", qr);
  }
  memset(qr, 0, sizeof(qr));
}

static bool fill_session_entropy_locked(
    uint8_t sid_entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES],
    uint8_t csrf_entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES],
    uint8_t *pattern_byte) {
  if (!portal.rf_entropy_ready) {
    ESP_LOGE(tag, "refusing to mint tokens before Wi-Fi RF HWRNG is active");
    return false;
  }
  esp_fill_random(sid_entropy, ITERATE_KIT_SESSION_ENTROPY_BYTES);
  esp_fill_random(csrf_entropy, ITERATE_KIT_CSRF_ENTROPY_BYTES);
  if (pattern_byte != NULL) {
    esp_fill_random(pattern_byte, 1U);
  }
  return true;
}

static void flash_confirm_and_restart(
    struct iterate_kit_rgb8 colour, const char *note) {
  struct iterate_kit_rgb8 on[ITERATE_KIT_CONVERSATION_LIGHT_COUNT];
  struct iterate_kit_rgb8 off[ITERATE_KIT_CONVERSATION_LIGHT_COUNT];
  memset(off, 0, sizeof(off));
  for (size_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) on[i] = colour;
  for (int pulse = 0; pulse < 3; ++pulse) {
    iterate_kit_led_ring_show_pixels(on, 200U);
    (void)iterate_kit_led_ring_present(NULL, esp_timer_get_time());
    vTaskDelay(pdMS_TO_TICKS(160));
    iterate_kit_led_ring_show_pixels(off, 200U);
    (void)iterate_kit_led_ring_present(NULL, esp_timer_get_time());
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  iterate_kit_platform_restart_with_note(note);
}

static void halt_with_erase_failure(const char *action) {
  ESP_LOGE(
      tag, "%s FAILED erase or read-back verification — credentials may remain on flash; "
           "showing fault ring instead of success",
      action);
  for (;;) {
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT];
    havpe_ring_render_erase_error((uint32_t)monotonic_ms(), pixels);
    iterate_kit_led_ring_show_pixels(pixels, 200U);
    (void)iterate_kit_led_ring_present(NULL, esp_timer_get_time());
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

/*
 * NON-BLOCKING PHYSICAL ACTIONS (NEVER WAIT FOR HTTP HANDLERS).
 *
 * We deliberately DO NOT call synchronous `httpd_stop()`, which would wait for
 * any stalled client socket. Instead:
 *   1. Latch `portal_writes_latched` atomically so any in-flight `read_post_body`
 *      aborts immediately.
 *   2. Acquire `portal.lock` — guaranteed not to block on network I/O because
 *      no HTTP handler ever holds `portal.lock` across `recv` or `send`.
 *   3. Permanently latch `guard.writes_disabled = true`, clear RAM copies, and
 *      run the verified flash/NVS erase WHILE HOLDING `portal.lock`.
 *   4. Release `portal.lock` with `writes_disabled` permanently latched so no
 *      late HTTP handler can ever write stale state back to flash.
 */
static void execute_enter_setup(void) {
  ESP_LOGI(tag, "physical Enter Setup Mode triggered — keeping Wi-Fi and Mind intact");
  __atomic_store_n(&portal_writes_latched, true, __ATOMIC_SEQ_CST);
  if (portal.lock != NULL) (void)xSemaphoreTake(portal.lock, portMAX_DELAY);
  iterate_kit_portal_guard_disable_writes(&portal.guard);
  portal.reboot_pending = false;
  arm_setup_ap_once_rtc();
  if (portal.lock != NULL) xSemaphoreGive(portal.lock);
  flash_confirm_and_restart((struct iterate_kit_rgb8){44U, 0U, 56U}, "enter-setup-ap");
}

static void execute_wifi_reset(void) {
  ESP_LOGW(tag, "Wi-Fi reset triggered — latching writes off, clearing RAM and Wi-Fi flash");
  __atomic_store_n(&portal_writes_latched, true, __ATOMIC_SEQ_CST);
  if (portal.lock != NULL) (void)xSemaphoreTake(portal.lock, portMAX_DELAY);
  iterate_kit_portal_guard_disable_writes(&portal.guard);
  portal.reboot_pending = false;
  iterate_kit_configuration_clear_wifi(&portal.config, &portal.presence);
  const bool ok = iterate_kit_platform_clear_wifi_provisioning();
  if (portal.lock != NULL) xSemaphoreGive(portal.lock);
  if (!ok) {
    halt_with_erase_failure("Wi-Fi reset");
  }
  flash_confirm_and_restart((struct iterate_kit_rgb8){0U, 36U, 48U}, "wifi-reset");
}

static void execute_full_wipe(void) {
  ESP_LOGW(tag, "full wipe triggered — latching writes off, clearing RAM, flash, NVS token and volume");
  __atomic_store_n(&portal_writes_latched, true, __ATOMIC_SEQ_CST);
  if (portal.lock != NULL) (void)xSemaphoreTake(portal.lock, portMAX_DELAY);
  iterate_kit_portal_guard_disable_writes(&portal.guard);
  portal.reboot_pending = false;
  memset(&portal.config, 0, sizeof(portal.config));
  memset(&portal.presence, 0, sizeof(portal.presence));
  const bool ok = iterate_kit_platform_wipe_provisioning();
  if (portal.lock != NULL) xSemaphoreGive(portal.lock);
  if (!ok) {
    halt_with_erase_failure("full wipe");
  }
  flash_confirm_and_restart((struct iterate_kit_rgb8){48U, 4U, 0U}, "factory-wipe");
}

/* --- mDNS advertisement (`iterate-<id>.local`, `_http._tcp`, `_iterate._tcp`) --- */

static void start_mdns_once(void) {
  if (portal.mdns_started) return;
  if (mdns_init() != ESP_OK) {
    ESP_LOGE(tag, "mdns_init failed");
    return;
  }
  if (mdns_hostname_set(portal.hostname) != ESP_OK ||
      mdns_instance_name_set(portal.hostname) != ESP_OK) {
    ESP_LOGE(tag, "mdns hostname setup failed");
    return;
  }
  const char *fw = esp_app_get_description()->version;
  mdns_txt_item_t http_txt[] = {
    {"path", "/"},
    {"model", "havpe"},
    {"id", portal.short_id},
    {"fw", fw},
  };
  mdns_txt_item_t iterate_txt[] = {
    {"board", "home-assistant-voice-preview-edition"},
    {"id", portal.short_id},
    {"fw", fw},
    {"proto", "1"},
    {"mode", iterate_kit_boot_mode_name(portal.mode)},
  };
  (void)mdns_service_add(
      portal.hostname, "_http", "_tcp", 80, http_txt,
      sizeof(http_txt) / sizeof(http_txt[0]));
  (void)mdns_service_add(
      portal.hostname, "_iterate", "_tcp", 80, iterate_txt,
      sizeof(iterate_txt) / sizeof(iterate_txt[0]));
  portal.mdns_started = true;
  ESP_LOGI(
      tag, "mDNS advertising http://%s.local/ (_http._tcp, _iterate._tcp)",
      portal.hostname);
}

/* --- Captive DNS server task (Setup AP mode only, UDP/53 -> 192.168.4.1) --- */

static void captive_dns_task(void *context) {
  static const uint8_t ap_ip[4] = {192, 168, 4, 1};
  uint8_t rx[512];
  uint8_t tx[512];
  struct sockaddr_in bind_addr;
  int sock;
  (void)context;

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(tag, "captive DNS socket failed: errno=%d", errno);
    vTaskDelete(NULL);
    return;
  }
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(53);
  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(tag, "captive DNS bind(:53) failed: errno=%d", errno);
    close(sock);
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(tag, "captive DNS listening on UDP/53 -> 192.168.4.1");

  for (;;) {
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const ssize_t received =
        recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&peer, &peer_len);
    if (received <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const size_t reply_len = iterate_kit_captive_dns_respond(
        rx, (size_t)received, ap_ip, tx, sizeof(tx));
    if (reply_len > 0U) {
      (void)sendto(sock, tx, reply_len, 0, (struct sockaddr *)&peer, peer_len);
    }
  }
}

/* --- HTTP server helpers & handlers --------------------------------------- */

static bool check_host_header(httpd_req_t *req) {
  char host[96] = {0};
  char ip_copy[20] = {0};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    return false;
  }
  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  (void)snprintf(ip_copy, sizeof(ip_copy), "%s", portal.ip_address);
  const bool allowed = iterate_kit_portal_guard_host_allowed(&portal.guard, host, ip_copy);
  xSemaphoreGive(portal.lock);
  return allowed;
}

static void read_session_cookie(
    httpd_req_t *req, char out_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY]) {
  char cookie_hdr[COOKIE_HEADER_MAX_BYTES];
  out_sid[0] = '\0';
  if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) == ESP_OK) {
    (void)iterate_kit_web_cookie_get(
        cookie_hdr, "kit_sid", out_sid, ITERATE_KIT_SESSION_TOKEN_CAPACITY);
  }
  memset(cookie_hdr, 0, sizeof(cookie_hdr));
}

static void format_set_cookie(
    const char sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY],
    char out[SET_COOKIE_MAX_BYTES]) {
  (void)snprintf(
      out, SET_COOKIE_MAX_BYTES, "kit_sid=%s; HttpOnly; SameSite=Strict; Path=/", sid);
}

static esp_err_t send_redirect(
    httpd_req_t *req, const char *location, const char *status, const char *set_cookie) {
  (void)httpd_resp_set_status(req, status);
  (void)httpd_resp_set_hdr(req, "Location", location);
  (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (set_cookie != NULL && set_cookie[0] != '\0') {
    (void)httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
  }
  return httpd_resp_send(req, "", 0);
}

/*
 * Bounded POST body reader with a hard monotonic wall-clock deadline.
 * Called ONLY AFTER Host, mode policy, write-latch, and session pre-checks pass,
 * and NEVER while holding `portal.lock`. Returning ESP_FAIL closes stalled
 * connections immediately instead of looping forever on timeouts.
 */
static esp_err_t read_post_body(
    httpd_req_t *req, char *buf, size_t max_body_bytes, size_t *out_len) {
  *out_len = 0U;
  if (req->content_len > max_body_bytes) {
    (void)httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
    return ESP_FAIL;
  }
  const uint64_t deadline_ms = monotonic_ms() + (uint64_t)POST_BODY_DEADLINE_MS;
  size_t remaining = req->content_len;
  size_t offset = 0U;
  while (remaining > 0U) {
    if (__atomic_load_n(&portal_writes_latched, __ATOMIC_SEQ_CST) ||
        monotonic_ms() >= deadline_ms) {
      memset(buf, 0, max_body_bytes + 1U);
      return ESP_FAIL;
    }
    const int received = httpd_req_recv(req, buf + offset, remaining);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT && monotonic_ms() < deadline_ms) {
        continue;
      }
      memset(buf, 0, max_body_bytes + 1U);
      return ESP_FAIL;
    }
    offset += (size_t)received;
    remaining -= (size_t)received;
  }
  buf[offset] = '\0';
  *out_len = offset;
  return ESP_OK;
}

static void set_banner_locked(const char *session_token, const char *message, bool is_error) {
  if (session_token != NULL) {
    (void)snprintf(
        portal.banner_session_token, sizeof(portal.banner_session_token), "%s", session_token);
  } else {
    portal.banner_session_token[0] = '\0';
  }
  (void)snprintf(portal.banner_message, sizeof(portal.banner_message), "%s", message);
  portal.banner_is_error = is_error;
}

static void schedule_reboot_locked(const char *note, uint64_t now_ms) {
  portal.reboot_pending = true;
  portal.reboot_at_ms = now_ms + REBOOT_GRACE_MS;
  (void)snprintf(portal.reboot_note, sizeof(portal.reboot_note), "%s", note);
}

static const char *auth_error_message(enum iterate_kit_portal_auth_result auth) {
  switch (auth) {
    case ITERATE_KIT_PORTAL_AUTH_OK:
      return "";
    case ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED:
      return "Device is resetting or restarting. Changes are disabled.";
    case ITERATE_KIT_PORTAL_AUTH_NO_SESSION:
      return "Session expired. Reload the page and try again.";
    case ITERATE_KIT_PORTAL_AUTH_BAD_CSRF:
      return "Form token expired. Reload the page and try again.";
    case ITERATE_KIT_PORTAL_AUTH_REQUIRED:
      return "This browser is not paired. Slide Mute ON, tap the puck, and confirm the ring pattern.";
    case ITERATE_KIT_PORTAL_AUTH_NO_CLAIM:
      return "No pairing window open. Slide Mute ON and tap the centre button on the puck first.";
    case ITERATE_KIT_PORTAL_AUTH_CLAIM_BUSY:
      return "Another browser already requested pairing. If the ring does not match your screen, slide Mute OFF to cancel.";
  }
  return "Authorization failed.";
}

/*
 * Fast pre-check run BEFORE reading a POST request body so unauthenticated or
 * stalled requests from unknown clients are rejected immediately at the headers.
 */
static bool precheck_session_before_body(
    httpd_req_t *req,
    const char *cookie_sid,
    bool require_lan_unlock,
    bool require_claim_open,
    esp_err_t *resp_err) {
  if (__atomic_load_n(&portal_writes_latched, __ATOMIC_SEQ_CST)) {
    *resp_err = send_redirect(req, "/", "303 See Other", NULL);
    return false;
  }
  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(500)) != pdTRUE) {
    *resp_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
    return false;
  }
  const uint64_t now = monotonic_ms();
  const char *err_msg = NULL;
  if (iterate_kit_portal_guard_writes_disabled(&portal.guard)) {
    err_msg = auth_error_message(ITERATE_KIT_PORTAL_AUTH_WRITES_DISABLED);
  } else if (!iterate_kit_portal_guard_session_exists(&portal.guard, cookie_sid)) {
    err_msg = auth_error_message(ITERATE_KIT_PORTAL_AUTH_NO_SESSION);
  } else if (require_lan_unlock && !portal.guard.is_setup_ap &&
             !iterate_kit_portal_guard_session_is_unlocked(&portal.guard, cookie_sid, now)) {
    err_msg = auth_error_message(ITERATE_KIT_PORTAL_AUTH_REQUIRED);
  } else if (require_claim_open) {
    const enum iterate_kit_claim_state st =
        iterate_kit_portal_guard_claim_state(&portal.guard, now);
    if (st == ITERATE_KIT_CLAIM_NONE) {
      err_msg = auth_error_message(ITERATE_KIT_PORTAL_AUTH_NO_CLAIM);
    } else if (st == ITERATE_KIT_CLAIM_PENDING_CONFIRM) {
      err_msg = auth_error_message(ITERATE_KIT_PORTAL_AUTH_CLAIM_BUSY);
    }
  }
  if (err_msg != NULL) {
    if (cookie_sid[0] != '\0') set_banner_locked(cookie_sid, err_msg, true);
    xSemaphoreGive(portal.lock);
    *resp_err = send_redirect(req, "/", "303 See Other", NULL);
    return false;
  }
  xSemaphoreGive(portal.lock);
  return true;
}

static bool authorize_post_locked(
    const char *session_token,
    const char *body,
    size_t body_len,
    uint64_t now_ms,
    const char **error_out) {
  char csrf[ITERATE_KIT_CSRF_TOKEN_CAPACITY + 8] = {0};
  (void)iterate_kit_web_form_get_field(body, body_len, "csrf", csrf, sizeof(csrf));
  const enum iterate_kit_portal_auth_result auth =
      iterate_kit_portal_guard_authorize_post(&portal.guard, session_token, csrf, now_ms);
  memset(csrf, 0, sizeof(csrf));
  if (auth == ITERATE_KIT_PORTAL_AUTH_OK) return true;
  *error_out = auth_error_message(auth);
  return false;
}

static void build_status_snapshot_locked(
    struct iterate_kit_portal_status *out,
    const char *session_token,
    bool include_banner,
    uint64_t now_ms) {
  memset(out, 0, sizeof(*out));
  out->mode = portal.mode;
  (void)snprintf(out->short_id, sizeof(out->short_id), "%s", portal.short_id);
  (void)snprintf(out->hostname, sizeof(out->hostname), "%s", portal.hostname);
  (void)snprintf(
      out->firmware_version, sizeof(out->firmware_version), "%s",
      esp_app_get_description()->version);
  (void)snprintf(out->ip_address, sizeof(out->ip_address), "%s", portal.ip_address);
  out->wifi_connected = portal.wifi_connected;
  out->has_rssi = portal.has_rssi;
  out->wifi_rssi = portal.wifi_rssi;
  (void)snprintf(
      out->connection_state, sizeof(out->connection_state), "%s", portal.connection_state);
  out->key_refused = portal.key_refused;
  out->volume_percent = portal.volume_percent;
  out->unlocked_remaining_s =
      iterate_kit_portal_guard_session_unlock_remaining_s(&portal.guard, session_token, now_ms);
  out->claim_state = iterate_kit_portal_guard_claim_state(&portal.guard, now_ms);
  out->claim_remaining_s = iterate_kit_portal_guard_claim_remaining_s(&portal.guard, now_ms);
  out->claim_is_mine = iterate_kit_portal_guard_session_has_pending_claim(
      &portal.guard, session_token, now_ms, &out->claim_pattern);
  out->scan_results = portal.scan_results;
  out->scan_count = portal.scan_count;
  if (include_banner &&
      portal.banner_message[0] != '\0' &&
      session_token != NULL &&
      strcmp(portal.banner_session_token, session_token) == 0) {
    out->banner_message = portal.banner_message;
    out->banner_is_error = portal.banner_is_error;
  }
}

static esp_err_t handle_get_root(httpd_req_t *req) {
  char cookie_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char active_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char active_csrf[ITERATE_KIT_CSRF_TOKEN_CAPACITY];
  char set_cookie[SET_COOKIE_MAX_BYTES] = {0};
  uint8_t sid_entropy[ITERATE_KIT_SESSION_ENTROPY_BYTES];
  uint8_t csrf_entropy[ITERATE_KIT_CSRF_ENTROPY_BYTES];
  bool created_new = false;

  if (!check_host_header(req)) {
    if (portal.mode == ITERATE_KIT_BOOT_MODE_SETUP_AP) {
      return send_redirect(req, "http://192.168.4.1/", "302 Found", NULL);
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected Host header");
  }
  if (portal.mode == ITERATE_KIT_BOOT_MODE_SETUP_AP) {
    (void)esp_wifi_scan_start(NULL, false);
  }
  read_session_cookie(req, cookie_sid);
  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
  }
  if (!fill_session_entropy_locked(sid_entropy, csrf_entropy, NULL)) {
    xSemaphoreGive(portal.lock);
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "RNG not ready");
  }
  const uint64_t now = monotonic_ms();
  const bool session_ok = iterate_kit_portal_guard_ensure_session(
      &portal.guard, cookie_sid, sid_entropy, csrf_entropy, now,
      active_sid, active_csrf, &created_new);
  memset(sid_entropy, 0, sizeof(sid_entropy));
  memset(csrf_entropy, 0, sizeof(csrf_entropy));
  memset(cookie_sid, 0, sizeof(cookie_sid));
  if (!session_ok) {
    xSemaphoreGive(portal.lock);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Session error");
  }

  struct iterate_kit_portal_status status;
  build_status_snapshot_locked(&status, active_sid, true, now);
  const size_t len = iterate_kit_web_render_page(
      &portal.config, &portal.presence, &status, active_csrf,
      html_buffer, sizeof(html_buffer));
  if (status.banner_message != NULL) {
    portal.banner_session_token[0] = '\0';
    portal.banner_message[0] = '\0';
    portal.banner_is_error = false;
  }
  memset(active_csrf, 0, sizeof(active_csrf));
  if (created_new) {
    format_set_cookie(active_sid, set_cookie);
  }
  memset(active_sid, 0, sizeof(active_sid));
  /* Release `portal.lock` BEFORE network send so a slow client cannot block gestures. */
  xSemaphoreGive(portal.lock);

  if (len == 0U) {
    memset(set_cookie, 0, sizeof(set_cookie));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page overflow");
  }
  if (set_cookie[0] != '\0') {
    (void)httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
  }
  (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
  (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  (void)httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  const esp_err_t err = httpd_resp_send(req, html_buffer, (ssize_t)len);
  memset(set_cookie, 0, sizeof(set_cookie));
  return err;
}

static esp_err_t handle_get_status_json(httpd_req_t *req) {
  char cookie_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  if (!check_host_header(req)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected Host header");
  }
  read_session_cookie(req, cookie_sid);
  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
  }
  struct iterate_kit_portal_status status;
  build_status_snapshot_locked(&status, cookie_sid, false, monotonic_ms());
  memset(cookie_sid, 0, sizeof(cookie_sid));
  const size_t len = iterate_kit_web_render_status_json(
      &portal.config, &portal.presence, &status, json_buffer, sizeof(json_buffer));
  /* Release `portal.lock` BEFORE network send. */
  xSemaphoreGive(portal.lock);

  if (len == 0U) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON overflow");
  }
  (void)httpd_resp_set_type(req, "application/json");
  (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json_buffer, (ssize_t)len);
}

static esp_err_t handle_post_claim_request(httpd_req_t *req) {
  char body[POST_BODY_TINY_MAX + 1];
  char cookie_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  char new_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY] = {0};
  char set_cookie[SET_COOKIE_MAX_BYTES] = {0};
  uint8_t rot_sid[ITERATE_KIT_SESSION_ENTROPY_BYTES];
  uint8_t rot_csrf[ITERATE_KIT_CSRF_ENTROPY_BYTES];
  uint8_t pattern_byte = 0U;
  size_t body_len = 0U;
  esp_err_t pre_err = ESP_OK;

  if (!check_host_header(req)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected Host header");
  }
  if (portal.mode == ITERATE_KIT_BOOT_MODE_SETUP_AP) {
    return send_redirect(req, "/", "303 See Other", NULL);
  }
  read_session_cookie(req, cookie_sid);
  if (!precheck_session_before_body(req, cookie_sid, false, true, &pre_err)) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return pre_err;
  }
  if (read_post_body(req, body, POST_BODY_TINY_MAX, &body_len) != ESP_OK) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return ESP_FAIL;
  }

  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
    memset(body, 0, sizeof(body));
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
  }
  if (!fill_session_entropy_locked(rot_sid, rot_csrf, &pattern_byte)) {
    memset(body, 0, sizeof(body));
    memset(cookie_sid, 0, sizeof(cookie_sid));
    xSemaphoreGive(portal.lock);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "RNG not ready");
  }
  char csrf[ITERATE_KIT_CSRF_TOKEN_CAPACITY + 8] = {0};
  struct iterate_kit_claim_pattern pattern;
  (void)iterate_kit_web_form_get_field(body, body_len, "csrf", csrf, sizeof(csrf));
  const enum iterate_kit_portal_auth_result auth =
      iterate_kit_portal_guard_request_claim(
          &portal.guard, cookie_sid, csrf, pattern_byte,
          rot_sid, rot_csrf, monotonic_ms(), new_sid, &pattern);
  memset(rot_sid, 0, sizeof(rot_sid));
  memset(rot_csrf, 0, sizeof(rot_csrf));
  memset(csrf, 0, sizeof(csrf));
  memset(body, 0, sizeof(body));
  if (auth == ITERATE_KIT_PORTAL_AUTH_OK) {
    format_set_cookie(new_sid, set_cookie);
    set_banner_locked(
        new_sid, "Check the puck's LED ring and tap the centre button again to confirm.", false);
  } else {
    set_banner_locked(cookie_sid, auth_error_message(auth), true);
  }
  memset(cookie_sid, 0, sizeof(cookie_sid));
  memset(new_sid, 0, sizeof(new_sid));
  xSemaphoreGive(portal.lock);

  const esp_err_t err = send_redirect(req, "/", "303 See Other", set_cookie);
  memset(set_cookie, 0, sizeof(set_cookie));
  return err;
}

static esp_err_t handle_post_claim_cancel(httpd_req_t *req) {
  char body[POST_BODY_TINY_MAX + 1];
  char cookie_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  size_t body_len = 0U;
  esp_err_t pre_err = ESP_OK;

  if (!check_host_header(req)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected Host header");
  }
  if (portal.mode == ITERATE_KIT_BOOT_MODE_SETUP_AP) {
    return send_redirect(req, "/", "303 See Other", NULL);
  }
  read_session_cookie(req, cookie_sid);
  if (!precheck_session_before_body(req, cookie_sid, false, false, &pre_err)) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return pre_err;
  }
  if (read_post_body(req, body, POST_BODY_TINY_MAX, &body_len) != ESP_OK) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return ESP_FAIL;
  }

  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
    memset(body, 0, sizeof(body));
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
  }
  char csrf[ITERATE_KIT_CSRF_TOKEN_CAPACITY + 8] = {0};
  (void)iterate_kit_web_form_get_field(body, body_len, "csrf", csrf, sizeof(csrf));
  const enum iterate_kit_portal_auth_result auth =
      iterate_kit_portal_guard_cancel_claim_by_session(
          &portal.guard, cookie_sid, csrf, monotonic_ms());
  memset(csrf, 0, sizeof(csrf));
  memset(body, 0, sizeof(body));
  if (auth == ITERATE_KIT_PORTAL_AUTH_OK) {
    set_banner_locked(cookie_sid, "Pairing request cancelled.", false);
  } else {
    set_banner_locked(cookie_sid, auth_error_message(auth), true);
  }
  memset(cookie_sid, 0, sizeof(cookie_sid));
  xSemaphoreGive(portal.lock);
  return send_redirect(req, "/", "303 See Other", NULL);
}

static esp_err_t handle_post_config_action(
    httpd_req_t *req,
    enum iterate_kit_web_action action,
    size_t max_body_bytes,
    void (*apply_fn)(
        const char *, size_t,
        struct iterate_kit_configuration *,
        struct iterate_kit_configuration_presence *,
        struct iterate_kit_portal_action_result *),
    const char *reboot_note) {
  char body[POST_BODY_MIND_MAX + 1];
  char cookie_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  size_t body_len = 0U;
  esp_err_t pre_err = ESP_OK;

  if (!check_host_header(req)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected Host header");
  }
  if (!iterate_kit_web_action_allowed(portal.mode, action)) {
    return httpd_resp_send_err(
        req, HTTPD_403_FORBIDDEN,
        "This action is permitted only in encrypted Setup Mode (WPA2 SoftAP)");
  }
  read_session_cookie(req, cookie_sid);
  if (!precheck_session_before_body(
          req, cookie_sid, action == ITERATE_KIT_WEB_ACTION_SETTINGS, false, &pre_err)) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return pre_err;
  }
  if (max_body_bytes > POST_BODY_MIND_MAX) max_body_bytes = POST_BODY_MIND_MAX;
  if (read_post_body(req, body, max_body_bytes, &body_len) != ESP_OK) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return ESP_FAIL;
  }

  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
    memset(body, 0, sizeof(body));
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
  }
  const uint64_t now = monotonic_ms();
  const char *auth_err = NULL;
  if (!authorize_post_locked(cookie_sid, body, body_len, now, &auth_err)) {
    set_banner_locked(cookie_sid, auth_err, true);
    memset(body, 0, sizeof(body));
    memset(cookie_sid, 0, sizeof(cookie_sid));
    xSemaphoreGive(portal.lock);
    return send_redirect(req, "/", "303 See Other", NULL);
  }

  struct iterate_kit_portal_action_result result;
  apply_fn(body, body_len, &portal.config, &portal.presence, &result);
  memset(body, 0, sizeof(body));

  if (!result.ok) {
    set_banner_locked(cookie_sid, result.message, true);
    memset(cookie_sid, 0, sizeof(cookie_sid));
    xSemaphoreGive(portal.lock);
    return send_redirect(req, "/", "303 See Other", NULL);
  }

  if (result.apply_volume) {
    portal.volume_percent = result.new_volume;
    if (portal.mode == ITERATE_KIT_BOOT_MODE_NORMAL) {
      (void)iterate_kit_board_set_volume(result.new_volume, &portal.volume_percent);
    } else {
      iterate_kit_board_save_volume(result.new_volume);
    }
  }

  if (result.config_changed) {
    if (result.clear_refresh_token && !iterate_kit_platform_refresh_token_clear()) {
      (void)iterate_kit_platform_read_stored_provisioning(&portal.config, &portal.presence);
      set_banner_locked(cookie_sid, "Failed to clear prior refresh token in NVS.", true);
      memset(cookie_sid, 0, sizeof(cookie_sid));
      xSemaphoreGive(portal.lock);
      return send_redirect(req, "/", "303 See Other", NULL);
    }
    if (!iterate_kit_platform_write_provisioning(&portal.config, &portal.presence)) {
      (void)iterate_kit_platform_read_stored_provisioning(&portal.config, &portal.presence);
      set_banner_locked(cookie_sid, "Failed to write configuration to flash.", true);
      memset(cookie_sid, 0, sizeof(cookie_sid));
      xSemaphoreGive(portal.lock);
      return send_redirect(req, "/", "303 See Other", NULL);
    }
  }

  set_banner_locked(cookie_sid, result.message, false);
  memset(cookie_sid, 0, sizeof(cookie_sid));
  if (result.reboot_needed && portal.mode == ITERATE_KIT_BOOT_MODE_SETUP_AP) {
    __atomic_store_n(&portal_writes_latched, true, __ATOMIC_SEQ_CST);
    iterate_kit_portal_guard_disable_writes(&portal.guard);
    schedule_reboot_locked(reboot_note, now);
  }
  xSemaphoreGive(portal.lock);
  return send_redirect(req, "/", "303 See Other", NULL);
}

static esp_err_t handle_post_wifi(httpd_req_t *req) {
  return handle_post_config_action(
      req, ITERATE_KIT_WEB_ACTION_WIFI, POST_BODY_WIFI_MAX,
      iterate_kit_web_apply_wifi_form, "wifi-updated");
}

static esp_err_t handle_post_mind(httpd_req_t *req) {
  return handle_post_config_action(
      req, ITERATE_KIT_WEB_ACTION_MIND, POST_BODY_MIND_MAX,
      iterate_kit_web_apply_mind_form, "mind-updated");
}

static esp_err_t handle_post_settings(httpd_req_t *req) {
  return handle_post_config_action(
      req, ITERATE_KIT_WEB_ACTION_SETTINGS, POST_BODY_SETTINGS_MAX,
      iterate_kit_web_apply_settings_form, "settings-updated");
}

static esp_err_t handle_post_reboot(httpd_req_t *req) {
  char body[POST_BODY_TINY_MAX + 1];
  char cookie_sid[ITERATE_KIT_SESSION_TOKEN_CAPACITY];
  size_t body_len = 0U;
  esp_err_t pre_err = ESP_OK;

  if (!check_host_header(req)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected Host header");
  }
  if (!iterate_kit_web_action_allowed(portal.mode, ITERATE_KIT_WEB_ACTION_REBOOT)) {
    return httpd_resp_send_err(
        req, HTTPD_403_FORBIDDEN, "Reboot is permitted only in encrypted Setup Mode");
  }
  read_session_cookie(req, cookie_sid);
  if (!precheck_session_before_body(req, cookie_sid, false, false, &pre_err)) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return pre_err;
  }
  if (read_post_body(req, body, POST_BODY_TINY_MAX, &body_len) != ESP_OK) {
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return ESP_FAIL;
  }

  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
    memset(body, 0, sizeof(body));
    memset(cookie_sid, 0, sizeof(cookie_sid));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
  }
  const uint64_t now = monotonic_ms();
  const char *auth_err = NULL;
  if (!authorize_post_locked(cookie_sid, body, body_len, now, &auth_err)) {
    set_banner_locked(cookie_sid, auth_err, true);
    memset(body, 0, sizeof(body));
    memset(cookie_sid, 0, sizeof(cookie_sid));
    xSemaphoreGive(portal.lock);
    return send_redirect(req, "/", "303 See Other", NULL);
  }
  memset(body, 0, sizeof(body));
  __atomic_store_n(&portal_writes_latched, true, __ATOMIC_SEQ_CST);
  iterate_kit_portal_guard_disable_writes(&portal.guard);
  set_banner_locked(cookie_sid, "Exiting Setup Mode and rejoining Wi-Fi.", false);
  memset(cookie_sid, 0, sizeof(cookie_sid));
  schedule_reboot_locked("exit-setup-ap", now);
  xSemaphoreGive(portal.lock);
  return send_redirect(req, "/", "303 See Other", NULL);
}

static esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t err) {
  (void)err;
  if (portal.mode == ITERATE_KIT_BOOT_MODE_SETUP_AP ||
      iterate_kit_web_is_captive_probe(req->uri)) {
    return send_redirect(req, "http://192.168.4.1/", "302 Found", NULL);
  }
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

static bool start_http_server(void) {
  if (portal.server != NULL) return true;
  if (!portal.rf_entropy_ready) {
    ESP_LOGE(tag, "refusing to start HTTP server before Wi-Fi RF HWRNG is active");
    return false;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.task_priority = 3;
  config.core_id = 0;
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 2;
  config.send_wait_timeout = 2;
  config.max_uri_handlers = 10;

  if (httpd_start(&portal.server, &config) != ESP_OK) {
    ESP_LOGE(tag, "httpd_start failed");
    return false;
  }

  static const httpd_uri_t routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = handle_get_root},
    {.uri = "/api/status", .method = HTTP_GET, .handler = handle_get_status_json},
    {.uri = "/claim-request", .method = HTTP_POST, .handler = handle_post_claim_request},
    {.uri = "/claim-cancel", .method = HTTP_POST, .handler = handle_post_claim_cancel},
    {.uri = "/wifi", .method = HTTP_POST, .handler = handle_post_wifi},
    {.uri = "/mind", .method = HTTP_POST, .handler = handle_post_mind},
    {.uri = "/settings", .method = HTTP_POST, .handler = handle_post_settings},
    {.uri = "/reboot", .method = HTTP_POST, .handler = handle_post_reboot},
  };
  for (size_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    (void)httpd_register_uri_handler(portal.server, &routes[i]);
  }
  (void)httpd_register_err_handler(portal.server, HTTPD_404_NOT_FOUND, handle_not_found);
  ESP_LOGI(tag, "HTTP portal started on port 80");
  return true;
}

/* --- Wi-Fi event helpers for Setup AP & LAN Unlinked modes ---------------- */

static void update_scan_results(void) {
  static wifi_ap_record_t records[ITERATE_KIT_WEB_SCAN_MAX];
  uint16_t count = ITERATE_KIT_WEB_SCAN_MAX;
  if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) return;
  if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(500)) != pdTRUE) return;
  portal.scan_count = 0U;
  for (uint16_t i = 0U; i < count && portal.scan_count < ITERATE_KIT_WEB_SCAN_MAX; ++i) {
    const char *ssid = (const char *)records[i].ssid;
    if (ssid[0] == '\0') continue;
    bool duplicate = false;
    for (size_t j = 0U; j < portal.scan_count; ++j) {
      if (strcmp(portal.scan_results[j].ssid, ssid) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    struct iterate_kit_wifi_scan_entry *dst = &portal.scan_results[portal.scan_count++];
    (void)snprintf(dst->ssid, sizeof(dst->ssid), "%s", ssid);
    dst->rssi = records[i].rssi;
    dst->open = records[i].authmode == WIFI_AUTH_OPEN;
  }
  xSemaphoreGive(portal.lock);
  ESP_LOGI(tag, "Wi-Fi scan found %u unique 2.4 GHz networks", (unsigned)portal.scan_count);
}

static void setup_ap_wifi_event(
    void *arg, esp_event_base_t base, int32_t event_id, void *data) {
  (void)arg;
  (void)base;
  (void)data;
  if (event_id == WIFI_EVENT_SCAN_DONE) {
    update_scan_results();
  }
}

static void lan_unlinked_wifi_event(
    void *arg, esp_event_base_t base, int32_t event_id, void *data) {
  (void)arg;
  (void)base;
  (void)data;
  if (event_id == WIFI_EVENT_STA_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
      portal.wifi_connected = false;
      portal.has_rssi = false;
      portal.ip_address[0] = '\0';
      (void)snprintf(portal.connection_state, sizeof(portal.connection_state), "wifi_connecting");
      xSemaphoreGive(portal.lock);
    }
    (void)esp_wifi_connect();
  }
}

static void lan_unlinked_ip_event(
    void *arg, esp_event_base_t base, int32_t event_id, void *data) {
  (void)arg;
  (void)base;
  if (event_id == IP_EVENT_STA_GOT_IP && data != NULL) {
    const ip_event_got_ip_t *got = data;
    wifi_ap_record_t ap_info;
    if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
      portal.wifi_connected = true;
      (void)snprintf(
          portal.ip_address, sizeof(portal.ip_address), IPSTR, IP2STR(&got->ip_info.ip));
      if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        portal.has_rssi = true;
        portal.wifi_rssi = ap_info.rssi;
      }
      (void)snprintf(portal.connection_state, sizeof(portal.connection_state), "awaiting_mind");
      xSemaphoreGive(portal.lock);
    }
    ESP_LOGI(
        tag, "joined Wi-Fi (IP %s) — open http://%s.local/ for status; hold Mute+button 3 s to link Mind",
        portal.ip_address, portal.hostname);
  }
}

static void handle_gesture_and_claim_cancel(
    enum havpe_gesture_event event,
    enum havpe_gesture_visual vis,
    bool muted,
    uint64_t now_ms) {
  if (event == HAVPE_GESTURE_WIFI_RESET) {
    execute_wifi_reset();
  } else if (event == HAVPE_GESTURE_FULL_WIPE) {
    execute_full_wipe();
  } else if (event == HAVPE_GESTURE_ENTER_SETUP) {
    if (portal.mode != ITERATE_KIT_BOOT_MODE_SETUP_AP) {
      execute_enter_setup();
    }
  } else if (event == HAVPE_GESTURE_PAIR_TAP && portal.mode != ITERATE_KIT_BOOT_MODE_SETUP_AP) {
    bool confirmed = false;
    if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (iterate_kit_portal_guard_claim_state(&portal.guard, now_ms) ==
          ITERATE_KIT_CLAIM_PENDING_CONFIRM) {
        confirmed = iterate_kit_portal_guard_confirm_claim(&portal.guard, now_ms);
      } else {
        iterate_kit_portal_guard_open_claim(&portal.guard, now_ms);
      }
      xSemaphoreGive(portal.lock);
    }
    if (confirmed) {
      havpe_reset_gesture_trigger_unlock_flash(&portal.gesture, now_ms);
      ESP_LOGI(tag, "LAN browser visual pairing confirmed for 180 s");
    } else {
      ESP_LOGI(tag, "LAN browser pairing window opened for 30 s");
    }
    return;
  }

  /* "Anything else cancels": sliding Mute OFF or entering any hold cancels any claim. */
  if (portal.mode != ITERATE_KIT_BOOT_MODE_SETUP_AP &&
      (!muted || (vis != HAVPE_VISUAL_NONE && vis != HAVPE_VISUAL_UNLOCK_FLASH))) {
    if (xSemaphoreTake(portal.lock, 0) == pdTRUE) {
      if (portal.guard.claim_state != ITERATE_KIT_CLAIM_NONE) {
        iterate_kit_portal_guard_cancel_claim(&portal.guard);
        ESP_LOGI(tag, "pending LAN pairing cancelled");
      }
      xSemaphoreGive(portal.lock);
    }
  }
}

static bool render_claim_ring_overlay(
    uint64_t now_ms, struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]) {
  enum iterate_kit_claim_state state = ITERATE_KIT_CLAIM_NONE;
  struct iterate_kit_claim_pattern pattern = {0};
  if (xSemaphoreTake(portal.lock, 0) == pdTRUE) {
    state = iterate_kit_portal_guard_claim_state(&portal.guard, now_ms);
    if (state == ITERATE_KIT_CLAIM_PENDING_CONFIRM) {
      (void)iterate_kit_portal_guard_active_claim_pattern(&portal.guard, now_ms, &pattern);
    }
    xSemaphoreGive(portal.lock);
  }
  if (state == ITERATE_KIT_CLAIM_PENDING_CONFIRM) {
    havpe_ring_render_claim_pattern(pattern.color, pattern.position, (uint32_t)now_ms, pixels);
    return true;
  }
  if (state == ITERATE_KIT_CLAIM_OPEN) {
    havpe_ring_render_claim_open((uint32_t)now_ms, pixels);
    return true;
  }
  return false;
}

static void check_deferred_reboot(uint64_t now_ms) {
  char note[48] = {0};
  bool due = false;
  if (xSemaphoreTake(portal.lock, 0) == pdTRUE) {
    if (portal.reboot_pending && now_ms >= portal.reboot_at_ms) {
      due = true;
      (void)snprintf(note, sizeof(note), "%s", portal.reboot_note);
    }
    xSemaphoreGive(portal.lock);
  }
  if (due) {
    ESP_LOGI(tag, "restarting (%s)", note);
    iterate_kit_platform_restart_with_note(note);
  }
}

/* --- Public entry points -------------------------------------------------- */

void havpe_portal_run_setup_ap(
    const struct iterate_kit_led_ring *ring_facts,
    const struct iterate_kit_configuration *stored_cfg,
    const struct iterate_kit_configuration_presence *stored_presence) {
  init_identity_base(ITERATE_KIT_BOOT_MODE_SETUP_AP, stored_cfg, stored_presence);
  (void)snprintf(portal.ip_address, sizeof(portal.ip_address), "192.168.4.1");
  (void)snprintf(portal.connection_state, sizeof(portal.connection_state), "setup");
  init_gpio_controls();
  if (ring_facts != NULL) (void)iterate_kit_led_ring_start(ring_facts);

  (void)esp_netif_init();
  (void)esp_event_loop_create_default();
  (void)esp_netif_create_default_wifi_ap();
  (void)esp_netif_create_default_wifi_sta();

  wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
  (void)esp_wifi_init(&wifi_init);
  (void)esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_SCAN_DONE, setup_ap_wifi_event, NULL, NULL);
  (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

  /*
   * Bring up Wi-Fi RF in STA-only mode first so `esp_fill_random()` draws true
   * hardware RF entropy for the setup code WITHOUT beaconing an unconfigured
   * SoftAP, then stop, configure WPA2 APSTA, and start the AP.
   */
  (void)esp_wifi_set_mode(WIFI_MODE_STA);
  if (esp_wifi_start() == ESP_OK) {
    portal.rf_entropy_ready = true;
    init_setup_ap_passphrase_with_rf_active();
    portal.rf_entropy_ready = false;
    (void)esp_wifi_stop();
  } else {
    ESP_LOGE(tag, "esp_wifi_start(STA) failed; cannot initialize HWRNG setup code");
  }

  wifi_config_t ap_cfg = {0};
  (void)snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", portal.hostname);
  ap_cfg.ap.ssid_len = (uint8_t)strlen(portal.hostname);
  (void)snprintf(
      (char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", portal.setup_code);
  ap_cfg.ap.channel = 1;
  ap_cfg.ap.max_connection = 4;
  ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_cfg.ap.pmf_cfg.required = false;

  (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
  (void)esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
  memset(&ap_cfg, 0, sizeof(ap_cfg));
  if (esp_wifi_start() == ESP_OK) {
    portal.rf_entropy_ready = true;
    (void)esp_wifi_scan_start(NULL, false);
    (void)xTaskCreatePinnedToCore(
        captive_dns_task, "captive-dns", DNS_TASK_STACK_BYTES, NULL, 3, NULL, 0);
    (void)start_http_server();
  } else {
    ESP_LOGE(tag, "esp_wifi_start(APSTA) failed; HTTP portal not started");
  }

  for (;;) {
    const uint64_t now = monotonic_ms();
    const bool muted = read_muted();
    const enum havpe_gesture_event ev = havpe_reset_gesture_step(
        &portal.gesture, read_button_pressed(), muted, now);
    enum havpe_gesture_visual vis = HAVPE_VISUAL_NONE;
    uint8_t progress = 0U;
    havpe_reset_gesture_visual(&portal.gesture, now, &vis, &progress);
    handle_gesture_and_claim_cancel(ev, vis, muted, now);
    check_deferred_reboot(now);

    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT];
    if (vis != HAVPE_VISUAL_NONE) {
      havpe_ring_render_gesture(vis, progress, (uint32_t)now, pixels);
    } else {
      havpe_ring_render_setup_ap((uint32_t)now, pixels);
    }
    iterate_kit_led_ring_show_pixels(pixels, 150U);
    (void)iterate_kit_led_ring_present(NULL, esp_timer_get_time());
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void havpe_portal_run_lan_unlinked(
    const struct iterate_kit_led_ring *ring_facts,
    const struct iterate_kit_configuration *stored_cfg,
    const struct iterate_kit_configuration_presence *stored_presence) {
  init_identity_base(ITERATE_KIT_BOOT_MODE_LAN_UNLINKED, stored_cfg, stored_presence);
  (void)snprintf(portal.connection_state, sizeof(portal.connection_state), "wifi_connecting");
  init_gpio_controls();
  if (ring_facts != NULL) (void)iterate_kit_led_ring_start(ring_facts);

  (void)esp_netif_init();
  (void)esp_event_loop_create_default();
  (void)esp_netif_create_default_wifi_sta();

  wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
  wifi_config_t sta_cfg = {0};
  const size_t ssid_len = strnlen(portal.config.wifi_ssid, sizeof(sta_cfg.sta.ssid));
  const size_t pass_len = strnlen(portal.config.wifi_password, sizeof(sta_cfg.sta.password));
  memcpy(sta_cfg.sta.ssid, portal.config.wifi_ssid, ssid_len);
  memcpy(sta_cfg.sta.password, portal.config.wifi_password, pass_len);
  sta_cfg.sta.threshold.authmode =
      pass_len == 0U ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

  (void)esp_wifi_init(&wifi_init);
  (void)esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, lan_unlinked_wifi_event, NULL, NULL);
  (void)esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, lan_unlinked_ip_event, NULL, NULL);
  (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
  (void)esp_wifi_set_mode(WIFI_MODE_STA);
  (void)esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
  memset(&sta_cfg, 0, sizeof(sta_cfg));
  if (esp_wifi_start() == ESP_OK) {
    /* Wi-Fi RF is now active, so per-session/CSRF tokens and pairing patterns use HWRNG.
     * Note: the SoftAP setup code is never loaded or accepted in LAN modes. */
    portal.rf_entropy_ready = true;
    start_mdns_once();
    (void)start_http_server();
  } else {
    ESP_LOGE(tag, "esp_wifi_start(STA) failed; LAN portal not started");
  }

  for (;;) {
    const uint64_t now = monotonic_ms();
    const bool muted = read_muted();
    const enum havpe_gesture_event ev = havpe_reset_gesture_step(
        &portal.gesture, read_button_pressed(), muted, now);
    enum havpe_gesture_visual vis = HAVPE_VISUAL_NONE;
    uint8_t progress = 0U;
    havpe_reset_gesture_visual(&portal.gesture, now, &vis, &progress);
    handle_gesture_and_claim_cancel(ev, vis, muted, now);
    check_deferred_reboot(now);

    bool connected = false;
    if (xSemaphoreTake(portal.lock, 0) == pdTRUE) {
      connected = portal.wifi_connected;
      xSemaphoreGive(portal.lock);
    }
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT];
    if (vis != HAVPE_VISUAL_NONE) {
      havpe_ring_render_gesture(vis, progress, (uint32_t)now, pixels);
    } else if (!render_claim_ring_overlay(now, pixels)) {
      havpe_ring_render_lan_unlinked(connected, (uint32_t)now, pixels);
    }
    iterate_kit_led_ring_show_pixels(pixels, 150U);
    (void)iterate_kit_led_ring_present(NULL, esp_timer_get_time());
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void havpe_portal_prepare_normal(
    const struct iterate_kit_configuration *stored_cfg,
    const struct iterate_kit_configuration_presence *stored_presence) {
  init_identity_base(ITERATE_KIT_BOOT_MODE_NORMAL, stored_cfg, stored_presence);
  (void)snprintf(portal.connection_state, sizeof(portal.connection_state), "wifi_connecting");
}

bool havpe_portal_take_cancel_call(void) {
  return havpe_reset_gesture_take_cancel_call(&portal.gesture);
}

void havpe_portal_poll_normal(
    const struct iterate_kit_voice_view *view,
    bool button_pressed,
    bool muted,
    uint64_t now_ms) {
  if (portal.lock == NULL) return;

  const enum havpe_gesture_event ev =
      havpe_reset_gesture_step(&portal.gesture, button_pressed, muted, now_ms);
  enum havpe_gesture_visual vis = HAVPE_VISUAL_NONE;
  uint8_t progress = 0U;
  havpe_reset_gesture_visual(&portal.gesture, now_ms, &vis, &progress);
  handle_gesture_and_claim_cancel(ev, vis, muted, now_ms);
  check_deferred_reboot(now_ms);

  struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT];
  if (vis != HAVPE_VISUAL_NONE) {
    havpe_ring_render_gesture(vis, progress, (uint32_t)now_ms, pixels);
    iterate_kit_led_ring_show_pixels(pixels, 120U);
  } else if (render_claim_ring_overlay(now_ms, pixels)) {
    iterate_kit_led_ring_show_pixels(pixels, 120U);
  }

  /* Sample STA IP / RSSI and lazily start mDNS + HTTP at most once per second. */
  if (now_ms - portal.last_net_poll_ms < 1000U) return;
  portal.last_net_poll_ms = now_ms;

  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info = {0};
  const bool has_ip =
      sta != NULL && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0U;
  wifi_ap_record_t ap_info;
  const bool has_ap = has_ip && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;

  if (xSemaphoreTake(portal.lock, 0) == pdTRUE) {
    portal.wifi_connected = has_ip;
    if (has_ip) {
      (void)snprintf(
          portal.ip_address, sizeof(portal.ip_address), IPSTR, IP2STR(&ip_info.ip));
    } else {
      portal.ip_address[0] = '\0';
    }
    portal.has_rssi = has_ap;
    if (has_ap) portal.wifi_rssi = ap_info.rssi;
    portal.volume_percent = iterate_kit_board_current_volume();
    if (view != NULL) {
      portal.key_refused =
          view->status != NULL && strstr(view->status, "key refused") != NULL;
      const char *state_word =
          view->fault ? "fault"
          : portal.key_refused ? "key_refused"
          : view->call_active ? "in_call"
          : view->link_ready ? "ready"
          : has_ip ? "connecting_mind"
          : "wifi_connecting";
      (void)snprintf(portal.connection_state, sizeof(portal.connection_state), "%s", state_word);
    }
    xSemaphoreGive(portal.lock);
  }

  if (has_ip && !portal.lan_services_started) {
    portal.lan_services_started = true;
    /* Wi-Fi STA is running and associated, so RF HWRNG entropy is active. */
    if (xSemaphoreTake(portal.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
      portal.rf_entropy_ready = true;
      xSemaphoreGive(portal.lock);
    }
    start_mdns_once();
    (void)start_http_server();
  }
}
