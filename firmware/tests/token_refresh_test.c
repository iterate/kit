#include "iterate/kit/token_refresh.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * A mind board's refresh exchange (token_refresh.h): the request it writes,
 * every answer shape a token endpoint behind Cloudflare sends, and the step
 * machine over a scripted byte stream that answers in small pieces.
 */

static const char client_id[] =
    "https://k.iterate.com/devices/mac/clients/00000000-0000-4000-8000-000000000000.json";

static void builds_the_form_post(void) {
  char request[ITERATE_KIT_TOKEN_REFRESH_REQUEST_CAPACITY];
  const size_t size = iterate_kit_token_refresh_build_request(
      request, sizeof(request), "iterate.example.com", 443U, true, client_id,
      "user_1:grant_1:s/cret+", "https://iterate.example.com/api");
  const char *body;
  char length[32];
  assert(size == strlen(request));
  assert(strncmp(request, "POST /oauth2/token HTTP/1.1\r\nHost: iterate.example.com\r\n", 56) == 0);
  body = strstr(request, "\r\n\r\n");
  assert(body != NULL);
  body += 4;
  assert(strcmp(body,
                "grant_type=refresh_token&refresh_token=user_1%3Agrant_1%3As%2Fcret%2B"
                "&client_id=https%3A%2F%2Fk.iterate.com%2Fdevices%2Fmac%2Fclients%2F"
                "00000000-0000-4000-8000-000000000000.json"
                "&resource=https%3A%2F%2Fiterate.example.com%2Fapi") == 0);
  (void)snprintf(length, sizeof(length), "Content-Length: %u\r\n", (unsigned)strlen(body));
  assert(strstr(request, length) != NULL);
  assert(strstr(request, "Connection: close\r\n") != NULL);

  /* a port that is not the scheme's own goes in the Host header */
  assert(iterate_kit_token_refresh_build_request(
             request, sizeof(request), "localhost", 8788U, false, client_id, "t",
             "http://localhost:8788/api") > 0U);
  assert(strstr(request, "Host: localhost:8788\r\n") != NULL);

  /* too small, or a missing value, builds nothing */
  assert(iterate_kit_token_refresh_build_request(
             request, 64U, "iterate.example.com", 443U, true, client_id, "t", "r") == 0U);
  assert(request[0] == '\0');
  assert(iterate_kit_token_refresh_build_request(
             request, sizeof(request), "iterate.example.com", 443U, true, client_id, "", "r") ==
         0U);
}

static enum iterate_kit_token_refresh_result parse(
    const char *answer, bool complete, struct iterate_kit_tokens *tokens, int32_t *status) {
  static uint8_t bytes[2048];
  const size_t size = strlen(answer);
  memcpy(bytes, answer, size);
  return iterate_kit_token_refresh_parse_response(bytes, size, complete, tokens, status);
}

static const char ok_body[] =
    "{\"access_token\":\"user_1:grant_1:access\",\"token_type\":\"bearer\","
    "\"expires_in\":3599,\"refresh_token\":\"user_1:grant_1:next\",\"scope\":\"iterate\"}";

