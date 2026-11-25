#include "iterate/kit/captive_dns.h"

#include <string.h>

enum {
  DNS_HEADER_SIZE = 12,
  DNS_ANSWER_SIZE = 16,
  DNS_TYPE_A = 1,
  DNS_TYPE_ANY = 255,
  DNS_CLASS_IN = 1,
  DNS_TTL_SECONDS = 30,
};

static uint16_t read_u16_be(const uint8_t *bytes) {
  return (uint16_t)(((uint16_t)bytes[0] << 8U) | (uint16_t)bytes[1]);
}

static void write_u16_be(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value >> 8U);
  bytes[1] = (uint8_t)(value & 0xffU);
}

static void write_u32_be(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)(value >> 24U);
  bytes[1] = (uint8_t)((value >> 16U) & 0xffU);
  bytes[2] = (uint8_t)((value >> 8U) & 0xffU);
  bytes[3] = (uint8_t)(value & 0xffU);
}

size_t iterate_kit_captive_dns_respond(
    const uint8_t *query,
    size_t query_size,
    const uint8_t ip4[4],
    uint8_t *response,
    size_t response_capacity) {
  size_t cursor = DNS_HEADER_SIZE;
  size_t labels = 0U;
  uint16_t flags;
  uint16_t qdcount;
  uint16_t qtype;
  uint16_t qclass;

  if (query == NULL || ip4 == NULL || response == NULL ||
      query_size < DNS_HEADER_SIZE) {
    return 0U;
  }

  flags = read_u16_be(query + 2U);
  qdcount = read_u16_be(query + 4U);
  /* Reject responses (QR=1), non-standard opcodes, and empty questions. */
  if ((flags & 0xf800U) != 0U || qdcount == 0U) {
    return 0U;
  }

  /* Walk the first QNAME strictly: no compression pointers in a query QNAME. */
  while (cursor < query_size) {
    const uint8_t label_len = query[cursor];
    if (label_len == 0U) {
      ++cursor;
      break;
    }
    if ((label_len & 0xc0U) != 0U || label_len > 63U || labels >= 128U) {
      return 0U;
    }
    if ((size_t)label_len + 1U > query_size - cursor) {
      return 0U;
    }
    cursor += (size_t)label_len + 1U;
    ++labels;
  }
  if (cursor > query_size || query_size - cursor < 4U) {
    return 0U;
  }

  qtype = read_u16_be(query + cursor);
  qclass = read_u16_be(query + cursor + 2U);
  cursor += 4U;

  const bool answer_a =
      qclass == DNS_CLASS_IN && (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY);
  const size_t response_size = cursor + (answer_a ? (size_t)DNS_ANSWER_SIZE : 0U);
  if (response_size > response_capacity) {
    return 0U;
  }

  /* Copy ID + first question verbatim. */
  memcpy(response, query, cursor);
  /* QR=1, AA=1, TC=0, RD copied, RA=0, RCODE=0. */
  write_u16_be(response + 2U, (uint16_t)(0x8400U | (flags & 0x0100U)));
  write_u16_be(response + 4U, 1U);
  write_u16_be(response + 6U, answer_a ? 1U : 0U);
  write_u16_be(response + 8U, 0U);
  write_u16_be(response + 10U, 0U);

  if (answer_a) {
    uint8_t *answer = response + cursor;
    /* Name pointer to QNAME at offset 12 (0xC00C). */
    write_u16_be(answer + 0U, 0xc00cU);
    write_u16_be(answer + 2U, DNS_TYPE_A);
    write_u16_be(answer + 4U, DNS_CLASS_IN);
    write_u32_be(answer + 6U, DNS_TTL_SECONDS);
    write_u16_be(answer + 10U, 4U);
    memcpy(answer + 12U, ip4, 4U);
  }
  return response_size;
}
