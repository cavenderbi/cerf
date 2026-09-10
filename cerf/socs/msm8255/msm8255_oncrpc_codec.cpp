#include "msm8255_oncrpc_codec.h"

#include "msm8255_rpc_router_peer.h"
#include "msm8255_rpcrouter_wire.h"

#include "../../boards/board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../cpu/emulated_memory.h"

#include <cstdint>

bool Msm8255OncrpcCodec::ShouldRegister() {
    auto* bd = emu_.TryGet<BoardContext>();
    return bd && bd->GetSoc() == SocFamily::MSM8255;
}

/* RFC 4506 section 4.11: a string is its byte count as an unsigned integer,
   then that many bytes, then 0 to 3 zero bytes so the total is a multiple of
   four. */
uint32_t Msm8255OncrpcCodec::SkipXdrString(uint32_t body, uint32_t size,
                                           uint32_t off, uint32_t which) {
    auto& mem = emu_.Get<EmulatedMemory>();

    if (size < kPacmarkBytes + off + 4u) {
        emu_.Get<Fatal>().Die(
            "msm8255 oncrpc codec: the rpc call is %u bytes, which is short of "
            "the %u that carry the length of its string argument %u", size,
            kPacmarkBytes + off + 4u, which);
    }

    const uint32_t len = Be32(mem.ReadWord(body + off));
    if (len == 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 oncrpc codec: string argument %u of the rpc call is empty, "
            "and only a named one is modeled", which);
    }
    if (len > kRouterMsgSizeMax) {
        emu_.Get<Fatal>().Die(
            "msm8255 oncrpc codec: string argument %u of the rpc call is %u "
            "bytes, which does not fit a %u-byte router message", which, len,
            kRouterMsgSizeMax);
    }

    return off + 4u + ((len + 3u) & ~3u);
}

/* RFC 5531 section 9: an accepted reply is xid, msg_type, reply_stat, the verf
   opaque_auth pair, accept_stat, then the procedure results. */
uint32_t Msm8255OncrpcCodec::WriteAcceptedReply(
    uint32_t out_pa, uint32_t self_pid, uint32_t src_cid, uint32_t peer_pid,
    uint32_t peer_cid, uint32_t xid, const uint32_t* results,
    uint32_t result_words) {
    auto& mem    = emu_.Get<EmulatedMemory>();
    auto& router = emu_.Get<Msm8255RpcRouterPeer>();

    const uint32_t body_bytes = kReplyResultsOff + 4u * result_words;
    router.WriteHeader(out_pa, kCtrlCmdData, self_pid, src_cid,
                       kPacmarkBytes + body_bytes, peer_pid, peer_cid);
    mem.WriteWord(out_pa + kHdrBytes, router.NextPacmark(body_bytes));

    const uint32_t out = out_pa + kHdrBytes + kPacmarkBytes;
    mem.WriteWord(out + kReplyXidOff,        Be32(xid));
    mem.WriteWord(out + kReplyTypeOff,       Be32(kOncrpcReply));
    mem.WriteWord(out + kReplyStatOff,       Be32(kMsgAccepted));
    mem.WriteWord(out + kReplyVerfFlavorOff, Be32(kAuthNone));
    mem.WriteWord(out + kReplyVerfLenOff,    Be32(0u));
    mem.WriteWord(out + kReplyAcceptStatOff, Be32(kAcceptSuccess));
    for (uint32_t i = 0; i < result_words; ++i) {
        mem.WriteWord(out + kReplyResultsOff + 4u * i, Be32(results[i]));
    }
    return kHdrBytes + kPacmarkBytes + body_bytes;
}

REGISTER_SERVICE(Msm8255OncrpcCodec);
