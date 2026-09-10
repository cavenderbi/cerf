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
constexpr uint32_t kNpaCid    = 1u;
constexpr uint32_t kNpaResult = 0u;

constexpr uint32_t kProcDefineNode     = 2u;
constexpr uint32_t kProcCreateClient   = 3u;
constexpr uint32_t kProcDefineResource = 22u;

constexpr uint32_t kClientTypeMax = 9u;

/* RFC 4506 section 4.4: bool is enum { FALSE = 0, TRUE = 1 }. Section 4.19
   makes an optional-data field a union whose discriminant is such a bool. */
constexpr uint32_t kXdrTrue = 1u;

constexpr uint32_t kDefineResultWords       = 1u;
constexpr uint32_t kCreateClientResultWords = 3u;

constexpr uint32_t kCallPayloadBytes = 64u;
constexpr uint32_t kReplyBodyBytes   = 28u;

/* RFC 5531 section 9: reply_body is a union whose discriminant is reply_stat,
   so xid, msg_type and reply_stat appear on both of its arms. */
constexpr uint32_t kReplyCommonBytes = kReplyStatOff + 4u;

/* RFC 5531 section 9: accepted_reply is the verifier followed by a union whose
   discriminant is accept_stat, whose SUCCESS arm alone carries results and
   whose PROG_UNAVAIL, PROC_UNAVAIL, GARBAGE_ARGS and SYSTEM_ERR arms are void. */
constexpr uint32_t kReplyAcceptedBytes = kReplyAcceptStatOff + 4u;

constexpr uint32_t kCallArg0Off    = kCallArgsOff +  0u;
constexpr uint32_t kCallArg1Off    = kCallArgsOff +  4u;
constexpr uint32_t kCallArg2Off    = kCallArgsOff +  8u;
constexpr uint32_t kCallArgCbOff   = kCallArgsOff + 12u;
constexpr uint32_t kCallArgNodeOff = kCallArgsOff + 16u;

constexpr uint32_t kDefineArg0       = 1u;
constexpr uint32_t kNoCallbackHandle = 0xFFFFFFFFu;

constexpr uint32_t kCreateClientReplyBytes =
    kReplyResultsOff + 4u * kCreateClientResultWords;

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
        cb_proc_        = 0;
        cb_outstanding_ = false;

        next_client_handle_ = 0;
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

    if (size < kPacmarkBytes + kCallArgsOff) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc payload is %u bytes, which is short "
            "of the %u-byte call header", size, kPacmarkBytes + kCallArgsOff);
    }

    router.ValidatePacmark(mem.ReadWord(in_pa + kHdrBytes),
                           size - kPacmarkBytes);

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
    if (prog != kNpaProg || vers != kNpaVers) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc call prog 0x%08X vers 0x%08X is not "
            "modeled", prog, vers);
    }
    if (proc != kProcDefineNode && proc != kProcDefineResource &&
        proc != kProcCreateClient) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc procedure %u with a %u-byte payload "
            "is not modeled", proc, size);
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

    if (proc == kProcCreateClient) {
        return AnswerCreateClient(in_pa, body, size, out_pa, out_cap, self_pid,
                                  peer_pid, peer_cid, xid);
    }

    uint32_t callback = 0u;
    uint32_t node     = 0u;
    if (proc == kProcDefineNode) {
        ReadDefineNodeArgs(body, size, callback, node);
    } else {
        ReadDefineResourceArgs(body, size, callback, node);
    }

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

    const uint32_t results[kDefineResultWords] = {kNpaResult};
    uint32_t written = WriteAcceptedReply(out_pa, self_pid, peer_pid, peer_cid,
                                          xid, results, kDefineResultWords);
    if (cb_bytes != 0u) {
        written += EmitCallback(out_pa + written, self_pid, proc, callback,
                                node);
    }
    return written + router.AnswerConfirmRx(in_pa, out_pa, out_cap, written);
}

void Msm8255NpaRemoteServer::ReadDefineNodeArgs(uint32_t body, uint32_t size,
                                                uint32_t& callback,
                                                uint32_t& object) {
    auto& mem = emu_.Get<EmulatedMemory>();

    if (size != kCallPayloadBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the node-define call is %u bytes, and "
            "only the %u-byte form is modeled", size, kCallPayloadBytes);
    }

    const uint32_t arg0 = Be32(mem.ReadWord(body + kCallArg0Off));
    const uint32_t arg1 = Be32(mem.ReadWord(body + kCallArg1Off));
    const uint32_t arg2 = Be32(mem.ReadWord(body + kCallArg2Off));
    if (arg0 != kDefineArg0 || arg1 != 0u || arg2 != 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: rpc call arguments %u %u %u are not the "
            "request this peer models", arg0, arg1, arg2);
    }

    callback = Be32(mem.ReadWord(body + kCallArgCbOff));
    object   = Be32(mem.ReadWord(body + kCallArgNodeOff));
}

