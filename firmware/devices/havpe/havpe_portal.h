#ifndef ITERATE_KIT_HAVPE_PORTAL_H
#define ITERATE_KIT_HAVPE_PORTAL_H

#include "iterate/kit/configuration.h"
#include "iterate/kit/platforms/led_ring.h"
#include "iterate/kit/voice/loop.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Consume the one-boot RTC flag set by `POST /enter-setup` on the LAN page so
 * the board boots once into encrypted Setup Mode (WPA2 SoftAP) while keeping
 * its saved Wi-Fi and Mind credentials intact. Returns true only after a
 * software restart that explicitly armed the flag.
 */
bool havpe_portal_consume_setup_ap_request(void);

/**
 * Run HAVPE Setup Mode (WPA2 SoftAP `iterate-<id>`, captive DNS on UDP/53,
 * captive portal HTTP server on TCP/80, rotating cyan LED ring, and physical
 * recovery gestures). Never returns: reboots once Wi-Fi credentials are saved
 * or a full wipe completes.
 */
void havpe_portal_run_setup_ap(
    const struct iterate_kit_led_ring *ring_facts,
    const struct iterate_kit_configuration *stored_cfg,
    const struct iterate_kit_configuration_presence *stored_presence);

/**
 * Run HAVPE LAN Awaiting-Mind Mode (joins saved Wi-Fi STA, advertises
 * `iterate-<id>.local` over mDNS with `_http._tcp` and `_iterate._tcp`, serves
 * the status/settings page on TCP/80, breathing teal LED ring, and physical
 * gestures). Never returns: reboots once a mind is linked or Wi-Fi is reset.
 */
void havpe_portal_run_lan_unlinked(
    const struct iterate_kit_led_ring *ring_facts,
    const struct iterate_kit_configuration *stored_cfg,
    const struct iterate_kit_configuration_presence *stored_presence);

/**
 * Prepare LAN portal state before entering the shared voice loop in Normal
 * Mode. mDNS and the tiny HTTP server start lazily once Wi-Fi STA gets an IP.
 */
void havpe_portal_prepare_normal(
    const struct iterate_kit_configuration *stored_cfg,
    const struct iterate_kit_configuration_presence *stored_presence);

/**
 * Called from `havpe_ui_present()` on the app task during Normal Mode:
 * - starts mDNS + LAN HTTP server once STA acquires an IPv4 lease;
 * - samples the centre button + mute switch for Wi-Fi reset, full wipe, and
 *   mute+tap LAN unlock;
 * - paints gesture progress/armed/unlock overlays on the LED ring;
 * - updates the live status snapshot and executes deferred reboots.
 */
void havpe_portal_poll_normal(
    const struct iterate_kit_voice_view *view,
    bool button_pressed,
    bool muted,
    uint64_t now_ms);

/**
 * Consume the one-shot call-cancel edge when an unmuted centre-button hold
 * enters Wi-Fi reset progress (>= 1.5 s), so `board.c` emits `end_press` and
 * never leaves a call running. App task only.
 */
bool havpe_portal_take_cancel_call(void);

#ifdef __cplusplus
}
#endif

#endif
