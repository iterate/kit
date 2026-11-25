#include "havpe_reset_gesture.h"
#include "iterate/kit/button.h"
#include "iterate/kit/session_grammar.h"

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

static void muted_tap_setup_mode_dead_zone_and_full_wipe_are_distinct(void) {
  struct havpe_reset_gesture g = {0};
  enum havpe_gesture_visual vis;
  uint8_t leds;

  assert(havpe_reset_gesture_step(&g, false, true, 0U) == HAVPE_GESTURE_NONE);

  /* 1) Muted short tap (< 1.5 s) -> PAIR_TAP (caller triggers unlock flash on Tap 2). */
  assert(havpe_reset_gesture_step(&g, true, true, 100U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 130U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 400U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 430U) == HAVPE_GESTURE_PAIR_TAP);
  havpe_reset_gesture_trigger_unlock_flash(&g, 430U);
  havpe_reset_gesture_visual(&g, 500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_UNLOCK_FLASH);
  havpe_reset_gesture_visual(&g, 2500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_NONE);

  /* 2) Muted 3.5 s hold -> ENTER_SETUP (violet progress 1.5..3 s, armed 3..6 s). */
  assert(havpe_reset_gesture_step(&g, true, true, 3000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 3030U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 5200U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_SETUP_MODE_PROGRESS && leds > 0U && leds < 12U);
  havpe_reset_gesture_visual(&g, 6500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_SETUP_MODE_ARMED && leds == 12U);
  assert(havpe_reset_gesture_step(&g, false, true, 6550U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 6580U) == HAVPE_GESTURE_ENTER_SETUP);

  /* 3) Muted 7.5 s hold (6..10 s dead-zone) -> red warning arc; release does NOTHING. */
  assert(havpe_reset_gesture_step(&g, true, true, 10000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 10030U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 17530U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_FULL_WIPE_PROGRESS && leds > 0U && leds < 12U);
  assert(havpe_reset_gesture_step(&g, false, true, 17550U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 17580U) == HAVPE_GESTURE_NONE);

  /* 4) Muted 10.5 s hold -> FULL_WIPE (red armed 10..15 s). */
  assert(havpe_reset_gesture_step(&g, true, true, 20000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 20030U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 30500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_FULL_WIPE_ARMED && leds == 12U);
  assert(havpe_reset_gesture_step(&g, false, true, 30600U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, true, 30630U) == HAVPE_GESTURE_FULL_WIPE);

  /* 5) Flipping mute switch mid-hold cancels cleanly. */
  assert(havpe_reset_gesture_step(&g, true, true, 40000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, true, 40030U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, true, false, 43500U) == HAVPE_GESTURE_NONE);
  havpe_reset_gesture_visual(&g, 43500U, &vis, &leds);
  assert(vis == HAVPE_VISUAL_NONE);
  assert(havpe_reset_gesture_step(&g, false, false, 44000U) == HAVPE_GESTURE_NONE);
  assert(havpe_reset_gesture_step(&g, false, false, 44030U) == HAVPE_GESTURE_NONE);
}

static void renders_distinct_ring_patterns_for_modes_gestures_and_claims(void) {
  struct iterate_kit_rgb8 ap0[12], ap1[12];
  struct iterate_kit_rgb8 lan_wait[12], lan_ready[12];
  struct iterate_kit_rgb8 wifi_arm[12], setup_arm[12], wipe_arm[12];
  struct iterate_kit_rgb8 claim_open[12], claim_pat[12];
  struct iterate_kit_rgb8 err0[12], err1[12];

  havpe_ring_render_setup_ap(0U, ap0);
  havpe_ring_render_setup_ap(300U, ap1);
  assert(memcmp(ap0, ap1, sizeof(ap0)) != 0);
  assert(ap0[0].red == 0U && ap0[0].blue > ap0[0].green);

  havpe_ring_render_lan_unlinked(false, 600U, lan_wait);
  havpe_ring_render_lan_unlinked(true, 600U, lan_ready);
  assert(lan_wait[0].red > 0U && lan_wait[0].blue == 0U);
  assert(lan_ready[0].red == 0U && lan_ready[0].blue > 0U);

  havpe_ring_render_gesture(HAVPE_VISUAL_WIFI_RESET_ARMED, 12U, 0U, wifi_arm);
  havpe_ring_render_gesture(HAVPE_VISUAL_SETUP_MODE_ARMED, 12U, 0U, setup_arm);
  havpe_ring_render_gesture(HAVPE_VISUAL_FULL_WIPE_ARMED, 12U, 0U, wipe_arm);
  /* Wi-Fi reset is cyan; Setup Mode is violet (red+blue, green==0); Full wipe is red. */
  assert(wifi_arm[0].red == 0U && wifi_arm[0].blue > 20U);
  assert(setup_arm[0].red > 20U && setup_arm[0].green == 0U && setup_arm[0].blue > 20U);
  assert(wipe_arm[0].red > 20U && wipe_arm[0].blue == 0U);

  /* Claim open is calm white; claim pattern lights ONLY the 3 quadrant LEDs in the claim colour. */
  havpe_ring_render_claim_open(300U, claim_open);
  assert(claim_open[0].red == claim_open[0].green && claim_open[0].green == claim_open[0].blue);
  havpe_ring_render_claim_pattern(
      ITERATE_KIT_CLAIM_COLOR_MAGENTA, ITERATE_KIT_CLAIM_POS_RIGHT, 500U, claim_pat);
  for (uint8_t i = 0U; i < 12U; ++i) {
    const bool in_right = (i == 2U || i == 3U || i == 4U);
    if (in_right) {
      assert(claim_pat[i].red > 20U && claim_pat[i].green == 0U && claim_pat[i].blue > 20U);
    } else {
      assert(claim_pat[i].red == 0U && claim_pat[i].green == 0U && claim_pat[i].blue == 0U);
    }
  }

  /* Erase failure ring alternates red and amber and flips phase over time. */
  havpe_ring_render_erase_error(0U, err0);
  havpe_ring_render_erase_error(250U, err1);
  assert(err0[0].red > 30U && err0[0].green == 0U);
  assert(err0[1].red > 30U && err0[1].green > 10U);
  assert(memcmp(err0, err1, sizeof(err0)) != 0);
}

/*
 * Mirror board.c's exact poll path: button.c produces `press` on the 30 ms
 * down-edge, havpe_reset_gesture produces `end_press` once an unmuted hold
 * reaches 1.5 s, and session_grammar turns those into `start_call`/`end_call`.
 */
struct call_harness {
  struct iterate_kit_button button;
  struct havpe_reset_gesture gesture;
  struct iterate_kit_session session;
  bool wants_call;
  bool call_active;
};

static enum havpe_gesture_event step_board_and_gesture(
    struct call_harness *h, bool pressed, bool muted, uint64_t now_ms) {
  const enum havpe_gesture_event ev =
      havpe_reset_gesture_step(&h->gesture, pressed, muted, now_ms);
  iterate_kit_button_update(&h->button, pressed, now_ms);
  const struct iterate_kit_session_poll poll = {
    .press = iterate_kit_button_take_press(&h->button),
    .end_press = havpe_reset_gesture_take_cancel_call(&h->gesture),
    .wants_call = h->wants_call,
    .call_active = h->call_active,
  };
  struct iterate_kit_session_actions actions;
  iterate_kit_session_step(&h->session, &poll, &actions);
  if (!muted && actions.start_call) {
    h->wants_call = true;
    h->call_active = true;
  }
  if (muted || actions.end_call) {
    h->wants_call = false;
    h->call_active = false;
  }
  return ev;
}

static void wifi_reset_hold_cancels_opening_call_and_never_leaves_one_running(void) {
  struct call_harness h;
  memset(&h, 0, sizeof(h));

  assert(step_board_and_gesture(&h, false, false, 0U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, false, false, 40U) == HAVPE_GESTURE_NONE);

  /* 1) Normal short tap (< 1.5 s) starts a call on down-edge and keeps it active. */
  assert(step_board_and_gesture(&h, true, false, 100U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, true, false, 130U) == HAVPE_GESTURE_NONE);
  assert(h.wants_call && h.call_active);
  assert(step_board_and_gesture(&h, false, false, 300U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, false, false, 330U) == HAVPE_GESTURE_NONE);
  assert(h.wants_call && h.call_active);

  /* 2) Unmuted hold starting while ALREADY in a call: down-edge ends it at 30 ms,
   *    and the 1.5 s end_press is a safe no-op that never starts a new call. */
  assert(step_board_and_gesture(&h, true, false, 1000U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, true, false, 1030U) == HAVPE_GESTURE_NONE);
  assert(!h.wants_call && !h.call_active);
  assert(step_board_and_gesture(&h, true, false, 2600U) == HAVPE_GESTURE_NONE);
  assert(!h.wants_call && !h.call_active);
  assert(step_board_and_gesture(&h, false, false, 3000U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, false, false, 3030U) == HAVPE_GESTURE_NONE);
  assert(!h.wants_call && !h.call_active);

  /* 3) Idle -> unmuted hold aborted at 3 s: down-edge starts call, 1.5 s cancels it. */
  assert(step_board_and_gesture(&h, true, false, 5000U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, true, false, 5030U) == HAVPE_GESTURE_NONE);
  assert(h.wants_call && h.call_active);
  assert(step_board_and_gesture(&h, true, false, 6530U) == HAVPE_GESTURE_NONE);
  assert(!h.wants_call && !h.call_active);
  assert(step_board_and_gesture(&h, false, false, 8000U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, false, false, 8030U) == HAVPE_GESTURE_NONE);
  assert(!h.wants_call && !h.call_active);

  /* 4) Idle -> full 5 s unmuted hold: call cancelled at 1.5 s, WIFI_RESET fires with no call. */
  assert(step_board_and_gesture(&h, true, false, 10000U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, true, false, 10030U) == HAVPE_GESTURE_NONE);
  assert(h.wants_call);
  assert(step_board_and_gesture(&h, true, false, 11600U) == HAVPE_GESTURE_NONE);
  assert(!h.wants_call && !h.call_active);
  assert(step_board_and_gesture(&h, true, false, 15200U) == HAVPE_GESTURE_NONE);
  assert(!h.wants_call && !h.call_active);
  assert(step_board_and_gesture(&h, false, false, 15300U) == HAVPE_GESTURE_NONE);
  assert(step_board_and_gesture(&h, false, false, 15330U) == HAVPE_GESTURE_WIFI_RESET);
  assert(!h.wants_call && !h.call_active);
}

int main(void) {
  ignores_boot_held_button_until_clean_release();
  unmuted_five_second_hold_resets_wifi_and_never_wipes();
  muted_tap_setup_mode_dead_zone_and_full_wipe_are_distinct();
  renders_distinct_ring_patterns_for_modes_gestures_and_claims();
  wifi_reset_hold_cancels_opening_call_and_never_leaves_one_running();
  return 0;
}
