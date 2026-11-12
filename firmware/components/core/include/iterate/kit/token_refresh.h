#ifndef ITERATE_KIT_TOKEN_REFRESH_H
#define ITERATE_KIT_TOKEN_REFRESH_H

#include "iterate/kit/configuration.h"
#include "iterate/kit/websocket_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A MIND BOARD'S ACCESS TOKEN. A mind (iterate/core) issues no token without
 * an expiry: its issuer gives an access token for an hour and a refresh token
 * that lives while it is used, and ends after 7 days with no use. Kit writes
 * the refresh token into the board's image (configuration.h, PLATFORM_MIND);
 * before each upgrade the board trades it at the issuer's token endpoint,
 *
 *   POST /oauth2/token   grant_type=refresh_token&refresh_token=…
 *                        &client_id=…&resource=https://iterate.<domain>/api
 *
 * on the same host as the upgrade, over the platform's own byte stream, and
 * keeps the new refresh token the answer carries (each refresh rotates it; the
 * issuer still takes the previous one once, so a board that lost the newer one
 * to a reset before storing it is not locked out).
 *
 * One HTTP/1.1 request per connection (`Connection: close`). The answer is
 * read whole into caller storage, with Content-Length, chunked encoding or the
 * peer's close as its end. No allocation, no blocking beyond the stream's own
 * connect bound.
 */

enum {
  /** Room for the request line, the headers and the form. */
  ITERATE_KIT_TOKEN_REFRESH_REQUEST_CAPACITY = 1536,
};

struct iterate_kit_tokens {
  char access_token[ITERATE_KIT_OAUTH_TOKEN_CAPACITY];
  char refresh_token[ITERATE_KIT_OAUTH_TOKEN_CAPACITY];
  /** The answer's expires_in; 3600 when it named none. */
  uint32_t expires_in_s;
};

enum iterate_kit_token_refresh_result {
  ITERATE_KIT_TOKEN_REFRESH_DONE = 0,
  ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK,
  /**
   * The issuer refused the grant (400 or 401: invalid_grant, invalid_client):
   * revoked, its person no longer a member, or unused for 7 days. The same
   * token gets the same answer, so the caller backs off; setting the board up
   * again is what mends it.
   */
  ITERATE_KIT_TOKEN_REFRESH_REFUSED,
  /** The network, the issuer (5xx, 429) or the answer failed: try again. */
  ITERATE_KIT_TOKEN_REFRESH_FAILED,
};

/**
 * Builds the whole request into `destination`. `host` is the Host header (the
 * port is added when it is not the scheme's default). Returns its size, or 0
 * when it does not fit or an argument is missing.
 */
size_t iterate_kit_token_refresh_build_request(
    char *destination,
    size_t capacity,
    const char *host,
    uint16_t port,
    bool secure,
    const char *client_id,
    const char *refresh_token,
    const char *resource);

/**
 * Reads one HTTP answer, decoding a chunked body in place once it is whole
 * (so `bytes` is the caller's and changes). `complete` says the peer closed, so a body with no
 * declared length ends here. WOULD_BLOCK: more bytes are needed. On DONE both
 * tokens are copied into `tokens`; on anything else `tokens` is cleared.
 * `http_status` gets the status (0 before a status line, -1 when it did not
 * parse).
 */
enum iterate_kit_token_refresh_result iterate_kit_token_refresh_parse_response(
    uint8_t *bytes,
    size_t byte_count,
    bool complete,
    struct iterate_kit_tokens *tokens,
    int32_t *http_status);

/** One exchange, stepped by its owner like the WebSocket client's open. */
struct iterate_kit_token_refresh {
  const struct iterate_kit_byte_stream_ops *stream;
  void *stream_context;
  uint8_t request[ITERATE_KIT_TOKEN_REFRESH_REQUEST_CAPACITY];
  size_t request_size;
  size_t request_offset;
  uint8_t *response;
  size_t response_capacity;
  size_t response_size;
  bool connected;
  bool active;
  /** The latest answer's HTTP status, for health (0: none yet). */
  int32_t last_http_status;
};

/**
 * Starts an exchange: builds the request against `endpoint` (the upgrade's
 * own, which the stream dials) and keeps `response` storage borrowed until
 * close. FAILED when the request cannot be built.
 */
enum iterate_kit_token_refresh_result iterate_kit_token_refresh_begin(
    struct iterate_kit_token_refresh *refresh,
    const struct iterate_kit_byte_stream_ops *stream,
    void *stream_context,
    const struct iterate_kit_websocket_endpoint *endpoint,
    const char *client_id,
    const char *refresh_token,
    const char *resource,
    uint8_t *response,
    size_t response_capacity);

/** One step: connect, write, read, parse. Anything but WOULD_BLOCK ends it. */
enum iterate_kit_token_refresh_result iterate_kit_token_refresh_step(
    struct iterate_kit_token_refresh *refresh,
    struct iterate_kit_tokens *tokens);

/** Closes the stream and forgets the request (it holds the token). */
void iterate_kit_token_refresh_close(struct iterate_kit_token_refresh *refresh);

/**
 * Builds `https://<host>/api` from the image's base URL: the resource a
 * mind's API token names.
 */
enum iterate_kit_configuration_error iterate_kit_token_refresh_resource(
    const char *base_url, char *destination, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
