#define _CRT_SECURE_NO_WARNINGS
#include "slirp_backend_internal.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

void ClassifyFrame(const uint8_t* f, std::size_t len,
                   char* out, std::size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (len < 14) { _snprintf_s(out, out_len, _TRUNCATE, "short<14"); return; }
    uint16_t etype = (uint16_t(f[12]) << 8) | f[13];
    if (etype == 0x0806) {
        if (len >= 42) {
            uint16_t op = (uint16_t(f[20]) << 8) | f[21];
            _snprintf_s(out, out_len, _TRUNCATE,
                        "arp op=%u sender=%u.%u.%u.%u target=%u.%u.%u.%u", op,
                        f[28], f[29], f[30], f[31], f[38], f[39], f[40], f[41]);
        } else if (len >= 22) {
            uint16_t op = (uint16_t(f[20]) << 8) | f[21];
            _snprintf_s(out, out_len, _TRUNCATE, "arp op=%u", op);
        } else {
            _snprintf_s(out, out_len, _TRUNCATE, "arp");
        }
        return;
    }
    if (etype == 0x0800 && len >= 34) {
        uint8_t proto = f[14 + 9];
        const uint8_t* ip = f + 14;
        uint8_t ihl = (ip[0] & 0x0F) * 4;
        if (len < 14u + ihl) {
            _snprintf_s(out, out_len, _TRUNCATE, "ipv4 proto=%u ihl=%u short", proto, ihl);
            return;
        }
        const uint8_t* l4 = ip + ihl;
        char sip[16] = {}, dip[16] = {};
        _snprintf_s(sip, sizeof(sip), _TRUNCATE, "%u.%u.%u.%u",
                    ip[12], ip[13], ip[14], ip[15]);
        _snprintf_s(dip, sizeof(dip), _TRUNCATE, "%u.%u.%u.%u",
                    ip[16], ip[17], ip[18], ip[19]);
        if (proto == 17 && len >= 14u + ihl + 8u) {
            uint16_t sp = (uint16_t(l4[0]) << 8) | l4[1];
            uint16_t dp = (uint16_t(l4[2]) << 8) | l4[3];
            const char* hint = (sp == 68 || dp == 68 || sp == 67 || dp == 67)
                ? " dhcp"
                : (sp == 53 || dp == 53 ? " dns" : "");
            _snprintf_s(out, out_len, _TRUNCATE,
                        "ipv4 udp %s:%u -> %s:%u%s", sip, sp, dip, dp, hint);
        } else if (proto == 6 && len >= 14u + ihl + 20u) {
            uint16_t sp = (uint16_t(l4[0]) << 8) | l4[1];
            uint16_t dp = (uint16_t(l4[2]) << 8) | l4[3];
            uint8_t  flg = l4[13];
            char flags[8] = {};
            int n = 0;
            if (flg & 0x02) flags[n++] = 'S';
            if (flg & 0x10) flags[n++] = 'A';
            if (flg & 0x01) flags[n++] = 'F';
            if (flg & 0x04) flags[n++] = 'R';
            if (flg & 0x08) flags[n++] = 'P';
            if (n == 0) flags[n++] = '.';
            _snprintf_s(out, out_len, _TRUNCATE,
                        "ipv4 tcp %s:%u -> %s:%u [%s]", sip, sp, dip, dp, flags);
        } else if (proto == 1) {
            _snprintf_s(out, out_len, _TRUNCATE,
                        "ipv4 icmp %s -> %s", sip, dip);
        } else {
            _snprintf_s(out, out_len, _TRUNCATE,
                        "ipv4 proto=%u %s -> %s", proto, sip, dip);
        }
        return;
    }
    if (etype == 0x86DD) {
        _snprintf_s(out, out_len, _TRUNCATE, "ipv6");
        return;
    }
    _snprintf_s(out, out_len, _TRUNCATE, "etype=0x%04X", etype);
}
