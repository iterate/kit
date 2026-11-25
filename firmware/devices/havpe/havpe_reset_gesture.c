#include "havpe_reset_gesture.h"

#include <stddef.h>
#include <string.h>

enum havpe_gesture_event havpe_reset_gesture_step(
    struct havpe_reset_gesture *state,
    bool button_pressed,
    bool muted,
    uint64_t now_ms) {
  if (state == NULL) return HAVPE_GESTURE_NONE;

  if (!state->initialized) {
    state->initialized = true;
    state->raw_pressed = button_pressed;
    state->debounced_pressed = button_pressed;
    state->raw_changed_ms = now_ms;
    state->armed_after_release = !button_pressed;
    return HAVPE_GESTURE_NONE;
  }

  if (button_pressed != state->raw_pressed) {
    state->raw_pressed = button_pressed;
    state->raw_changed_ms = now_ms;
  }

  if (now_ms >= state->raw_changed_ms &&
      now_ms - state->raw_changed_ms >= HAVPE_GESTURE_DEBOUNCE_MS) {
    if (!state->armed_after_release) {
      if (!button_pressed) {
        state->debounced_pressed = false;
        state->armed_after_release = true;
      }
      return HAVPE_GESTURE_NONE;
    }

    if (button_pressed != state->debounced_pressed) {
      state->debounced_pressed = button_pressed;
      if (button_pressed) {
        state->holding = true;
        state->hold_muted = muted;
        state->hold_cancelled = false;
        state->cancel_call_sent = false;
        state->hold_started_ms = now_ms;
        return HAVPE_GESTURE_NONE;
      }

      /* Release edge: classify the completed hold. */
      if (state->holding) {
        const uint64_t held_ms =
            now_ms >= state->hold_started_ms ? now_ms - state->hold_started_ms : 0U;
        const bool was_muted = state->hold_muted;
        const bool cancelled = state->hold_cancelled;
        state->holding = false;
        state->hold_cancelled = false;
        if (cancelled) return HAVPE_GESTURE_NONE;

        if (!was_muted) {
          if (held_ms >= HAVPE_GESTURE_WIFI_RESET_MS &&
              held_ms < HAVPE_GESTURE_WIFI_RESET_MAX_MS) {
            return HAVPE_GESTURE_WIFI_RESET;
          }
          return HAVPE_GESTURE_NONE;
        }
        /* Muted holds: short tap unlocks LAN, 10..15 s triggers full wipe. */
        if (held_ms < HAVPE_GESTURE_SHOW_PROGRESS_MS) {
          state->unlock_flash_until_ms = now_ms + HAVPE_GESTURE_UNLOCK_FLASH_MS;
          return HAVPE_GESTURE_LAN_UNLOCK;
        }
        if (held_ms >= HAVPE_GESTURE_FULL_WIPE_MS &&
            held_ms < HAVPE_GESTURE_FULL_WIPE_MAX_MS) {
          return HAVPE_GESTURE_FULL_WIPE;
        }
        return HAVPE_GESTURE_NONE;
      }
    }
  }

  if (state->holding && !state->hold_cancelled) {
    /* Flipping the mute switch mid-hold cancels cleanly until release. */
    if (muted != state->hold_muted) {
      state->hold_cancelled = true;
    } else {
      const uint64_t held_ms =
          now_ms >= state->hold_started_ms ? now_ms - state->hold_started_ms : 0U;
      const uint64_t max_ms = state->hold_muted
          ? (uint64_t)HAVPE_GESTURE_FULL_WIPE_MAX_MS
          : (uint64_t)HAVPE_GESTURE_WIFI_RESET_MAX_MS;
      if (!state->hold_muted && !state->cancel_call_sent &&
          held_ms >= HAVPE_GESTURE_SHOW_PROGRESS_MS) {
        /* An unmuted down-edge started (or toggled) a call; end it once this
         * becomes a Wi-Fi reset hold so no call is left running. */
        state->cancel_call_sent = true;
        state->cancel_call_pending = true;
      }
      if (held_ms >= max_ms) {
        state->hold_cancelled = true;
      }
    }
  }

  return HAVPE_GESTURE_NONE;
}

