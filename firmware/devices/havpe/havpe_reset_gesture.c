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
        /* Muted holds:
         *  < 1.5 s    -> LAN pairing tap (open or confirm visual claim)
         *  3..6 s     -> Enter Setup Mode once (violet, keeps Wi-Fi & Mind)
         *  6..10 s    -> Safe dead-zone (red progress arc; release does nothing)
         *  10..15 s   -> Full wipe (red) */
        if (held_ms < HAVPE_GESTURE_SHOW_PROGRESS_MS) {
          return HAVPE_GESTURE_PAIR_TAP;
        }
        if (held_ms >= HAVPE_GESTURE_SETUP_MODE_MS &&
            held_ms < HAVPE_GESTURE_SETUP_MODE_MAX_MS) {
          return HAVPE_GESTURE_ENTER_SETUP;
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

void havpe_reset_gesture_trigger_unlock_flash(
    struct havpe_reset_gesture *state, uint64_t now_ms) {
  if (state == NULL) return;
  state->unlock_flash_until_ms = now_ms + (uint64_t)HAVPE_GESTURE_UNLOCK_FLASH_MS;
}

bool havpe_reset_gesture_take_cancel_call(struct havpe_reset_gesture *state) {
  if (state == NULL || !state->cancel_call_pending) return false;
  state->cancel_call_pending = false;
  return true;
}

static uint8_t scale_progress_leds(uint64_t elapsed, uint64_t span) {
  if (span == 0U) return ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
  uint8_t lit = (uint8_t)((elapsed * ITERATE_KIT_CONVERSATION_LIGHT_COUNT) / span);
  if (lit == 0U) lit = 1U;
  if (lit > ITERATE_KIT_CONVERSATION_LIGHT_COUNT) {
    lit = ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
  }
  return lit;
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
      if (visual != NULL) *visual = HAVPE_VISUAL_WIFI_RESET_PROGRESS;
      if (progress_leds != NULL) {
        *progress_leds = scale_progress_leds(
            held_ms - HAVPE_GESTURE_SHOW_PROGRESS_MS,
            HAVPE_GESTURE_WIFI_RESET_MS - HAVPE_GESTURE_SHOW_PROGRESS_MS);
      }
      return;
    }

    /* Muted hold timeline: 1.5..3 s violet fill, 3..6 s violet blink (Setup Mode),
     * 6..10 s red fill (dead-zone warning), 10..15 s red blink (Full Wipe). */
    if (held_ms < HAVPE_GESTURE_SETUP_MODE_MS) {
      if (visual != NULL) *visual = HAVPE_VISUAL_SETUP_MODE_PROGRESS;
      if (progress_leds != NULL) {
        *progress_leds = scale_progress_leds(
            held_ms - HAVPE_GESTURE_SHOW_PROGRESS_MS,
            HAVPE_GESTURE_SETUP_MODE_MS - HAVPE_GESTURE_SHOW_PROGRESS_MS);
      }
      return;
    }
    if (held_ms < HAVPE_GESTURE_SETUP_MODE_MAX_MS) {
      if (visual != NULL) *visual = HAVPE_VISUAL_SETUP_MODE_ARMED;
      if (progress_leds != NULL) *progress_leds = ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
      return;
    }
    if (held_ms < HAVPE_GESTURE_FULL_WIPE_MS) {
      if (visual != NULL) *visual = HAVPE_VISUAL_FULL_WIPE_PROGRESS;
      if (progress_leds != NULL) {
        *progress_leds = scale_progress_leds(
            held_ms - HAVPE_GESTURE_SETUP_MODE_MAX_MS,
            HAVPE_GESTURE_FULL_WIPE_MS - HAVPE_GESTURE_SETUP_MODE_MAX_MS);
      }
      return;
    }
    if (visual != NULL) *visual = HAVPE_VISUAL_FULL_WIPE_ARMED;
    if (progress_leds != NULL) *progress_leds = ITERATE_KIT_CONVERSATION_LIGHT_COUNT;
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
    case HAVPE_VISUAL_SETUP_MODE_PROGRESS:
      for (uint8_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) {
        pixels[i] = i < progress_leds
            ? (struct iterate_kit_rgb8){26U, 0U, 38U}
            : (struct iterate_kit_rgb8){3U, 0U, 4U};
      }
      return;
    case HAVPE_VISUAL_SETUP_MODE_ARMED: {
      const bool high = ((now_ms / 180U) & 1U) == 0U;
      const struct iterate_kit_rgb8 c = high
          ? (struct iterate_kit_rgb8){44U, 0U, 56U}
          : (struct iterate_kit_rgb8){8U, 0U, 12U};
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

void havpe_ring_render_claim_open(
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]) {
  if (pixels == NULL) return;
  const uint8_t level = triangle_wave(now_ms, 1200U, 4U, 16U);
  const struct iterate_kit_rgb8 c = {level, level, level};
  for (uint8_t i = 0U; i < ITERATE_KIT_CONVERSATION_LIGHT_COUNT; ++i) {
    pixels[i] = c;
  }
}

void havpe_ring_render_claim_pattern(
    enum iterate_kit_claim_color color,
    enum iterate_kit_claim_position position,
    uint32_t now_ms,
    struct iterate_kit_rgb8 pixels[ITERATE_KIT_CONVERSATION_LIGHT_COUNT]) {
  static const uint8_t quadrants[4][3] = {
    {11U, 0U, 1U},  /* Top (12 o'clock) */
    {2U, 3U, 4U},   /* Right (3 o'clock) */
    {5U, 6U, 7U},   /* Bottom (6 o'clock) */
    {8U, 9U, 10U},  /* Left (9 o'clock) */
  };
  if (pixels == NULL) return;
  memset(pixels, 0, sizeof(*pixels) * ITERATE_KIT_CONVERSATION_LIGHT_COUNT);

  const uint8_t scale = triangle_wave(now_ms, 1000U, 60U, 100U);
  struct iterate_kit_rgb8 base = {0U, 8U, 48U};
  switch (color) {
    case ITERATE_KIT_CLAIM_COLOR_BLUE:
      base = (struct iterate_kit_rgb8){0U, 8U, 48U};
      break;
    case ITERATE_KIT_CLAIM_COLOR_GREEN:
      base = (struct iterate_kit_rgb8){0U, 44U, 6U};
      break;
    case ITERATE_KIT_CLAIM_COLOR_AMBER:
      base = (struct iterate_kit_rgb8){48U, 28U, 0U};
      break;
    case ITERATE_KIT_CLAIM_COLOR_MAGENTA:
      base = (struct iterate_kit_rgb8){44U, 0U, 36U};
      break;
  }
  const struct iterate_kit_rgb8 lit = {
    (uint8_t)(((uint32_t)base.red * scale) / 100U),
    (uint8_t)(((uint32_t)base.green * scale) / 100U),
    (uint8_t)(((uint32_t)base.blue * scale) / 100U),
  };
  const uint8_t q = (uint8_t)position < 4U ? (uint8_t)position : 0U;
  for (uint8_t k = 0U; k < 3U; ++k) {
    pixels[quadrants[q][k]] = lit;
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
