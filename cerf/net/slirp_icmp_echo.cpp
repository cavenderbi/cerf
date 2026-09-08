#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "slirp_backend.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../core/cerf_emulator.h"
#include "../core/log.h"
#include "../state/emulation_freeze.h"

namespace {

/* Internet checksum (RFC 1071) over the ICMP header+payload we built
   ourselves. The IP header's own checksum is computed inline by the
   caller because it's a fixed 20 bytes with no payload dependency. */
uint16_t InetChecksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (len & 1) sum += (uint32_t)(data[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Identify an outbound IPv4 ICMP echo request. Returns false if the
   frame is anything else (caller falls through to slirp_input). */
struct IcmpEchoRequest {
    std::array<uint8_t, 6> guest_mac;
    uint32_t guest_ip_n;   /* network-byte-order */
    uint32_t dest_ip_n;    /* network-byte-order */
    uint16_t id;           /* host order */
    uint16_t seq;          /* host order */
    std::vector<uint8_t> payload;
};

bool ParseIcmpEchoRequest(const uint8_t* frame, size_t len,
                          IcmpEchoRequest& out) {
    /* Min: 14 (Eth) + 20 (IP) + 8 (ICMP) = 42. */
    if (len < 42) return false;
    if (!(frame[12] == 0x08 && frame[13] == 0x00)) return false;  /* IPv4 */
    const uint8_t* ip = frame + 14;
    uint8_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || (size_t)14 + ihl + 8 > len) return false;
    if (ip[9] != 1 /* IPPROTO_ICMP */) return false;
    const uint8_t* icmp = frame + 14 + ihl;
    if (icmp[0] != 8 /* ICMP_ECHO_REQUEST */ || icmp[1] != 0) return false;
    std::memcpy(out.guest_mac.data(), frame + 6, 6);
    std::memcpy(&out.guest_ip_n, ip + 12, 4);
    std::memcpy(&out.dest_ip_n, ip + 16, 4);
    out.id  = (uint16_t)((icmp[4] << 8) | icmp[5]);
    out.seq = (uint16_t)((icmp[6] << 8) | icmp[7]);
    out.payload.assign(icmp + 8, frame + len);
    return true;
}

/* Build the reply frame from an IcmpSendEcho result. Caller has already
   confirmed er->Status == IP_SUCCESS. */
std::vector<uint8_t> BuildIcmpEchoReplyFrame(const IcmpEchoRequest& req,
                                             const ICMP_ECHO_REPLY& er,
                                             uint16_t mtu_cap) {
    std::vector<uint8_t> out;
    size_t icmp_data_len = er.DataSize;
    size_t max_data = (size_t)mtu_cap - 14 - 20 - 8;
    if (icmp_data_len > max_data) return out;  /* empty = caller drops */
    out.assign(14 + 20 + 8 + icmp_data_len, 0);

    /* Ethernet: dst = guest, src = libslirp gateway MAC. */
    std::memcpy(out.data() + 0, req.guest_mac.data(), 6);
    const uint8_t gw_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    std::memcpy(out.data() + 6, gw_mac, 6);
    out[12] = 0x08; out[13] = 0x00;

    /* IPv4 header, 20 bytes, no options. */
    uint8_t* oip = out.data() + 14;
    oip[0] = 0x45; oip[1] = 0;
    uint16_t total_len = (uint16_t)(20 + 8 + icmp_data_len);
    oip[2] = (uint8_t)(total_len >> 8); oip[3] = (uint8_t)total_len;
    oip[4] = 0; oip[5] = 1;  /* ID */
    oip[6] = 0; oip[7] = 0;  /* flags + frag offset */
    oip[8] = 64;             /* TTL */
    oip[9] = 1;              /* IPPROTO_ICMP */
    oip[10] = 0; oip[11] = 0;                /* checksum placeholder */
    std::memcpy(oip + 12, &req.dest_ip_n, 4);  /* src = original dst */
    std::memcpy(oip + 16, &req.guest_ip_n, 4); /* dst = guest */
    /* IPv4 header checksum: 16-bit ones-complement over the 20-byte header. */
    {
        uint32_t sum = 0;
        for (int i = 0; i < 20; i += 2)
            sum += (uint32_t)((oip[i] << 8) | oip[i + 1]);
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        uint16_t ck = (uint16_t)~sum;
        oip[10] = (uint8_t)(ck >> 8); oip[11] = (uint8_t)ck;
    }

    /* ICMP echo reply: type 0, echo id/seq, original payload. */
    uint8_t* oicmp = out.data() + 14 + 20;
    oicmp[0] = 0; oicmp[1] = 0;
    oicmp[2] = 0; oicmp[3] = 0;                        /* checksum placeholder */
    oicmp[4] = (uint8_t)(req.id  >> 8); oicmp[5] = (uint8_t)req.id;
    oicmp[6] = (uint8_t)(req.seq >> 8); oicmp[7] = (uint8_t)req.seq;
    if (icmp_data_len && er.Data)
        std::memcpy(oicmp + 8, er.Data, icmp_data_len);
    uint16_t icmp_ck = InetChecksum(oicmp, 8 + icmp_data_len);
    oicmp[2] = (uint8_t)(icmp_ck >> 8); oicmp[3] = (uint8_t)icmp_ck;
    return out;
}

} /* namespace */

bool SlirpBackend::TryInterceptIcmpEcho(const uint8_t* frame, std::size_t len) {
    IcmpEchoRequest req;
    if (!ParseIcmpEchoRequest(frame, len, req)) return false;

    uint16_t mtu_cap = (uint16_t)mtu_;

    auto done = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<void> mark_done(nullptr, [done](void*) {
        done->store(true, std::memory_order_release);
    });

    auto echo = [this, req = std::move(req), mtu_cap, mark_done]() mutable {
        HANDLE h = IcmpCreateFile();
        if (h == INVALID_HANDLE_VALUE) return;

        /* Reply buffer: sizeof(ICMP_ECHO_REPLY) + payload + 8 extra per
           MS guidance - keeps the ReplyBuffer large enough for the echo
           reply plus at least one ICMP_ERROR if the ping fails. */
        DWORD reply_size =
            (DWORD)(sizeof(ICMP_ECHO_REPLY) + req.payload.size() + 8);
        std::vector<uint8_t> reply_buf(reply_size);

        DWORD n = IcmpSendEcho(h, req.dest_ip_n,
                               req.payload.empty() ? nullptr : req.payload.data(),
                               (WORD)req.payload.size(),
                               nullptr,
                               reply_buf.data(), reply_size,
                               1000 /* ms */);
        IcmpCloseHandle(h);
        if (n == 0) {
            LOG(Net, "ICMP echo to 0x%08X: no reply (err=%lu)\n",
                req.dest_ip_n, GetLastError());
            return;
        }
        auto* er = reinterpret_cast<ICMP_ECHO_REPLY*>(reply_buf.data());
        if (er->Status != IP_SUCCESS) {
            LOG(Net, "ICMP echo to 0x%08X: status=%lu\n",
                req.dest_ip_n, er->Status);
            return;
        }
        auto reply_frame = BuildIcmpEchoReplyFrame(req, *er, mtu_cap);
        if (reply_frame.empty()) return;

        auto frozen = emu_.Get<EmulationFreeze>().WorkerSection();
        DispatchFrame(reply_frame.data(), reply_frame.size());
    };

    std::lock_guard<std::mutex> lk(icmp_mutex_);
    if (icmp_stopping_) return true;   /* shutdown joined the live set; drop the echo */

    for (auto it = icmp_threads_.begin(); it != icmp_threads_.end();) {
        if (it->done->load(std::memory_order_acquire)) {
            it->thread.join();
            it = icmp_threads_.erase(it);
        } else {
            ++it;
        }
    }
    icmp_threads_.push_back({std::thread(std::move(echo)), std::move(done)});
    return true;
}

void SlirpBackend::JoinIcmpThreads() {
    std::vector<IcmpEcho> live;
    {
        std::lock_guard<std::mutex> lk(icmp_mutex_);
        icmp_stopping_ = true;
        live.swap(icmp_threads_);
    }
    for (auto& e : live)
        if (e.thread.joinable()) e.thread.join();
}