bool havpe_reset_gesture_take_cancel_call(struct havpe_reset_gesture *state) {
  if (state == NULL || !state->cancel_call_pending) return false;
  state->cancel_call_pending = false;
  return true;
}

void havpe_reset_gesture_visual(
    const struct havpe_reset_gesture *state,
    uint64_t now_ms,
    enum havpe_gesture_visual *visual,
    uint8_t *progress_leds) {
  if (visual != NULL) *visual = HAVPE_VISUAL_NONE;
  if (progress_leds != NULL) *progress_leds = 0U;
  if (state == NULL) return;

  if (state->holding && !state->hold_cancelled && now_ms >= state->hold_started_ms) {
    const uint64_t held_ms = now_ms - state->hold_started_ms;
    if (held_ms < HAVPE_GESTURE_SHOW_PROGRESS_MS) return;

    if (!state->hold_muted) {
      if (held_ms >= HAVPE_GESTURE_WIFI_RESET_MS) {
        if (visual != NULL) *visual = HAVPE_VISUAL_WIFI_RESET_ARMED;
        if (progress_leds != NULL) *progress_leds = ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
        return;
      }
      const uint64_t span = HAVPE_GESTURE_WIFI_RESET_MS - HAVPE_GESTURE_SHOW_PROGRESS_MS;
      uint8_t lit = (uint8_t)(((held_ms - HAVPE_GESTURE_SHOW_PROGRESS_MS) *
          ITERATE_KIT_CONVERSATION_LIGHT_COUNT) / span);
      if (lit == 0U) lit = 1U;
      if (lit > ITERATE_KIT_CONVERSATION_LIGHT_COUNT) {
        lit = ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
      }
      if (visual != NULL) *visual = HAVPE_VISUAL_WIFI_RESET_PROGRESS;
      if (progress_leds != NULL) *progress_leds = lit;
      return;
    }

    if (held_ms >= HAVPE_GESTURE_FULL_WIPE_MS) {
      if (visual != NULL) *visual = HAVPE_VISUAL_FULL_WIPE_ARMED;
      if (progress_leds != NULL) *progress_leds = ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
      return;
    }
    const uint64_t span = HAVPE_GESTURE_FULL_WIPE_MS - HAVPE_GESTURE_SHOW_PROGRESS_MS;
    uint8_t lit = (uint8_t)(((held_ms - HAVPE_GESTURE_SHOW_PROGRESS_MS) *
        ITERATE_KIT_CONVERSATION_LIGHT_COUNT) / span);
    if (lit == 0U) lit = 1U;
    if (lit > ITERATE_KIT_CONVERSATION_LIGHT_COUNT) {
      lit = ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
    }
    if (visual != NULL) *visual = HAVPE_VISUAL_FULL_WIPE_PROGRESS;
    if (progress_leds != NULL) *progress_leds = lit;
    return;
  }

  if (state->unlock_flash_until_ms != 0U && now_ms < state->unlock_flash_until_ms) {
    if (visual != NULL) *visual = HAVPE_VISUAL_UNLOCK_FLASH;
    if (progress_leds != NULL) *progress_leds = ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
  }
}

void havpe_ring_render_setup_ap(
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]) {
  /* Rotating cyan/blue comet: one orbit every 1.8 s (150 ms per LED). */
  if (pixels == NULL) return;
  const uint8_t head =
      (uint8_t)((now_ms / 150U) % ITERATE_KIT_CONVERSATION_LIGHT_COUNT);
  for (uint8_t index = 0U; index < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++index) {
    const uint8_t behind = (uint8_t)((head + ITERATE_KIT_CONVERSATION_LIGHT_COUNT - index) %
        ITERATE_KIT_CONVERSATION_LIGHT_COUNT);
    if (behind == 0U) {
      pixels[index] = (struct iterate_kit_rgb8){0U, 24U, 36U};
    } else if (behind == 1U) {
      pixels[index] = (struct iterate_kit_rgb8){0U, 14U, 22U};
    } else if (behind == 2U) {
      pixels[index] = (struct iterate_kit_rgb8){0U, 6U, 10U};
    } else {
      pixels[index] = (struct iterate_kit_rgb8){0U, 2U, 4U};
    }
  }
}

