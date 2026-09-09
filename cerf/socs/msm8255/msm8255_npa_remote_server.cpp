#include "msm8255_npa_remote_server.h"

#include "msm8255_rpc_router_peer.h"
#include "msm8255_rpcrouter_wire.h"

#include "../../boards/board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../cpu/emulated_memory.h"
#include "../../state/state_stream.h"
#include "../guest_cpu_reset.h"

#include <cstdint>

namespace {

constexpr uint32_t kNpaProg   = 0x300000A4u;
constexpr uint32_t kNpaVers   = 0x00010001u;
constexpr uint32_t kNpaProc   = 2u;
constexpr uint32_t kNpaCid    = 1u;
constexpr uint32_t kNpaResult = 0u;

constexpr uint32_t kCallPayloadBytes = 64u;
constexpr uint32_t kReplyBodyBytes   = 28u;

constexpr uint32_t kCallArg0Off    = kCallArgsOff +  0u;
constexpr uint32_t kCallArg1Off    = kCallArgsOff +  4u;
constexpr uint32_t kCallArg2Off    = kCallArgsOff +  8u;
constexpr uint32_t kCallArgCbOff   = kCallArgsOff + 12u;
constexpr uint32_t kCallArgNodeOff = kCallArgsOff + 16u;

constexpr uint32_t kDefineArg0       = 1u;
constexpr uint32_t kNoCallbackHandle = 0xFFFFFFFFu;

constexpr uint32_t kCbProg      = 0x310000A4u;
constexpr uint32_t kCbProc      = 1u;
constexpr uint32_t kCbClientCid = 2u;
constexpr uint32_t kCbBodyBytes = 60u;
constexpr uint32_t kCbReplyBytes = 28u;

constexpr uint32_t kCbArgIndexOff = kCallArgsOff +  0u;
constexpr uint32_t kCbArgNodeOff  = kCallArgsOff +  4u;
constexpr uint32_t kCbArgSpareOff = kCallArgsOff +  8u;
constexpr uint32_t kCbArgCountOff = kCallArgsOff + 12u;
constexpr uint32_t kCbArgLastOff  = kCallArgsOff + 16u;

}

bool Msm8255NpaRemoteServer::ShouldRegister() {
    auto* bd = emu_.TryGet<BoardContext>();
    return bd && bd->GetSoc() == SocFamily::MSM8255;
}

void Msm8255NpaRemoteServer::OnReady() {
    emu_.Get<GuestCpuReset>().RegisterResetListener([this](ResetLineKind) {
        next_xid_       = 1;
        cb_xid_         = 0;
        cb_outstanding_ = false;
    });
}

uint32_t Msm8255NpaRemoteServer::ServerProg() const { return kNpaProg; }
uint32_t Msm8255NpaRemoteServer::ServerVers() const { return kNpaVers; }
uint32_t Msm8255NpaRemoteServer::ServerCid() const { return kNpaCid; }
uint32_t Msm8255NpaRemoteServer::CallbackClientCid() const {
    return kCbClientCid;
}