static void parses_a_content_length_answer(void) {
  char answer[1024];
  struct iterate_kit_tokens tokens;
  int32_t status = 0;
  (void)snprintf(answer, sizeof(answer),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\ncontent-length: %u\r\n"
                 "Cache-Control: no-store\r\n\r\n%s",
                 (unsigned)strlen(ok_body), ok_body);
  assert(parse(answer, false, &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_DONE);
  assert(status == 200);
  assert(strcmp(tokens.access_token, "user_1:grant_1:access") == 0);
  assert(strcmp(tokens.refresh_token, "user_1:grant_1:next") == 0);
  assert(tokens.expires_in_s == 3599U);

  /* half a body waits for the rest */
  answer[strlen(answer) - 10U] = '\0';
  assert(parse(answer, false, &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK);
  assert(tokens.access_token[0] == '\0');
  /* and the peer's close then is a failure, not a token */
  assert(parse(answer, true, &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_FAILED);
}

static void parses_a_chunked_answer(void) {
  char answer[1024];
  struct iterate_kit_tokens tokens;
  int32_t status = 0;
  const size_t half = strlen(ok_body) / 2U;
  (void)snprintf(answer, sizeof(answer),
                 "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n%x\r\n%.*s\r\n%x\r\n%s\r\n0\r\n\r\n",
                 (unsigned)half, (int)half, ok_body, (unsigned)(strlen(ok_body) - half),
                 ok_body + half);
  assert(parse(answer, false, &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_DONE);
  assert(strcmp(tokens.refresh_token, "user_1:grant_1:next") == 0);
  /* no last chunk yet: wait */
  answer[strlen(answer) - 5U] = '\0';
  assert(parse(answer, false, &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK);
}

static void classifies_refusals_and_failures(void) {
  struct iterate_kit_tokens tokens;
  int32_t status = 0;
  assert(parse("HTTP/1.1 400 Bad Request\r\nContent-Length: 27\r\n\r\n{\"error\":\"invalid_grant\"}  ",
               false, &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_REFUSED);
  assert(status == 400);
  assert(parse("HTTP/1.1 401 Unauthorized\r\nContent-Length: 2\r\n\r\n{}", false, &tokens,
               &status) == ITERATE_KIT_TOKEN_REFRESH_REFUSED);
  assert(parse("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 2\r\n\r\n{}", false, &tokens,
               &status) == ITERATE_KIT_TOKEN_REFRESH_FAILED);
  assert(parse("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n", false, &tokens,
               &status) == ITERATE_KIT_TOKEN_REFRESH_FAILED);
  /* a 200 with no refresh token is no credential */
  assert(parse("HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n{\"access_token\":\"a\"}", false,
               &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_FAILED);
  assert(tokens.access_token[0] == '\0');
  assert(parse("garbage\r\n\r\n", false, &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_FAILED);
  assert(status == -1);
  /* no length and no close yet: wait; then the close ends the body */
  assert(parse("HTTP/1.1 200 OK\r\n\r\n{\"access_token\":\"a\",\"refresh_token\":\"b\"}", false,
               &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK);
  assert(parse("HTTP/1.1 200 OK\r\n\r\n{\"access_token\":\"a\",\"refresh_token\":\"b\"}", true,
               &tokens, &status) == ITERATE_KIT_TOKEN_REFRESH_DONE);
  assert(tokens.expires_in_s == 3600U);
}

/* A stream that blocks once per step and answers seven bytes at a time. */
struct scripted_stream {
  const char *answer;
  size_t answer_offset;
  char written[ITERATE_KIT_TOKEN_REFRESH_REQUEST_CAPACITY];
  size_t written_size;
  bool block_next;
  int connects;
  int closes;
};

static enum iterate_kit_byte_stream_result scripted_connect(void *context) {
  struct scripted_stream *stream = context;
  ++stream->connects;
  return ITERATE_KIT_BYTE_STREAM_PROGRESS;
}

static enum iterate_kit_byte_stream_result scripted_read(
    void *context, uint8_t *bytes, size_t capacity, size_t *bytes_read) {
  struct scripted_stream *stream = context;
  size_t left = strlen(stream->answer) - stream->answer_offset;
  stream->block_next = !stream->block_next;
  if (stream->block_next) return ITERATE_KIT_BYTE_STREAM_WOULD_BLOCK;
  if (left == 0U) return ITERATE_KIT_BYTE_STREAM_FAILED;
  if (left > 7U) left = 7U;
  if (left > capacity) left = capacity;
  memcpy(bytes, stream->answer + stream->answer_offset, left);
  stream->answer_offset += left;
  *bytes_read = left;
  return ITERATE_KIT_BYTE_STREAM_PROGRESS;
}

static enum iterate_kit_byte_stream_result scripted_write(
    void *context, const uint8_t *bytes, size_t byte_count, size_t *bytes_written) {
  struct scripted_stream *stream = context;
  if (byte_count > 100U) byte_count = 100U;
  memcpy(stream->written + stream->written_size, bytes, byte_count);
  stream->written_size += byte_count;
  *bytes_written = byte_count;
  return ITERATE_KIT_BYTE_STREAM_PROGRESS;
}

static void scripted_close(void *context) {
  struct scripted_stream *stream = context;
  ++stream->closes;
}

static const struct iterate_kit_byte_stream_ops scripted_ops = {
  scripted_connect, scripted_read, scripted_write, scripted_close, NULL, NULL,
};

static void steps_an_exchange_over_a_stream(void) {
  static struct iterate_kit_token_refresh refresh;
  static uint8_t response[2048];
  char answer[1024];
  struct scripted_stream stream;
  struct iterate_kit_websocket_endpoint endpoint;
  struct iterate_kit_tokens tokens;
  enum iterate_kit_token_refresh_result result;
  int steps = 0;
  memset(&stream, 0, sizeof(stream));
  (void)snprintf(answer, sizeof(answer), "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n%s",
                 (unsigned)strlen(ok_body), ok_body);
  stream.answer = answer;
  assert(iterate_kit_websocket_endpoint_parse("wss://iterate.example.com/api", &endpoint));
  assert(iterate_kit_token_refresh_begin(&refresh, &scripted_ops, &stream, &endpoint, client_id,
                                         "user_1:grant_1:first", "https://iterate.example.com/api",
                                         response, sizeof(response)) ==
         ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK);
  do {
    result = iterate_kit_token_refresh_step(&refresh, &tokens);
    ++steps;
  } while (result == ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK && steps < 1000);
  assert(result == ITERATE_KIT_TOKEN_REFRESH_DONE);
  assert(stream.connects == 1);
  assert(stream.written_size == refresh.request_size);
  assert(strstr(stream.written, "refresh_token=user_1%3Agrant_1%3Afirst&") != NULL);
  assert(strcmp(tokens.refresh_token, "user_1:grant_1:next") == 0);
  assert(refresh.last_http_status == 200);
  iterate_kit_token_refresh_close(&refresh);
  assert(stream.closes == 1);
  /* the request held the old token: close forgets it */
  for (size_t index = 0U; index < sizeof(refresh.request); ++index) assert(refresh.request[index] == 0U);
}

static enum iterate_kit_byte_stream_result blocked_connect(void *context) {
  struct scripted_stream *stream = context;
  ++stream->connects;
  return ITERATE_KIT_BYTE_STREAM_WOULD_BLOCK;
}

/* A deadline that aborts a connect still in its handshake closes the stream. */
static void closes_a_stream_still_connecting(void) {
  static struct iterate_kit_token_refresh refresh;
  static uint8_t response[256];
  const struct iterate_kit_byte_stream_ops ops = {
    blocked_connect, scripted_read, scripted_write, scripted_close, NULL, NULL,
  };
  struct scripted_stream stream;
  struct iterate_kit_websocket_endpoint endpoint;
  struct iterate_kit_tokens tokens;
  memset(&stream, 0, sizeof(stream));
  assert(iterate_kit_websocket_endpoint_parse("wss://iterate.example.com/api", &endpoint));
  assert(iterate_kit_token_refresh_begin(&refresh, &ops, &stream, &endpoint, client_id, "t",
                                         "https://iterate.example.com/api", response,
                                         sizeof(response)) == ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK);
  assert(iterate_kit_token_refresh_step(&refresh, &tokens) == ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK);
  assert(stream.connects == 1);
  iterate_kit_token_refresh_close(&refresh);
  assert(stream.closes == 1);
  iterate_kit_token_refresh_close(&refresh);
  assert(stream.closes == 1);
}

static void builds_the_resource(void) {
  char resource[64];
  assert(iterate_kit_token_refresh_resource("https://iterate.example.com", resource,
                                            sizeof(resource)) == ITERATE_KIT_CONFIGURATION_OK);
  assert(strcmp(resource, "https://iterate.example.com/api") == 0);
  assert(iterate_kit_token_refresh_resource("https://iterate.example.com", resource, 10U) ==
         ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG);
  assert(iterate_kit_token_refresh_resource("ftp://x", resource, sizeof(resource)) ==
         ITERATE_KIT_CONFIGURATION_INVALID_VALUE);
}

int main(void) {
  builds_the_form_post();
  parses_a_content_length_answer();
  parses_a_chunked_answer();
  classifies_refusals_and_failures();
  steps_an_exchange_over_a_stream();
  closes_a_stream_still_connecting();
  builds_the_resource();
  return 0;
}
