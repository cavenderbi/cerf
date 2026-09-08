#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "slirp_backend.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "../core/log.h"

namespace {

constexpr uint16_t QTYPE_AAAA = 28;

/* Ones-complement checksum over a buffer (RFC 1071). Used for IPv4 header
   and UDP (with pseudo-header). */
uint16_t InetSum(const uint8_t* data, size_t len, uint32_t seed = 0) {
    uint32_t sum = seed;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (len & 1) sum += (uint32_t)(data[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Walk DNS question name (labels + 00 terminator; compression not expected
   in a query). Returns offset just past the terminator, or 0 on malformed
   input. Caller must ensure `off` is inside `dns_len`. */
size_t SkipDnsName(const uint8_t* dns, size_t dns_len, size_t off) {
    while (off < dns_len) {
        uint8_t len = dns[off];
        if (len == 0) return off + 1;
        if ((len & 0xC0) != 0) return 0;  /* compression not allowed in question */
        off += (size_t)len + 1;
    }
    return 0;
}

/* Returns true if `frame` is a well-formed DNS query for QTYPE=AAAA and
   fills `*q_name_end_off` with the offset into the DNS payload just past
   the question name's null terminator (i.e. the start of QTYPE/QCLASS). */
bool IsAaaaQuery(const uint8_t* frame, size_t len, size_t* dns_off_out,
                 size_t* dns_len_out, size_t* q_name_end_off_out) {
    if (len < 14 + 20 + 8 + 12 + 5) return false;               /* ETH+IP+UDP+DNShdr+"\0"+QTYPE+QCLASS */
    if (!(frame[12] == 0x08 && frame[13] == 0x00)) return false; /* IPv4 ethertype */

    const uint8_t* ip = frame + 14;
    if ((ip[0] & 0xF0) != 0x40) return false;                   /* IPv4 version */
    uint8_t ihl = (uint8_t)((ip[0] & 0x0F) * 4);
    if (ihl < 20) return false;
    if (ip[9] != 17) return false;                              /* proto == UDP */

    size_t udp_off = 14 + (size_t)ihl;
    if (udp_off + 8 > len) return false;
    const uint8_t* udp = frame + udp_off;
    uint16_t dst_port = (uint16_t)((udp[2] << 8) | udp[3]);
    if (dst_port != 53) return false;

    uint16_t udp_len_field = (uint16_t)((udp[4] << 8) | udp[5]);
    if (udp_len_field < 8) return false;
    size_t dns_off = udp_off + 8;
    size_t dns_len = (size_t)udp_len_field - 8;
    if (dns_off + dns_len > len) return false;
    if (dns_len < 12 + 5) return false;                          /* header + minimal question */

    const uint8_t* dns = frame + dns_off;
    uint16_t flags    = (uint16_t)((dns[2] << 8) | dns[3]);
    if (flags & 0x8000) return false;                            /* QR bit set → it's a response, not a query */
    uint16_t qd_count = (uint16_t)((dns[4] << 8) | dns[5]);
    if (qd_count != 1) return false;                             /* only plain single-question queries */

    size_t name_end = SkipDnsName(dns, dns_len, 12);
    if (name_end == 0 || name_end + 4 > dns_len) return false;

    uint16_t qtype  = (uint16_t)((dns[name_end] << 8) | dns[name_end + 1]);
    if (qtype != QTYPE_AAAA) return false;

    *dns_off_out        = dns_off;
    *dns_len_out        = dns_len;
    *q_name_end_off_out = name_end;
    return true;
}

/* Build a NoData response frame from the query frame. The response
   echoes the question section verbatim, sets QR=1/AA=0/RA=1/RCODE=0
   (NoError, NoData), and ANCOUNT=NSCOUNT=ARCOUNT=0. Swaps src/dst MAC,
   IP, and UDP port; recomputes IPv4 and UDP checksums. */
std::vector<uint8_t> BuildAaaaNoDataReply(const uint8_t* query_frame,
                                          size_t query_len,
                                          size_t dns_off,
                                          size_t q_name_end) {
    /* Response DNS payload: 12-byte header + question section (up to
       QTYPE+QCLASS after the name terminator) = q_name_end + 4. */
    size_t resp_dns_len = q_name_end + 4;
    if (dns_off + resp_dns_len > query_len) return {};           /* sanity */

    size_t resp_udp_len = 8 + resp_dns_len;
    size_t resp_ip_len  = 20 + resp_udp_len;
    size_t resp_total   = 14 + resp_ip_len;

    std::vector<uint8_t> out(resp_total, 0);

    /* Ethernet: swap src/dst. */
    std::memcpy(out.data() + 0, query_frame + 6, 6);             /* dst ← query src (guest) */
    std::memcpy(out.data() + 6, query_frame + 0, 6);             /* src ← query dst (slirp DNS) */
    out[12] = 0x08; out[13] = 0x00;

    /* IPv4 header - fixed 20-byte, no options. */
    uint8_t* oip = out.data() + 14;
    oip[0] = 0x45;                                               /* ver=4, ihl=5 */
    oip[1] = 0;                                                  /* DSCP/ECN */
    oip[2] = (uint8_t)(resp_ip_len >> 8); oip[3] = (uint8_t)resp_ip_len;
    oip[4] = 0; oip[5] = 1;                                      /* ID */
    oip[6] = 0; oip[7] = 0;                                      /* flags + frag offset */
    oip[8] = 64;                                                 /* TTL */
    oip[9] = 17;                                                 /* UDP */
    oip[10] = 0; oip[11] = 0;                                    /* header checksum placeholder */

    const uint8_t* qip = query_frame + 14;
    uint8_t qihl = (uint8_t)((qip[0] & 0x0F) * 4);
    std::memcpy(oip + 12, qip + 16, 4);                          /* src IP ← query dst IP (slirp DNS) */
    std::memcpy(oip + 16, qip + 12, 4);                          /* dst IP ← query src IP (guest)    */

    /* IPv4 header checksum (16-bit ones-complement over the 20-byte header). */
    uint16_t ip_ck = InetSum(oip, 20, 0);
    oip[10] = (uint8_t)(ip_ck >> 8); oip[11] = (uint8_t)ip_ck;

    /* UDP header - swap ports; checksum computed after DNS payload written. */
    uint8_t* oudp = out.data() + 14 + 20;
    const uint8_t* qudp = query_frame + 14 + qihl;
    oudp[0] = qudp[2]; oudp[1] = qudp[3];                        /* src port ← query dst port (53) */
    oudp[2] = qudp[0]; oudp[3] = qudp[1];                        /* dst port ← query src port      */
    oudp[4] = (uint8_t)(resp_udp_len >> 8); oudp[5] = (uint8_t)resp_udp_len;
    oudp[6] = 0; oudp[7] = 0;                                    /* checksum placeholder */

    /* DNS header. */
    uint8_t* odns = out.data() + 14 + 20 + 8;
    const uint8_t* qdns = query_frame + dns_off;
    odns[0] = qdns[0]; odns[1] = qdns[1];                        /* transaction ID */
    /* Flags: QR=1, Opcode=0, AA=0, TC=0, RD=(echo client's), RA=1, Z=0, RCODE=0. */
    uint8_t rd = (uint8_t)(qdns[2] & 0x01);                      /* preserve RD flag */
    odns[2] = (uint8_t)(0x80 | rd);
    odns[3] = (uint8_t)(0x80);                                   /* RA=1, RCODE=0 (NoError) */
    odns[4] = 0; odns[5] = 1;                                    /* QDCOUNT=1 */
    odns[6] = 0; odns[7] = 0;                                    /* ANCOUNT=0 - NoData */
    odns[8] = 0; odns[9] = 0;                                    /* NSCOUNT=0 */
    odns[10] = 0; odns[11] = 0;                                  /* ARCOUNT=0 */

    /* Copy question section verbatim (name + QTYPE + QCLASS). */
    std::memcpy(odns + 12, qdns + 12, resp_dns_len - 12);

    /* UDP checksum: pseudo-header (src_ip, dst_ip, 0, proto, udp_len) +
       UDP header + data. Mandatory for the reply even though IPv4 senders
       can leave it zero; CE5 resolvers that validate it will drop a zero. */
    uint8_t pseudo[12] = {};
    std::memcpy(pseudo + 0, oip + 12, 4);                        /* src IP (slirp DNS) */
    std::memcpy(pseudo + 4, oip + 16, 4);                        /* dst IP (guest)     */
    pseudo[8]  = 0;
    pseudo[9]  = 17;                                             /* UDP */
    pseudo[10] = (uint8_t)(resp_udp_len >> 8);
    pseudo[11] = (uint8_t)resp_udp_len;
    uint32_t seed = 0;
    for (int i = 0; i < 12; i += 2) seed += (uint32_t)((pseudo[i] << 8) | pseudo[i + 1]);
    uint16_t udp_ck = InetSum(oudp, resp_udp_len, seed);
    if (udp_ck == 0) udp_ck = 0xFFFF;                            /* 0 means "no checksum" in UDP */
    oudp[6] = (uint8_t)(udp_ck >> 8); oudp[7] = (uint8_t)udp_ck;

    return out;
}

} /* namespace */

bool SlirpBackend::TryInterceptAaaaQuery(const uint8_t* frame, std::size_t len) {
    if (host_has_v6_) return false;                              /* IPv6 works - let AAAA through */

    size_t dns_off = 0, dns_len = 0, q_name_end = 0;
    if (!IsAaaaQuery(frame, len, &dns_off, &dns_len, &q_name_end)) return false;

    std::vector<uint8_t> reply =
        BuildAaaaNoDataReply(frame, len, dns_off, q_name_end);
    if (reply.empty()) return false;

    DispatchFrame(reply.data(), reply.size());

    LOG(Net, "AAAA NoData synthesized (host has no v6 internet; "
             "%u-byte query → %zu-byte reply)\n",
        (unsigned)len, reply.size());
    return true;
}
