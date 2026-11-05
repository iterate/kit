#ifndef ITERATE_KIT_VOICE_DEVICE_PROFILE_H
#define ITERATE_KIT_VOICE_DEVICE_PROFILE_H

#include <stdint.h>

/* Shared transport budgets for PCM16 mono at 16 kHz (32 bytes/ms).
 * Device DMA, codec timing and AEC calibration belong to the board. */
enum {

  /* Bounded Cap’n Web arenas; exhaustion is a session failure. */
  ITERATE_KIT_VOICE_PENDING_CALL_CAPACITY = 16,
  /* Mount provision plus two overlapping stream subscriptions and one
   * live-state subscription; leave four explicit recovery slots. */
  ITERATE_KIT_VOICE_EXPORT_CAPACITY = 8,
  ITERATE_KIT_VOICE_IMPORT_CAPACITY = 16,
  ITERATE_KIT_VOICE_TOKEN_CAPACITY = 1024,
  ITERATE_KIT_VOICE_OUTPUT_CAPACITY = 128,

  /* Reserve outbox space so microphone traffic cannot starve RPC replies. */
  ITERATE_KIT_VOICE_CONTROL_INBOX_SLOT_CAPACITY = 16384,
  ITERATE_KIT_VOICE_CONTROL_OUTBOX_SLOT_CAPACITY = 8192,
  ITERATE_KIT_VOICE_CONTROL_INBOX_SLOTS = 64,
  ITERATE_KIT_VOICE_CONTROL_OUTBOX_SLOTS = 64,
  ITERATE_KIT_VOICE_MIC_OUTBOX_RESERVE = 40,

  /* Capture grain; GPT-Live itself accepts arbitrary even PCM byte lengths. */
  ITERATE_KIT_VOICE_FRAME_MS = 20,
  ITERATE_KIT_VOICE_FRAME_SAMPLES = 320,
  ITERATE_KIT_VOICE_FRAME_BYTES = 640,
  ITERATE_KIT_VOICE_SAMPLE_RATE_HZ =
      ITERATE_KIT_VOICE_FRAME_SAMPLES * 1000 / ITERATE_KIT_VOICE_FRAME_MS,

  /* 20 s opening deadline plus 500 ms wake history. */
  ITERATE_KIT_VOICE_MIC_QUEUE_DEPTH = 21000 / ITERATE_KIT_VOICE_FRAME_MS,

  /* Eight is the catch-up cap; normal partial batches flush every 50 ms. */
  ITERATE_KIT_VOICE_MIC_FRAMES_PER_APPEND = 8,

  ITERATE_KIT_VOICE_MIC_FLUSH_MS = 50,


  /* Ten seconds of bounded incoming jitter; this is capacity, not a delay. */
  ITERATE_KIT_VOICE_SPEAKER_BUFFER_BYTES = 320000,

  /* Two 100 ms GPT-Live deltas: 110 ms cushion plus 90 ms DMA lead.
   * Seven file-backed turns stayed continuous at 200 ms; 100 ms starved. */
  ITERATE_KIT_VOICE_SPEAKER_PREFILL_BYTES = 110 * 32 + 2880,

  /* Short answers start at the same deadline without waiting for an end marker. */
  ITERATE_KIT_VOICE_SPEAKER_PRIME_WAIT_MS =
      ITERATE_KIT_VOICE_SPEAKER_PREFILL_BYTES /
      (ITERATE_KIT_VOICE_FRAME_BYTES / ITERATE_KIT_VOICE_FRAME_MS),
  ITERATE_KIT_VOICE_SPEAKER_CONCEAL_LIMIT_MS = 400,

  ITERATE_KIT_VOICE_SPEAKER_LAG_CATCHUP_MS = 500,
  ITERATE_KIT_VOICE_SPEAKER_IDLE_POWERDOWN_MS = 1500,

  /* Device presence is separate from the backend’s continuous input clock. */
  ITERATE_KIT_VOICE_CALL_KEEPALIVE_MS = 20000,

  ITERATE_KIT_VOICE_CONNECTION_OPEN_TIMEOUT_MS = 10000,
  ITERATE_KIT_VOICE_CONTROL_POLL_MS = 25,

  ITERATE_KIT_VOICE_UNHEALTHY_RESTART_MS = 120000,

  /* Only count silence while call acceptance or answer audio is owed. */
  ITERATE_KIT_VOICE_DOWNLINK_SILENCE_MS = 10000,


  /*
   * THE SESSION'S HEARTBEAT, AND WHAT DECIDES ITS PERIOD.
   *
   * The OS terminates `/api` in a stateless Worker, and Cloudflare sheds such
   * a request when its JavaScript has handled no incoming message for a while
   * (the runtime's "in use" versus "idle", Kenton Varda, 2022). A PING never
   * counts: the edge answers it and JavaScript never sees it. Measured on a
   * self-host, 2026-11-05: a socket that only PINGed was cut 23 times in
   * 3.75 h; one that sent the session's `whoami()` every 30 s was cut none
   * in 80 min (every 60 s: once). So the mount sends `whoami()` on the
   * SESSION once a period (itx_mount.h): an application message, answered by
   * the Worker without waking the project's Durable Object (asking the project
   * root instead woke it once a minute).
   *
   * THE SAME BEAT IS THE DEAD-SOCKET CHECK. A shed socket often never delivers
   * its close, so a beat unanswered for `_ANSWER_MS` means the session is gone,
   * and the voice loop replaces the socket generation — not the chip.
   */
  ITERATE_KIT_VOICE_SESSION_HEARTBEAT_MS = 30000,
  ITERATE_KIT_VOICE_SESSION_HEARTBEAT_ANSWER_MS = 15000,

  /*
   * THE HOP'S PING, FOR THE WINDOW BEFORE A MOUNT EXISTS.
   *
   * The transport sends a WebSocket PING when the peer has been silent this
   * long (is this TCP hop half-open). On a mounted board the heartbeat's
   * answers are inbound traffic every period, so this never fires there; it
   * covers the window where the transport is ready but no mount exists yet.
   * That is why the liveness watchdog keys on BOTH answers: the heartbeat's
   * while mounted, the PONG before. Keying on the PONG alone made the two race.
   */
  ITERATE_KIT_VOICE_HOP_KEEPALIVE_MS = 60000,

  ITERATE_KIT_VOICE_NO_LIVENESS_RESTART_MS = 420000,

  /* Failed remounts use bounded backoff, reset after a successful mount. */
  ITERATE_KIT_VOICE_REMOUNT_RETRY_MS = 2000,
  ITERATE_KIT_VOICE_REMOUNT_RETRY_MAX_MS = 30000,

};

/* A clock reset cannot manufacture elapsed time. */
static inline uint64_t iterate_kit_voice_elapsed_ms(
    uint64_t now, uint64_t since) {
  return since > now ? 0U : now - since;
}

#endif