/* RFC 4506 section 4.11: a string is its byte count as an unsigned integer,
   then that many bytes, then 0 to 3 zero bytes so the total is a multiple of
   four. */
uint32_t Msm8255NpaRemoteServer::SkipXdrString(uint32_t body, uint32_t size,
                                               uint32_t off, uint32_t which) {
    auto& mem = emu_.Get<EmulatedMemory>();

    if (size < kPacmarkBytes + off + 4u) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the rpc call is %u bytes, which is short "
            "of the %u that carry the length of its string argument %u", size,
            kPacmarkBytes + off + 4u, which);
    }

    const uint32_t len = Be32(mem.ReadWord(body + off));
    if (len == 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: string argument %u of the rpc call is "
            "empty, and only a named one is modeled", which);
    }
    if (len > kRouterMsgSizeMax) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: string argument %u of the rpc call is "
            "%u bytes, which does not fit a %u-byte router message", which, len,
            kRouterMsgSizeMax);
    }

    return off + 4u + ((len + 3u) & ~3u);
}

void Msm8255NpaRemoteServer::ReadDefineResourceArgs(uint32_t body,
                                                    uint32_t size,
                                                    uint32_t& callback,
                                                    uint32_t& object) {
    auto& mem = emu_.Get<EmulatedMemory>();

    const uint32_t off  = SkipXdrString(body, size, kCallArgsOff, 1u);
    const uint32_t want = kPacmarkBytes + off + 8u;
    if (size != want) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the resource-define call is %u bytes, "
            "and its name leaves its two trailing arguments needing %u", size,
            want);
    }

    callback = Be32(mem.ReadWord(body + off));
    object   = Be32(mem.ReadWord(body + off + 4u));
}

void Msm8255NpaRemoteServer::ReadCreateClientArgs(uint32_t body, uint32_t size,
                                                  uint32_t& type,
                                                  uint32_t& supplied) {
    auto& mem = emu_.Get<EmulatedMemory>();

    const uint32_t resource = SkipXdrString(body, size, kCallArgsOff, 1u);
    const uint32_t client   = SkipXdrString(body, size, resource, 2u);
    const uint32_t want     = kPacmarkBytes + client + 8u;
    if (size != want) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the create-client call is %u bytes, and "
            "its two names leave its type and out-pointer needing %u", size,
            want);
    }

    type     = Be32(mem.ReadWord(body + client));
    supplied = Be32(mem.ReadWord(body + client + 4u));
    if (type > kClientTypeMax) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the create-client call asks for client "
            "type %u, and the guest encodes only types 0 through %u", type,
            kClientTypeMax);
    }
    if (supplied != kXdrTrue) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the create-client call passes %u for the "
            "out-pointer it supplied, and only a supplied one is modeled",
            supplied);
    }
}

uint32_t Msm8255NpaRemoteServer::AnswerCreateClient(
    uint32_t in_pa, uint32_t body, uint32_t size, uint32_t out_pa,
    uint32_t out_cap, uint32_t self_pid, uint32_t peer_pid, uint32_t peer_cid,
    uint32_t xid) {
    uint32_t type     = 0u;
    uint32_t supplied = 0u;
    ReadCreateClientArgs(body, size, type, supplied);

    const uint32_t reply_bytes =
        kHdrBytes + kPacmarkBytes + kCreateClientReplyBytes;
    if (out_cap < reply_bytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the modem fifo has %u contiguous bytes "
            "free, and the create-client reply needs %u", out_cap, reply_bytes);
    }

    const uint32_t handle = ++next_client_handle_;
    const uint32_t results[kCreateClientResultWords] = {kNpaResult, kXdrTrue,
                                                        handle};
    const uint32_t written =
        WriteAcceptedReply(out_pa, self_pid, peer_pid, peer_cid, xid, results,
                           kCreateClientResultWords);
    return written + emu_.Get<Msm8255RpcRouterPeer>().AnswerConfirmRx(
                         in_pa, out_pa, out_cap, written);
}

