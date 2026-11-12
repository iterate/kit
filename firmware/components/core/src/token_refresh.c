#include "iterate/kit/token_refresh.h"

#include <stdio.h>
#include <string.h>

/*
 * The refresh exchange of token_refresh.h. Everything here is bounded by the
 * caller's storage, and every failure clears what it may have copied: a token
 * is a secret, and a half-parsed one is never a credential.
 */

enum { DEFAULT_EXPIRES_IN_S = 3600 };

static bool unreserved(unsigned char character) {
  return (character >= 'A' && character <= 'Z') ||
      (character >= 'a' && character <= 'z') ||
      (character >= '0' && character <= '9') ||
      character == '-' || character == '.' || character == '_' ||
      character == '~';
}

/* application/x-www-form-urlencoded: everything but RFC 3986's unreserved. */
static bool append_encoded(
    char *destination, size_t capacity, size_t *size, const char *value) {
  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0';
       ++cursor) {
    if (unreserved(*cursor)) {
      if (*size + 1U >= capacity) return false;
      destination[(*size)++] = (char)*cursor;
    } else {
      if (*size + 3U >= capacity) return false;
      destination[(*size)++] = '%';
      destination[(*size)++] = hex[*cursor >> 4U];
      destination[(*size)++] = hex[*cursor & 0x0fU];
    }
  }
  destination[*size] = '\0';
  return true;
}

static bool append_text(
    char *destination, size_t capacity, size_t *size, const char *value) {
  const size_t length = strlen(value);
  if (*size + length >= capacity) return false;
  memcpy(destination + *size, value, length + 1U);
  *size += length;
  return true;
}

/* The encoded size of a form value, or SIZE_MAX past `limit`. */
static size_t encoded_size(const char *value, size_t limit) {
  size_t size = 0U;
  for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
    size += unreserved(*cursor) ? 1U : 3U;
    if (size > limit) return SIZE_MAX;
  }
  return size;
}

size_t iterate_kit_token_refresh_build_request(
    char *destination,
    size_t capacity,
    const char *host,
    uint16_t port,
    bool secure,
    const char *client_id,
    const char *refresh_token,
    const char *resource) {
  static const char grant[] = "grant_type=refresh_token&refresh_token=";
  static const char client[] = "&client_id=";
  static const char audience[] = "&resource=";
  size_t form_size;
  size_t size;
  size_t part;
  int written;
  if (destination == NULL || capacity == 0U) return 0U;
  destination[0] = '\0';
  if (host == NULL || host[0] == '\0' || client_id == NULL || client_id[0] == '\0' ||
      refresh_token == NULL || refresh_token[0] == '\0' || resource == NULL ||
      resource[0] == '\0') {
    return 0U;
  }
  form_size = sizeof(grant) - 1U + sizeof(client) - 1U + sizeof(audience) - 1U;
  part = encoded_size(refresh_token, capacity);
  if (part == SIZE_MAX) return 0U;
  form_size += part;
  part = encoded_size(client_id, capacity);
  if (part == SIZE_MAX) return 0U;
  form_size += part;
  part = encoded_size(resource, capacity);
  if (part == SIZE_MAX) return 0U;
  form_size += part;
  if ((secure && port == 443U) || (!secure && port == 80U)) {
    written = snprintf(
        destination, capacity,
        "POST /oauth2/token HTTP/1.1\r\nHost: %s\r\n", host);
  } else {
    written = snprintf(
        destination, capacity,
        "POST /oauth2/token HTTP/1.1\r\nHost: %s:%u\r\n", host, (unsigned)port);
  }
  if (written < 0 || (size_t)written >= capacity) goto overflow;
  size = (size_t)written;
  written = snprintf(
      destination + size,
      capacity - size,
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Accept: application/json\r\n"
      "Connection: close\r\n"
      "Content-Length: %u\r\n"
      "\r\n",
      (unsigned)form_size);
  if (written < 0 || (size_t)written >= capacity - size) goto overflow;
  size += (size_t)written;
  if (!append_text(destination, capacity, &size, grant) ||
      !append_encoded(destination, capacity, &size, refresh_token) ||
      !append_text(destination, capacity, &size, client) ||
      !append_encoded(destination, capacity, &size, client_id) ||
      !append_text(destination, capacity, &size, audience) ||
      !append_encoded(destination, capacity, &size, resource)) {
    goto overflow;
  }
  return size;
overflow:
  memset(destination, 0, capacity);
  return 0U;
}

