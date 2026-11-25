#ifndef ITERATE_KIT_HAVPE_RESET_GESTURE_H
#define ITERATE_KIT_HAVPE_RESET_GESTURE_H

#include "iterate/kit/conversation_lights.h"
#include "iterate/kit/provisioning_state.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Physical setup, recovery, and browser-pairing gestures on the Home Assistant
 * Voice PE (no screen: centre button GPIO0, rotary dial, hardware mute GPIO3).
 *
 *   - Unmuted + hold centre button 5 s (5..10 s) -> WI-FI RESET (cyan)
 *     Clears only Wi-Fi credentials, keeps mind credential, reboots to Setup AP.
 *     Holding past 10 s while unmuted cancels safely instead of escalating.
 *
 *   - Muted (slide switch ON) + short tap (< 1.5 s) -> LAN PAIRING TAP
 *     Tap 1 opens a 30 s claim window; Tap 2 confirms the visual colour+quadrant
 *     pattern currently displayed on the ring for that browser only. Never
 *     starts a voice call (mute inhibits calls).
 *
 *   - Muted + hold centre button 3 s (3..6 s) -> ENTER SETUP MODE ONCE (violet)
 *     Reboots once into encrypted WPA2 SoftAP Setup Mode WITHOUT erasing saved
 *     Wi-Fi or Mind credentials (so the owner can link/relink a Mind or change
 *     Wi-Fi over WPA2). Releasing during 6..10 s is a safe dead-zone.
 *
 *   - Muted + hold centre button 10 s (10..15 s) -> FULL WIPE (red)
 *     Erases Wi-Fi AND mind/project credentials + rotated token + volume, then
 *     reboots to Setup AP. Holding past 15 s cancels safely.
 *
 * Flipping the mute switch mid-hold cancels the gesture until button release.
 * A button already held at boot (e.g. GPIO0 boot strap) is ignored until a
 * clean release first.
 */

enum {
  HAVPE_GESTURE_DEBOUNCE_MS = 30,
  HAVPE_GESTURE_SHOW_PROGRESS_MS = 1500,
  HAVPE_GESTURE_SETUP_MODE_MS = 3000,
  HAVPE_GESTURE_SETUP_MODE_MAX_MS = 6000,
  HAVPE_GESTURE_WIFI_RESET_MS = 5000,
  HAVPE_GESTURE_WIFI_RESET_MAX_MS = 10000,
  HAVPE_GESTURE_FULL_WIPE_MS = 10000,
  HAVPE_GESTURE_FULL_WIPE_MAX_MS = 15000,
  HAVPE_GESTURE_UNLOCK_FLASH_MS = 2000,
};

enum havpe_gesture_event {
  HAVPE_GESTURE_NONE = 0,
  HAVPE_GESTURE_PAIR_TAP,
  HAVPE_GESTURE_ENTER_SETUP,
  HAVPE_GESTURE_WIFI_RESET,
  HAVPE_GESTURE_FULL_WIPE,
};

enum havpe_gesture_visual {
  HAVPE_VISUAL_NONE = 0,
  HAVPE_VISUAL_WIFI_RESET_PROGRESS,
  HAVPE_VISUAL_WIFI_RESET_ARMED,
  HAVPE_VISUAL_SETUP_MODE_PROGRESS,
  HAVPE_VISUAL_SETUP_MODE_ARMED,
  HAVPE_VISUAL_FULL_WIPE_PROGRESS,
  HAVPE_VISUAL_FULL_WIPE_ARMED,
  HAVPE_VISUAL_UNLOCK_FLASH,
};

struct havpe_reset_gesture {
  bool initialized;
  bool armed_after_release;
  bool raw_pressed;
  bool debounced_pressed;
  uint64_t raw_changed_ms;
  bool holding;
  bool hold_muted;
  bool hold_cancelled;
  bool cancel_call_sent;
  bool cancel_call_pending;
  uint64_t hold_started_ms;
  uint64_t unlock_flash_until_ms;
};

/** Advance the gesture classifier; returns an edge event on button release. */
enum havpe_gesture_event havpe_reset_gesture_step(
    struct havpe_reset_gesture *state,
    bool button_pressed,
    bool muted,
    uint64_t now_ms);

/** Trigger the 2 s green confirmation pulse after Tap 2 confirms a browser claim. */
void havpe_reset_gesture_trigger_unlock_flash(
    struct havpe_reset_gesture *state, uint64_t now_ms);

/**
 * Consume the one-shot edge requesting `end_press` once an unmuted hold
 * reaches HAVPE_GESTURE_SHOW_PROGRESS_MS (1.5 s). Because `board.c` starts a
 * voice call on the centre button's down-edge for low-latency activation, this
 * cancels that opening/active call cleanly so a Wi-Fi reset hold (whether
 * completed, released early, or held past 10 s) never leaves a call running.
 */
bool havpe_reset_gesture_take_cancel_call(struct havpe_reset_gesture *state);

/** Current ring feedback state and 0..12 progress LEDs (pure query). */
void havpe_reset_gesture_visual(
    const struct havpe_reset_gesture *state,
    uint64_t now_ms,
    enum havpe_gesture_visual *visual,
    uint8_t *progress_leds);

/** Render the 12-pixel ring for Setup AP mode (rotating cyan/teal comet). */
void havpe_ring_render_setup_ap(
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]);

/**
 * Render the 12-pixel ring for LAN Awaiting-Mind mode: breathing amber while
 * joining Wi-Fi, calm breathing teal once on LAN waiting for mind link.
 */
void havpe_ring_render_lan_unlinked(
    bool wifi_connected,
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]);

/** Render a gesture progress/armed/unlock overlay onto the 12-pixel ring. */
void havpe_ring_render_gesture(
    enum havpe_gesture_visual visual,
    uint8_t progress_leds,
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]);

/** Render a calm white breathing ring while waiting for a browser to start pairing. */
void havpe_ring_render_claim_open(
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]);

/**
 * Render the visual verification pattern (3-LED quadrant arc in the claim's
 * colour) while waiting for the owner's confirming second tap on the board.
 */
void havpe_ring_render_claim_pattern(
    enum iterate_kit_claim_color color,
    enum iterate_kit_claim_position position,
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]);

/**
 * Render an unmistakable alternating red/amber fault ring when a Wi-Fi reset
 * or full wipe erase/read-back verification fails (never shows success flashes).
 */
void havpe_ring_render_erase_error(
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