uint32_t Msm8255NpaRemoteServer::AnswerCall(uint32_t in_pa, uint32_t size,
                                            uint32_t out_pa, uint32_t out_cap,
                                            uint32_t self_pid,
                                            uint32_t peer_pid,
                                            uint32_t peer_cid) {
    auto& mem    = emu_.Get<EmulatedMemory>();
    auto& router = emu_.Get<Msm8255RpcRouterPeer>();

    if (size != kCallPayloadBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc payload is %u bytes, and only the "
            "%u-byte call is modeled", size, kCallPayloadBytes);
    }

    router.ValidatePacmark(mem.ReadWord(in_pa + kHdrBytes),
                           kCallPayloadBytes - kPacmarkBytes);

    const uint32_t body = in_pa + kHdrBytes + kPacmarkBytes;
    const uint32_t xid  = Be32(mem.ReadWord(body + kCallXidOff));
    const uint32_t type = Be32(mem.ReadWord(body + kCallTypeOff));
    const uint32_t rpcv = Be32(mem.ReadWord(body + kCallRpcVersOff));
    const uint32_t prog = Be32(mem.ReadWord(body + kCallProgOff));
    const uint32_t vers = Be32(mem.ReadWord(body + kCallVersOff));
    const uint32_t proc = Be32(mem.ReadWord(body + kCallProcOff));

    if (type != kOncrpcCall || rpcv != kOncrpcVersion) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc message type %u version %u is not "
            "modeled", type, rpcv);
    }
    if (prog != kNpaProg || vers != kNpaVers || proc != kNpaProc) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc call prog 0x%08X vers 0x%08X proc "
            "%u is not modeled", prog, vers, proc);
    }

    const uint32_t cred_flavor = Be32(mem.ReadWord(body + kCallCredFlavorOff));
    const uint32_t cred_len    = Be32(mem.ReadWord(body + kCallCredLenOff));
    const uint32_t verf_flavor = Be32(mem.ReadWord(body + kCallVerfFlavorOff));
    const uint32_t verf_len    = Be32(mem.ReadWord(body + kCallVerfLenOff));
    if (cred_flavor != kAuthNone || cred_len != 0u ||
        verf_flavor != kAuthNone || verf_len != 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc call carries cred flavor %u length "
            "%u and verf flavor %u length %u, and only an unauthenticated call "
            "is modeled", cred_flavor, cred_len, verf_flavor, verf_len);
    }

    const uint32_t arg0 = Be32(mem.ReadWord(body + kCallArg0Off));
    const uint32_t arg1 = Be32(mem.ReadWord(body + kCallArg1Off));
    const uint32_t arg2 = Be32(mem.ReadWord(body + kCallArg2Off));
    if (arg0 != kDefineArg0 || arg1 != 0u || arg2 != 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc call arguments %u %u %u are not the "
            "request this peer models", arg0, arg1, arg2);
    }

    const uint32_t callback = Be32(mem.ReadWord(body + kCallArgCbOff));
    const uint32_t node     = Be32(mem.ReadWord(body + kCallArgNodeOff));

    const uint32_t reply_bytes = kHdrBytes + kPacmarkBytes + kReplyBodyBytes;
    const uint32_t cb_bytes    = callback != kNoCallbackHandle
                                     ? kHdrBytes + kPacmarkBytes + kCbBodyBytes
                                     : 0u;
    if (out_cap < reply_bytes + cb_bytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the modem fifo has %u contiguous bytes "
            "free, and the reply plus callback need %u", out_cap,
            reply_bytes + cb_bytes);
    }

    router.WriteHeader(out_pa, kCtrlCmdData, self_pid, kNpaCid,
                       kPacmarkBytes + kReplyBodyBytes, peer_pid, peer_cid);
    mem.WriteWord(out_pa + kHdrBytes, router.NextPacmark(kReplyBodyBytes));

    const uint32_t out = out_pa + kHdrBytes + kPacmarkBytes;
    mem.WriteWord(out + kReplyXidOff,        Be32(xid));
    mem.WriteWord(out + kReplyTypeOff,       Be32(kOncrpcReply));
    mem.WriteWord(out + kReplyStatOff,       Be32(kMsgAccepted));
    mem.WriteWord(out + kReplyVerfFlavorOff, Be32(kAuthNone));
    mem.WriteWord(out + kReplyVerfLenOff,    Be32(0u));
    mem.WriteWord(out + kReplyAcceptStatOff, Be32(kAcceptSuccess));
    mem.WriteWord(out + kReplyResultsOff,    Be32(kNpaResult));

    uint32_t written = reply_bytes;
    if (cb_bytes != 0u) {
        written += EmitCallback(out_pa + written, self_pid, callback, node);
    }
    return written + router.AnswerConfirmRx(in_pa, out_pa, out_cap, written);
}