static bool lower_equal(const char *a, const char *b, size_t length) {
  for (size_t index = 0U; index < length; ++index) {
    char x = a[index];
    char y = b[index];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

static const char *find_bytes(
    const char *haystack, size_t haystack_size, const char *needle) {
  const size_t needle_size = strlen(needle);
  if (needle_size == 0U || haystack_size < needle_size) return NULL;
  for (size_t index = 0U; index + needle_size <= haystack_size; ++index) {
    if (memcmp(haystack + index, needle, needle_size) == 0) return haystack + index;
  }
  return NULL;
}

/* One header's value in [headers, headers + size), or NULL. */
static const char *header_value(
    const char *headers, size_t size, const char *name, size_t *value_size) {
  const size_t name_size = strlen(name);
  const char *line = headers;
  const char *end = headers + size;
  while (line < end) {
    const char *line_end = find_bytes(line, (size_t)(end - line), "\r\n");
    if (line_end == NULL) line_end = end;
    if ((size_t)(line_end - line) > name_size && line[name_size] == ':' &&
        lower_equal(line, name, name_size)) {
      const char *value = line + name_size + 1;
      while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
      *value_size = (size_t)(line_end - value);
      return value;
    }
    line = line_end + 2;
  }
  return NULL;
}

static bool parse_decimal(const char *text, size_t size, size_t *value) {
  size_t result = 0U;
  if (size == 0U) return false;
  for (size_t index = 0U; index < size; ++index) {
    if (text[index] < '0' || text[index] > '9') return false;
    if (result > (SIZE_MAX - 9U) / 10U) return false;
    result = result * 10U + (size_t)(text[index] - '0');
  }
  *value = result;
  return true;
}

static int hex_digit(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

/*
 * A chunked body, decoded in place once it has all arrived: the decoded bytes
 * only ever move toward the front. Returns 1 with `body_size` set when the
 * last chunk arrived, 0 when more bytes are needed (nothing moved), -1 when it
 * is malformed.
 */
static int dechunk(char *chunked, size_t size, size_t *body_size) {
  for (int pass = 0; pass < 2; ++pass) {
    size_t offset = 0U;
    size_t out = 0U;
    for (;;) {
      size_t chunk = 0U;
      size_t digits = 0U;
      const char *line_end = find_bytes(chunked + offset, size - offset, "\r\n");
      if (line_end == NULL) return 0;
      while (chunked + offset + digits < line_end) {
        const int digit = hex_digit(chunked[offset + digits]);
        if (digit < 0) break;
        if (chunk > (SIZE_MAX >> 4U)) return -1;
        chunk = (chunk << 4U) | (size_t)digit;
        ++digits;
      }
      if (digits == 0U) return -1;
      offset = (size_t)(line_end - chunked) + 2U;
      if (chunk == 0U) {
        if (pass == 1) *body_size = out;
        break;
      }
      if (size - offset < chunk + 2U) return 0;
      if (chunked[offset + chunk] != '\r' || chunked[offset + chunk + 1U] != '\n') return -1;
      if (pass == 1) memmove(chunked + out, chunked + offset, chunk);
      out += chunk;
      offset += chunk + 2U;
    }
  }
  return 1;
}

static const char *skip_space(const char *cursor, const char *end) {
  while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n'))
    ++cursor;
  return cursor;
}

/*
 * A string member of a flat JSON object, unescaped into `destination`. Only
 * the escapes a token endpoint can send are taken (\" \\ \/); \u is refused,
 * since no token holds one. Nested values are skipped only as far as a flat
 * object needs: a token answer has none.
 */
static bool json_string_member(
    const char *body, size_t size, const char *name, char *destination, size_t capacity) {
  char needle[40];
  const char *end = body + size;
  const char *at;
  const int written = snprintf(needle, sizeof(needle), "\"%s\"", name);
  if (written < 0 || (size_t)written >= sizeof(needle)) return false;
  at = find_bytes(body, size, needle);
  if (at == NULL) return false;
  at = skip_space(at + written, end);
  if (at >= end || *at != ':') return false;
  at = skip_space(at + 1, end);
  if (at >= end || *at != '"') return false;
  ++at;
  size_t length = 0U;
  while (at < end && *at != '"') {
    char character = *at++;
    if (character == '\\') {
      if (at >= end) return false;
      character = *at++;
      if (character != '"' && character != '\\' && character != '/') return false;
    } else if ((unsigned char)character < 0x20U) {
      return false;
    }
    if (length + 1U >= capacity) return false;
    destination[length++] = character;
  }
  if (at >= end || length == 0U) return false;
  destination[length] = '\0';
  return true;
}

static bool json_number_member(const char *body, size_t size, const char *name, uint32_t *value) {
  char needle[40];
  const char *end = body + size;
  const char *at;
  uint32_t result = 0U;
  size_t digits = 0U;
  const int written = snprintf(needle, sizeof(needle), "\"%s\"", name);
  if (written < 0 || (size_t)written >= sizeof(needle)) return false;
  at = find_bytes(body, size, needle);
  if (at == NULL) return false;
  at = skip_space(at + written, end);
  if (at >= end || *at != ':') return false;
  at = skip_space(at + 1, end);
  while (at < end && *at >= '0' && *at <= '9') {
    if (result > (UINT32_MAX - 9U) / 10U) return false;
    result = result * 10U + (uint32_t)(*at - '0');
    ++at;
    ++digits;
  }
  if (digits == 0U) return false;
  *value = result;
  return true;
}

enum iterate_kit_token_refresh_result iterate_kit_token_refresh_parse_response(
    uint8_t *bytes,
    size_t byte_count,
    bool complete,
    struct iterate_kit_tokens *tokens,
    int32_t *http_status) {
  char *text = (char *)bytes;
  char *body;
  size_t body_size = 0U;
  const char *headers_end;
  const char *status_end;
  const char *value;
  size_t value_size;
  size_t status = 0U;
  char *payload;
  size_t payload_size;
  if (tokens != NULL) memset(tokens, 0, sizeof(*tokens));
  if (http_status != NULL) *http_status = 0;
  if (bytes == NULL || tokens == NULL) return ITERATE_KIT_TOKEN_REFRESH_FAILED;

  headers_end = find_bytes(text, byte_count, "\r\n\r\n");
  if (headers_end == NULL)
    return complete ? ITERATE_KIT_TOKEN_REFRESH_FAILED : ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK;
  status_end = find_bytes(text, (size_t)(headers_end - text) + 2U, "\r\n");
  if (status_end == NULL || status_end - text < 12 || memcmp(text, "HTTP/1.", 7) != 0 ||
      text[8] != ' ' || !parse_decimal(text + 9, 3U, &status) ||
      (status_end - text > 12 && text[12] != ' ')) {
    if (http_status != NULL) *http_status = -1;
    return ITERATE_KIT_TOKEN_REFRESH_FAILED;
  }
  if (http_status != NULL) *http_status = (int32_t)status;

  payload = text + (headers_end - text) + 4;
  payload_size = byte_count - (size_t)(payload - text);
  value = header_value(
      status_end + 2, (size_t)(headers_end - status_end), "transfer-encoding", &value_size);
  if (value != NULL && value_size >= 7U && lower_equal(value, "chunked", 7U)) {
    const int done = dechunk(payload, payload_size, &body_size);
    if (done < 0) return ITERATE_KIT_TOKEN_REFRESH_FAILED;
    if (done == 0)
      return complete ? ITERATE_KIT_TOKEN_REFRESH_FAILED : ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK;
  } else {
    size_t length = 0U;
    value = header_value(
        status_end + 2, (size_t)(headers_end - status_end), "content-length", &value_size);
    if (value != NULL) {
      if (!parse_decimal(value, value_size, &length)) return ITERATE_KIT_TOKEN_REFRESH_FAILED;
      if (payload_size < length)
        return complete ? ITERATE_KIT_TOKEN_REFRESH_FAILED
                        : ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK;
    } else {
      if (!complete) return ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK;
      length = payload_size;
    }
    body_size = length;
  }
  body = payload;

  if (status == 400U || status == 401U) return ITERATE_KIT_TOKEN_REFRESH_REFUSED;
  if (status != 200U ||
      !json_string_member(
          body, body_size, "access_token", tokens->access_token, sizeof(tokens->access_token)) ||
      !json_string_member(
          body, body_size, "refresh_token", tokens->refresh_token,
          sizeof(tokens->refresh_token))) {
    memset(tokens, 0, sizeof(*tokens));
    return ITERATE_KIT_TOKEN_REFRESH_FAILED;
  }
  if (!json_number_member(body, body_size, "expires_in", &tokens->expires_in_s) ||
      tokens->expires_in_s == 0U) {
    tokens->expires_in_s = DEFAULT_EXPIRES_IN_S;
  }
  return ITERATE_KIT_TOKEN_REFRESH_DONE;
}

enum iterate_kit_token_refresh_result iterate_kit_token_refresh_begin(
    struct iterate_kit_token_refresh *refresh,
    const struct iterate_kit_byte_stream_ops *stream,
    void *stream_context,
    const struct iterate_kit_websocket_endpoint *endpoint,
    const char *client_id,
    const char *refresh_token,
    const char *resource,
    uint8_t *response,
    size_t response_capacity) {
  if (refresh == NULL) return ITERATE_KIT_TOKEN_REFRESH_FAILED;
  memset(refresh, 0, sizeof(*refresh));
  if (stream == NULL || endpoint == NULL || response == NULL || response_capacity < 2U)
    return ITERATE_KIT_TOKEN_REFRESH_FAILED;
  refresh->request_size = iterate_kit_token_refresh_build_request(
      (char *)refresh->request,
      sizeof(refresh->request),
      endpoint->host,
      endpoint->port,
      endpoint->secure,
      client_id,
      refresh_token,
      resource);
  if (refresh->request_size == 0U) return ITERATE_KIT_TOKEN_REFRESH_FAILED;
  refresh->stream = stream;
  refresh->stream_context = stream_context;
  refresh->response = response;
  refresh->response_capacity = response_capacity;
  refresh->active = true;
  return ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK;
}

enum iterate_kit_token_refresh_result iterate_kit_token_refresh_step(
    struct iterate_kit_token_refresh *refresh,
    struct iterate_kit_tokens *tokens) {
  enum iterate_kit_byte_stream_result moved;
  if (refresh == NULL || !refresh->active || tokens == NULL)
    return ITERATE_KIT_TOKEN_REFRESH_FAILED;
  if (!refresh->connected) {
    moved = refresh->stream->connect(refresh->stream_context);
    if (moved == ITERATE_KIT_BYTE_STREAM_WOULD_BLOCK) return ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK;
    if (moved != ITERATE_KIT_BYTE_STREAM_PROGRESS) return ITERATE_KIT_TOKEN_REFRESH_FAILED;
    refresh->connected = true;
  }
  while (refresh->request_offset < refresh->request_size) {
    size_t written = 0U;
    moved = refresh->stream->write(
        refresh->stream_context,
        refresh->request + refresh->request_offset,
        refresh->request_size - refresh->request_offset,
        &written);
    if (moved == ITERATE_KIT_BYTE_STREAM_WOULD_BLOCK) return ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK;
    if (moved != ITERATE_KIT_BYTE_STREAM_PROGRESS || written == 0U)
      return ITERATE_KIT_TOKEN_REFRESH_FAILED;
    refresh->request_offset += written;
  }
  for (;;) {
    size_t read = 0U;
    bool closed = false;
    enum iterate_kit_token_refresh_result parsed;
    if (refresh->response_size + 1U >= refresh->response_capacity)
      return ITERATE_KIT_TOKEN_REFRESH_FAILED;
    moved = refresh->stream->read(
        refresh->stream_context,
        refresh->response + refresh->response_size,
        refresh->response_capacity - refresh->response_size - 1U,
        &read);
    if (moved == ITERATE_KIT_BYTE_STREAM_WOULD_BLOCK) return ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK;
    if (moved == ITERATE_KIT_BYTE_STREAM_FAILED || read == 0U) {
      /* Connection: close — the peer's close may be the body's end. */
      closed = true;
    } else {
      refresh->response_size += read;
    }
    parsed = iterate_kit_token_refresh_parse_response(
        refresh->response, refresh->response_size, closed, tokens, &refresh->last_http_status);
    if (parsed != ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK || closed) {
      return parsed == ITERATE_KIT_TOKEN_REFRESH_WOULD_BLOCK ? ITERATE_KIT_TOKEN_REFRESH_FAILED
                                                             : parsed;
    }
  }
}

void iterate_kit_token_refresh_close(struct iterate_kit_token_refresh *refresh) {
  if (refresh == NULL) return;
  /*
   * Whatever state the connect reached: a deadline can abort it mid-handshake
   * (the Mac's DNS, TCP and TLS each step), and a stream left open there would
   * carry the old handshake into the next exchange. The byte stream takes a
   * repeated close harmlessly (websocket_client.h).
   */
  if (refresh->active && refresh->stream != NULL)
    refresh->stream->close(refresh->stream_context);
  if (refresh->response != NULL) memset(refresh->response, 0, refresh->response_capacity);
  memset(refresh->request, 0, sizeof(refresh->request));
  refresh->connected = false;
  refresh->active = false;
  refresh->request_size = 0U;
  refresh->request_offset = 0U;
  refresh->response_size = 0U;
}

enum iterate_kit_configuration_error iterate_kit_token_refresh_resource(
    const char *base_url, char *destination, size_t capacity) {
  const size_t length = base_url == NULL ? 0U : strlen(base_url);
  if (destination != NULL && capacity > 0U) destination[0] = '\0';
  if (base_url == NULL || destination == NULL || capacity == 0U || length == 0U)
    return ITERATE_KIT_CONFIGURATION_INVALID_ARGUMENT;
  if (strncmp(base_url, "https://", 8U) != 0 && strncmp(base_url, "http://", 7U) != 0)
    return ITERATE_KIT_CONFIGURATION_INVALID_VALUE;
  if (length + sizeof("/api") > capacity) return ITERATE_KIT_CONFIGURATION_FIELD_TOO_LONG;
  memcpy(destination, base_url, length);
  memcpy(destination + length, "/api", sizeof("/api"));
  return ITERATE_KIT_CONFIGURATION_OK;
}