static uint8_t triangle_wave(uint32_t now_ms, uint32_t period_ms, uint8_t lo, uint8_t hi) {
  const uint32_t half = period_ms / 2U;
  const uint32_t phase = now_ms % period_ms;
  const uint32_t up = phase < half ? phase : period_ms - phase;
  return (uint8_t)(lo + ((uint32_t)(hi - lo) * up) / half);
}

void havpe_ring_render_lan_unlinked(
    bool wifi_connected,
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]) {
  struct iterate_kit_rgb8 colour;
  if (pixels == NULL) return;
  if (!wifi_connected) {
    /* Joining Wi-Fi: breathing amber, matching the normal connecting ring. */
    const uint8_t level = triangle_wave(now_ms, 2400U, 4U, 14U);
    colour = (struct iterate_kit_rgb8){level, (uint8_t)(level / 3U), 0U};
  } else {
    /* On LAN, waiting for mind link: calm breathing teal-blue. */
    const uint8_t green = triangle_wave(now_ms, 3000U, 4U, 16U);
    const uint8_t blue = triangle_wave(now_ms, 3000U, 8U, 24U);
    colour = (struct iterate_kit_rgb8){0U, green, blue};
  }
  for (uint8_t index = 0U; index < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++index) {
    pixels[index] = colour;
  }
}

void havpe_ring_render_gesture(
    enum havpe_gesture_visual visual,
    uint8_t progress_leds,
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]) {
  if (pixels == NULL) return;
  memset(pixels, 0, sizeof(*pixels) * ITERATE_KIT_CONVERSATION_LIGHT_COUNT);

  switch (visual) {
    case HAVPE_VISUAL_NONE:
      return;
    case HAVPE_VISUAL_WIFI_RESET_PROGRESS:
      for (uint8_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) {
        pixels[i] = i < progress_leds
            ? (struct iterate_kit_rgb8){0U, 20U, 30U}
            : (struct iterate_kit_rgb8){0U, 2U, 3U};
      }
      return;
    case HAVPE_VISUAL_WIFI_RESET_ARMED: {
      const bool high = ((now_ms / 180U) & 1U) == 0U;
      const struct iterate_kit_rgb8 c = high
          ? (struct iterate_kit_rgb8){0U, 34U, 48U}
          : (struct iterate_kit_rgb8){0U, 6U, 10U};
      for (uint8_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) pixels[i] = c;
      return;
    }
    case HAVPE_VISUAL_FULL_WIPE_PROGRESS:
      for (uint8_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) {
        pixels[i] = i < progress_leds
            ? (struct iterate_kit_rgb8){32U, 2U, 0U}
            : (struct iterate_kit_rgb8){3U, 0U, 0U};
      }
      return;
    case HAVPE_VISUAL_FULL_WIPE_ARMED: {
      const bool high = ((now_ms / 140U) & 1U) == 0U;
      const struct iterate_kit_rgb8 c = high
          ? (struct iterate_kit_rgb8){48U, 6U, 0U}
          : (struct iterate_kit_rgb8){8U, 0U, 0U};
      for (uint8_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) pixels[i] = c;
      return;
    }
    case HAVPE_VISUAL_UNLOCK_FLASH: {
      const uint8_t g = triangle_wave(now_ms, 500U, 8U, 28U);
      const struct iterate_kit_rgb8 c = {0U, g, (uint8_t)(g / 3U)};
      for (uint8_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) pixels[i] = c;
      return;
    }
  }
}

void havpe_ring_render_erase_error(
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]) {
  if (pixels == NULL) return;
  const uint8_t phase = (uint8_t)((now_ms / 250U) & 1U);
  for (uint8_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) {
    if (((i + phase) & 1U) == 0U) {
      pixels[i] = (struct iterate_kit_rgb8){48U, 0U, 0U};
    } else {
      pixels[i] = (struct iterate_kit_rgb8){40U, 18U, 0U};
    }
  }
}
