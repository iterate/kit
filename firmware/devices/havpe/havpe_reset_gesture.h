#ifndef ITERATE_KIT_HAVPE_RESET_GESTURE_H
#define ITERATE_KIT_HAVPE_RESET_GESTURE_H

#include "iterate/kit/conversation_lights.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Physical recovery and LAN-unlock gestures on the Home Assistant Voice PE.
 *
 * The HAVPE has no screen: only a centre button (GPIO0), a rotary dial and a
 * physical hardware mute switch (GPIO3). Separating Wi-Fi reset from full wipe
 * by BOTH the mute switch position AND hold duration makes an accidental full
 * wipe impossible when someone intends only to change Wi-Fi:
 *
 *   - Unmuted + hold centre button 5 s (5..10 s) -> WI-FI RESET
 *     Clears only Wi-Fi credentials, keeps mind credential, reboots to Setup AP.
 *     Holding past 10 s while unmuted cancels safely instead of escalating.
 *
 *   - Muted (slide switch ON) + hold centre button 10 s (10..15 s) -> FULL WIPE
 *     Erases Wi-Fi AND mind/project credentials + rotated token + volume, then
 *     reboots to Setup AP. Holding past 15 s cancels safely.
 *
 *   - Muted + short tap (< 1.5 s) -> LAN UNLOCK
 *     Opens a 3-minute administrative window on the local web UI without
 *     starting a voice call (the mute switch already inhibits calls).
 *
 * Flipping the mute switch mid-hold cancels the gesture until button release.
 * A button already held at boot (for example entering ROM download mode on
 * GPIO0) is ignored until a clean release first.
 */

enum {
  HAVPE_GESTURE_DEBOUNCE_MS = 30,
  HAVPE_GESTURE_SHOW_PROGRESS_MS = 1500,
  HAVPE_GESTURE_WIFI_RESET_MS = 5000,
  HAVPE_GESTURE_WIFI_RESET_MAX_MS = 10000,
  HAVPE_GESTURE_FULL_WIPE_MS = 10000,
  HAVPE_GESTURE_FULL_WIPE_MAX_MS = 15000,
  HAVPE_GESTURE_UNLOCK_FLASH_MS = 2000,
};

enum havpe_gesture_event {
  HAVPE_GESTURE_NONE = 0,
  HAVPE_GESTURE_LAN_UNLOCK,
  HAVPE_GESTURE_WIFI_RESET,
  HAVPE_GESTURE_FULL_WIPE,
};

enum havpe_gesture_visual {
  HAVPE_VISUAL_NONE = 0,
  HAVPE_VISUAL_WIFI_RESET_PROGRESS,
  HAVPE_VISUAL_WIFI_RESET_ARMED,
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

#ifdef __cplusplus
}
#endif

#endif