uint32_t Msm8255NpaRemoteServer::EmitCallback(uint32_t out_pa,
                                              uint32_t self_pid,
                                              uint32_t cb_index,
                                              uint32_t node) {
    auto& mem    = emu_.Get<EmulatedMemory>();
    auto& router = emu_.Get<Msm8255RpcRouterPeer>();

    uint32_t srv_pid = 0;
    uint32_t srv_cid = 0;
    if (!router.AnnouncedServer(srv_pid, srv_cid)) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the guest registered callback index %u "
            "before announcing the server endpoint that receives it", cb_index);
    }
    if (cb_outstanding_) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: callback xid %u is still unanswered, "
            "and more than one outstanding callback is not modeled", cb_xid_);
    }

    cb_xid_         = ++next_xid_;
    cb_outstanding_ = true;

    router.WriteHeader(out_pa, kCtrlCmdData, self_pid, kCbClientCid,
                       kPacmarkBytes + kCbBodyBytes, srv_pid, srv_cid);
    mem.WriteWord(out_pa + kHdrBytes, router.NextPacmark(kCbBodyBytes));

    const uint32_t out = out_pa + kHdrBytes + kPacmarkBytes;
    mem.WriteWord(out + kCallXidOff,        Be32(cb_xid_));
    mem.WriteWord(out + kCallTypeOff,       Be32(kOncrpcCall));
    mem.WriteWord(out + kCallRpcVersOff,    Be32(kOncrpcVersion));
    mem.WriteWord(out + kCallProgOff,       Be32(kCbProg));
    mem.WriteWord(out + kCallVersOff,       Be32(kNpaVers));
    mem.WriteWord(out + kCallProcOff,       Be32(kCbProc));
    mem.WriteWord(out + kCallCredFlavorOff, Be32(kAuthNone));
    mem.WriteWord(out + kCallCredLenOff,    Be32(0u));
    mem.WriteWord(out + kCallVerfFlavorOff, Be32(kAuthNone));
    mem.WriteWord(out + kCallVerfLenOff,    Be32(0u));
    mem.WriteWord(out + kCbArgIndexOff,     Be32(cb_index));
    mem.WriteWord(out + kCbArgNodeOff,      Be32(node));
    mem.WriteWord(out + kCbArgSpareOff,     Be32(0u));
    mem.WriteWord(out + kCbArgCountOff,     Be32(0u));
    mem.WriteWord(out + kCbArgLastOff,      Be32(0u));
    return kHdrBytes + kPacmarkBytes + kCbBodyBytes;
}

uint32_t Msm8255NpaRemoteServer::ConsumeCallbackReply(uint32_t in_pa,
                                                      uint32_t size,
                                                      uint32_t out_pa,
                                                      uint32_t out_cap) {
    auto& mem    = emu_.Get<EmulatedMemory>();
    auto& router = emu_.Get<Msm8255RpcRouterPeer>();

    if (size != kPacmarkBytes + kCbReplyBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the callback reply is %u bytes, and "
            "only the %u-byte accepted reply is modeled", size,
            kPacmarkBytes + kCbReplyBytes);
    }
    if (!cb_outstanding_) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the guest answered a callback that this "
            "peer never issued");
    }

    router.ValidatePacmark(mem.ReadWord(in_pa + kHdrBytes), kCbReplyBytes);

    const uint32_t body = in_pa + kHdrBytes + kPacmarkBytes;
    const uint32_t xid  = Be32(mem.ReadWord(body + kReplyXidOff));
    const uint32_t type = Be32(mem.ReadWord(body + kReplyTypeOff));
    const uint32_t stat = Be32(mem.ReadWord(body + kReplyStatOff));
    const uint32_t vfl  = Be32(mem.ReadWord(body + kReplyVerfFlavorOff));
    const uint32_t vlen = Be32(mem.ReadWord(body + kReplyVerfLenOff));
    const uint32_t acc  = Be32(mem.ReadWord(body + kReplyAcceptStatOff));

    if (xid != cb_xid_) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the callback reply carries xid %u and "
            "the outstanding callback is xid %u", xid, cb_xid_);
    }
    if (type != kOncrpcReply || stat != kMsgAccepted || acc != kAcceptSuccess) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the callback reply is type %u "
            "reply_stat %u accept_stat %u, and only an accepted success is "
            "modeled", type, stat, acc);
    }
    if (vfl != kAuthNone || vlen != 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the callback reply verifier is flavor "
            "%u length %u, and only the null verifier is modeled", vfl, vlen);
    }

    const uint32_t rc = Be32(mem.ReadWord(body + kReplyResultsOff));
    if (rc != kNpaResult) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the guest's node-define callback "
            "returned %u", rc);
    }

    cb_outstanding_ = false;
    return router.AnswerConfirmRx(in_pa, out_pa, out_cap, 0u);
}

void Msm8255NpaRemoteServer::SaveState(StateWriter& w) {
    w.Write<uint32_t>(next_xid_);
    w.Write<uint32_t>(cb_xid_);
    w.Write<uint32_t>(cb_outstanding_ ? 1u : 0u);
}

void Msm8255NpaRemoteServer::RestoreState(StateReader& r) {
    uint32_t outstanding = 0;
    r.Read(next_xid_);
    r.Read(cb_xid_);
    r.Read(outstanding);
    cb_outstanding_ = outstanding != 0u;
}

REGISTER_SERVICE(Msm8255NpaRemoteServer);