uint32_t Msm8255NpaRemoteServer::WriteAcceptedReply(
    uint32_t out_pa, uint32_t self_pid, uint32_t peer_pid, uint32_t peer_cid,
    uint32_t xid, const uint32_t* results, uint32_t result_words) {
    auto& mem    = emu_.Get<EmulatedMemory>();
    auto& router = emu_.Get<Msm8255RpcRouterPeer>();

    const uint32_t body_bytes = kReplyResultsOff + 4u * result_words;
    router.WriteHeader(out_pa, kCtrlCmdData, self_pid, kNpaCid,
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

uint32_t Msm8255NpaRemoteServer::EmitCallback(uint32_t out_pa,
                                              uint32_t self_pid,
                                              uint32_t proc,
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
            "msm8255 npa remote server: the callback xid %u this peer issued for "
            "procedure %u is still unanswered, and more than one outstanding "
            "callback is not modeled", cb_xid_, cb_proc_);
    }

    cb_xid_         = ++next_xid_;
    cb_proc_        = proc;
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

    if (size < kPacmarkBytes + kReplyCommonBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the callback reply is %u bytes, which "
            "is short of the %u bytes every reply carries", size,
            kPacmarkBytes + kReplyCommonBytes);
    }

    router.ValidatePacmark(mem.ReadWord(in_pa + kHdrBytes),
                           size - kPacmarkBytes);

    const uint32_t body = in_pa + kHdrBytes + kPacmarkBytes;
    const uint32_t xid  = Be32(mem.ReadWord(body + kReplyXidOff));
    const uint32_t type = Be32(mem.ReadWord(body + kReplyTypeOff));
    const uint32_t stat = Be32(mem.ReadWord(body + kReplyStatOff));

    if (type != kOncrpcReply) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the guest sent rpc message type %u to "
            "the callback client, and only a reply is modeled", type);
    }
    if (stat != kMsgAccepted) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the callback reply carries reply_stat "
            "%u, and only an accepted reply is modeled", stat);
    }
    if (!cb_outstanding_) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the guest answered a callback that this "
            "peer never issued");
    }
    if (xid != cb_xid_) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the callback reply carries xid %u and "
            "the outstanding callback is xid %u", xid, cb_xid_);
    }
    if (size < kPacmarkBytes + kReplyAcceptedBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the accepted callback reply is %u "
            "bytes, which is short of the %u that carry its verifier and "
            "accept_stat", size, kPacmarkBytes + kReplyAcceptedBytes);
    }

    const uint32_t vfl  = Be32(mem.ReadWord(body + kReplyVerfFlavorOff));
    const uint32_t vlen = Be32(mem.ReadWord(body + kReplyVerfLenOff));
    const uint32_t acc  = Be32(mem.ReadWord(body + kReplyAcceptStatOff));
    if (vfl != kAuthNone || vlen != 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the callback reply verifier is flavor "
            "%u length %u, and only the null verifier is modeled", vfl, vlen);
    }
    if (acc != kAcceptSuccess) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the guest answered this peer's callback "
            "for procedure %u with accept_stat %u, and only success is modeled",
            cb_proc_, acc);
    }
    if (size != kPacmarkBytes + kCbReplyBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the successful callback reply is %u "
            "bytes, and only the %u-byte form is modeled", size,
            kPacmarkBytes + kCbReplyBytes);
    }

    const uint32_t rc = Be32(mem.ReadWord(body + kReplyResultsOff));
    if (rc != kNpaResult) {
        emu_.Get<Fatal>().Die(
            "msm8255 npa remote server: the guest's callback for procedure %u "
            "returned %u", cb_proc_, rc);
    }

    cb_outstanding_ = false;
    return router.AnswerConfirmRx(in_pa, out_pa, out_cap, 0u);
}

void Msm8255NpaRemoteServer::SaveState(StateWriter& w) {
    w.Write<uint32_t>(next_xid_);
    w.Write<uint32_t>(cb_xid_);
    w.Write<uint32_t>(cb_proc_);
    w.Write<uint32_t>(cb_outstanding_ ? 1u : 0u);
    w.Write<uint32_t>(next_client_handle_);
}

void Msm8255NpaRemoteServer::RestoreState(StateReader& r) {
    uint32_t outstanding = 0;
    r.Read(next_xid_);
    r.Read(cb_xid_);
    r.Read(cb_proc_);
    r.Read(outstanding);
    r.Read(next_client_handle_);
    cb_outstanding_ = outstanding != 0u;
}

REGISTER_SERVICE(Msm8255NpaRemoteServer);
