#include "iterate/kit/captive_dns.h"

#include <assert.h>
#include <string.h>

static void answers_in_a_query_with_ap_ip(void) {
  /* Query for captive.apple.com, QTYPE=A (1), QCLASS=IN (1), ID=0x1234, RD=1. */
  const uint8_t query[] = {
    0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    7, 'c', 'a', 'p', 't', 'i', 'v', 'e',
    5, 'a', 'p', 'p', 'l', 'e',
    3, 'c', 'o', 'm', 0,
    0x00, 0x01, 0x00, 0x01,
  };
  const uint8_t ip4[4] = {192, 168, 4, 1};
  uint8_t reply[128];

  const size_t n = iterate_kit_captive_dns_respond(
      query, sizeof(query), ip4, reply, sizeof(reply));
  assert(n == sizeof(query) + 16U);
  /* Transaction ID copied, QR=1 AA=1 RD=1, QDCOUNT=1, ANCOUNT=1. */
  assert(reply[0] == 0x12U && reply[1] == 0x34U);
  assert(reply[2] == 0x85U && reply[3] == 0x00U);
  assert(reply[4] == 0x00U && reply[5] == 0x01U);
  assert(reply[6] == 0x00U && reply[7] == 0x01U);
  /* Answer section: pointer 0xC00C, TYPE=A, CLASS=IN, TTL=30, RDLEN=4, 192.168.4.1. */
  const uint8_t *ans = reply + sizeof(query);
  assert(ans[0] == 0xc0U && ans[1] == 0x0cU);
  assert(ans[2] == 0x00U && ans[3] == 0x01U);
  assert(ans[4] == 0x00U && ans[5] == 0x01U);
  assert(ans[9] == 30U);
  assert(ans[10] == 0x00U && ans[11] == 0x04U);
  assert(memcmp(ans + 12U, ip4, 4U) == 0);
}

static void returns_nodata_for_aaaa_queries(void) {
  const uint8_t query[] = {
    0xab, 0xcd, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    4, 't', 'e', 's', 't', 0,
    0x00, 0x1c, 0x00, 0x01, /* QTYPE=AAAA (28), QCLASS=IN */
  };
  const uint8_t ip4[4] = {192, 168, 4, 1};
  uint8_t reply[64];

  const size_t n = iterate_kit_captive_dns_respond(
      query, sizeof(query), ip4, reply, sizeof(reply));
  assert(n == sizeof(query));
  assert(reply[2] == 0x85U && reply[3] == 0x00U);
  assert(reply[6] == 0x00U && reply[7] == 0x00U);
}

static void rejects_malformed_and_oversized_packets(void) {
  const uint8_t ip4[4] = {192, 168, 4, 1};
  uint8_t reply[64];
  /* Truncated header. */
  assert(iterate_kit_captive_dns_respond(ip4, 4U, ip4, reply, sizeof(reply)) == 0U);
  /* Response packet (QR=1). */
  const uint8_t resp_pkt[18] = {0, 1, 0x80, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0};
  assert(iterate_kit_captive_dns_respond(resp_pkt, sizeof(resp_pkt), ip4, reply, sizeof(reply)) == 0U);
  /* Compression pointer inside query QNAME (illegal / loop hazard). */
  const uint8_t ptr_pkt[18] = {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xc0, 0x0c, 0, 1, 0, 1};
  assert(iterate_kit_captive_dns_respond(ptr_pkt, sizeof(ptr_pkt), ip4, reply, sizeof(reply)) == 0U);
  /* Label length running past packet end. */
  const uint8_t overrun_pkt[16] = {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 20, 'a', 'b', 'c'};
  assert(iterate_kit_captive_dns_respond(overrun_pkt, sizeof(overrun_pkt), ip4, reply, sizeof(reply)) == 0U);
  /* Too-small output buffer. */
  const uint8_t ok_pkt[] = {
    0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 0, 0, 1, 0, 1,
  };
  assert(iterate_kit_captive_dns_respond(ok_pkt, sizeof(ok_pkt), ip4, reply, 20U) == 0U);
}

int main(void) {
  answers_in_a_query_with_ap_ip();
  returns_nodata_for_aaaa_queries();
  rejects_malformed_and_oversized_packets();
  return 0;
}
