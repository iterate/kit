#include "havpe_reset_gesture.h"

#include <assert.h>
#include <string.h>

static void ignores_boot_held_button_until_clean_release(void) {
  struct havpe_reset_gesture g = {0};
  enum havpe_gesture_visual vis;
  uint8_t leds;

  /* Button held at boot (e.g. GPIO0 strap) must never trigger reset or wipe. */
  assert(havpe_reset_gesture_step(&g, true, false, 0U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, false, 6000U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 6000U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_NONE);
  /* Clean release arms future gestures. */
  assert(havpe_reset_gesture_step(&g, false, false, 6050U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, false, 6100U) == HAVPE_GESTURE_NONE);
}

static void unmuted_five_second_hold_resets_wifi_and_never_wipes(void) {
  struct havpe_reset_gesture g = {0};
  enum havpe_gesture_visual vis;
  uint8_t leds;

  /* Initialize released. */
  assert(havpe_reset_gesture_step(&g, false, false, 0U) == HAVPE_GESTURE_NONE);

  /* Press down and debounce (30 ms). */
  assert(havpe_reset_gesture_step(&g, true, false, 100U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, false, 130U) == HAVPE_GESTURE_NONE);

  /* Before 1.5 s: no overlay. */
  havpe_reset_gesture_visual(&g, 1000U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_NONE);

  /* At 3 s: Wi-Fi reset progress bar (cyan). */
  havpe_reset_gesture_visual(&g, 3130U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_WIFI_RESET_PROGRESS && leds > 0U && leds < 12U);

  /* At 5.1 s: armed. */
  havpe_reset_gesture_visual(&g, 5230U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_WIFI_RESET_ARMED && leds == 12U);

  /* Release at 5.5 s -> WIFI_RESET. */
  assert(havpe_reset_gesture_step(&g, false, false, 5600U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, false, 5630U) == HAVPE_GESTURE_WIFI_RESET);

  /* Holding past 10 s while unmuted cancels safely instead of full wipe. */
  assert(havpe_reset_gesture_step(&g, true, false, 10000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, false, 10030U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, false, 20500U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 20500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_NONE);
  assert(havpe_reset_gesture_step(&g, false, false, 20600U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, false, 20630U) == HAVPE_GESTURE_NONE);
}

static void muted_tap_unlocks_and_ten_second_hold_full_wipes(void) {
  struct havpe_reset_gesture g = {0};
  enum havpe_gesture_visual vis;
  uint8_t leds;

  assert(havpe_reset_gesture_step(&g, false, true, 0U) == HAVPE_GESTURE_NONE);

  /* Muted short tap -> LAN_UNLOCK + 2 s green flash. */
  assert(havpe_reset_gesture_step(&g, true, true, 100U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 130U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 400U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 430U) == HAVPE_GESTURE_LAN_UNLOCK);
  havpe_reset_gesture_visual(&g, 500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_UNLOCK_FLASH);
  havpe_reset_gesture_visual(&g, 2500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_NONE);

  /* Muted 6 s hold released early -> NONE (does not Wi-Fi reset or wipe). */
  assert(havpe_reset_gesture_step(&g, true, true, 3000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 3030U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 9030U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_FULL_WIPE_PROGRESS && leds > 0U && leds < 12U);
  assert(havpe_reset_gesture_step(&g, false, true, 9050U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 9080U) == HAVPE_GESTURE_NONE);

  /* Muted 10.5 s hold -> FULL_WIPE. */
  assert(havpe_reset_gesture_step(&g, true, true, 10000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 10030U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 20500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_FULL_WIPE_ARMED && leds == 12U);
  assert(havpe_reset_gesture_step(&g, false, true, 20600U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 20630U) == HAVPE_GESTURE_FULL_WIPE);

  /* Flipping mute switch mid-hold cancels cleanly. */
  assert(havpe_reset_gesture_step(&g, true, true, 30000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 30030U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, false, 35000U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 35000U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_NONE);
  assert(havpe_reset_gesture_step(&g, false, false, 41000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, false, 41030U) == HAVPE_GESTURE_NONE);
}

static void renders_distinct_ring_patterns_for_modes_and_gestures(void) {
  struct iterate_kit_rgb8 ap0[12], ap1[12];
  struct iterate_kit_rgb8 lan_wait[12], lan_ready[12];
  struct iterate_kit_rgb8 wifi_arm[12], wipe_arm[12];

  havpe_ring_render_setup_ap(0U, ap0);
  havpe_ring_render_setup_ap(300U, ap1);
  /* Comet advances and is cyan (red == 0, blue > green). */
  assert(memcmp(ap0, ap1, sizeof(ap0)) != 0);
  assert(ap0[0].red == 0U && ap0[0].blue > ap0[0].green);

  havpe_ring_render_lan_unlinked(false, 600U, lan_wait);
  havpe_ring_render_lan_unlinked(true, 600U, lan_ready);
  /* Connecting is amber (red > 0, blue == 0); awaiting mind is teal (red == 0, blue > 0). */
  assert(lan_wait[0].red > 0U && lan_wait[0].blue == 0U);
  assert(lan_ready[0].red == 0U && lan_ready[0].blue > 0U);

  havpe_ring_render_gesture(HAVPE_VISUAL_WIFI_RESET_ARMED, 12U, 0U, wifi_arm);
  havpe_ring_render_gesture(HAVPE_VISUAL_FULL_WIPE_ARMED, 12U, 0U, wipe_arm);
  /* Wi-Fi reset is cyan; full wipe is red. */
  assert(wifi_arm[0].red == 0U && wifi_arm[0].blue > 20U);
  assert(wipe_arm[0].red > 20U && wipe_arm[0].blue == 0U);
}

int main(void) {
  ignores_boot_held_button_until_clean_release();
  unmuted_five_second_hold_resets_wifi_and_never_wipes();
  muted_tap_unlocks_and_ten_second_hold_full_wipes();
  renders_distinct_ring_patterns_for_modes_and_gestures();
  return 0;
}
