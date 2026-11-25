#ifndef ITERATE_KIT_CAPTIVE_DNS_H
#define ITERATE_KIT_CAPTIVE_DNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Builds an RFC 1035 DNS response for a captive portal SoftAP.
 *
 * For standard IN A (and IN ANY) queries, returns a single A record answering
 * with `ip4` (TTL 30 s) so iOS/Android captive-network probes resolve every
 * hostname to the board's SoftAP IP (`192.168.4.1`). For other query types
 * (such as AAAA or HTTPS/SVCB), returns a clean NOERROR/NODATA response so
 * dual-stack phones fall back immediately to IPv4 without timing out.
 *
 * Returns the byte length written to `response`, or 0 if `query` is malformed
 * or does not fit in `response_capacity`. Pure and allocation-free.
 */
size_t iterate_kit_captive_dns_respond(
    const uint8_t *query,
    size_t query_size,
    const uint8_t ip4[4],
    uint8_t *response,
    size_t response_capacity);

#ifdef __cplusplus
}
#endif

#endif
