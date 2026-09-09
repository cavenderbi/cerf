#include "msm8255_rpc_router_peer.h"

#include "../../boards/board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../cpu/emulated_memory.h"

#include <cstdint>

namespace {

/* Linux arch/arm/mach-msm smd_rpcrouter.h: struct rr_header. */
constexpr uint32_t kHdrVersionOff   = 0u;
constexpr uint32_t kHdrTypeOff      = 4u;
constexpr uint32_t kHdrSrcPidOff    = 8u;
constexpr uint32_t kHdrSrcCidOff    = 12u;
constexpr uint32_t kHdrConfirmRxOff = 16u;
constexpr uint32_t kHdrSizeOff      = 20u;
constexpr uint32_t kHdrDstPidOff    = 24u;
constexpr uint32_t kHdrDstCidOff    = 28u;
constexpr uint32_t kHdrBytes        = 32u;

/* Linux arch/arm/mach-msm smd_rpcrouter.h: union rr_control_msg, whose widest
   arm is the five-word srv form. */
constexpr uint32_t kCtrlMsgBytes = 20u;
constexpr uint32_t kSrvProgOff   = 4u;
constexpr uint32_t kSrvVersOff   = 8u;
constexpr uint32_t kSrvPidOff    = 12u;
constexpr uint32_t kSrvCidOff    = 16u;

constexpr uint32_t kNpaRemoteProg = 0x300000A4u;
constexpr uint32_t kNpaRemoteVers = 0x00010001u;
constexpr uint32_t kNpaRemoteCid  = 1u;

/* Linux arch/arm/mach-msm smd_rpcrouter.h: RPCROUTER_VERSION,
   RPCROUTER_ROUTER_ADDRESS, RPCROUTER_CTRL_CMD_HELLO and
   RPCROUTER_CTRL_CMD_NEW_SERVER. */
constexpr uint32_t kRouterVersion   = 1u;
constexpr uint32_t kRouterAddress   = 0xFFFFFFFEu;
constexpr uint32_t kCtrlCmdHello    = 2u;
constexpr uint32_t kCtrlCmdNewServer = 4u;

}

bool Msm8255RpcRouterPeer::ShouldRegister() {
    auto* bd = emu_.TryGet<BoardContext>();
    return bd && bd->GetSoc() == SocFamily::MSM8255;
}

uint32_t Msm8255RpcRouterPeer::Answer(uint32_t in_pa, uint32_t in_avail,
                                      uint32_t out_pa, uint32_t out_cap,
                                      uint32_t& consumed) {
    auto& mem = emu_.Get<EmulatedMemory>();

    if (in_avail < kHdrBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: %u bytes are queued, which is short of "
            "the %u-byte router header", in_avail, kHdrBytes);
    }

    const uint32_t size = mem.ReadWord(in_pa + kHdrSizeOff);
    if (size != kCtrlMsgBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: message payload is %u bytes, and only the "
            "%u-byte control message is modeled", size, kCtrlMsgBytes);
    }
    if (in_avail < kHdrBytes + size) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: the header declares %u payload bytes and "
            "only %u are queued", size, in_avail - kHdrBytes);
    }
    consumed = kHdrBytes + size;

    const uint32_t version = mem.ReadWord(in_pa + kHdrVersionOff);
    if (version != kRouterVersion) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: router version %u is not modeled",
            version);
    }

    const uint32_t dst_cid = mem.ReadWord(in_pa + kHdrDstCidOff);
    if (dst_cid != kRouterAddress) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: the guest addressed cid 0x%08X, and only "
            "router-addressed control messages are modeled", dst_cid);
    }

    const uint32_t confirm_rx = mem.ReadWord(in_pa + kHdrConfirmRxOff);
    if (confirm_rx != 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: the guest asked for receive confirmation "
            "and the resume-tx path is not modeled");
    }

    const uint32_t type = mem.ReadWord(in_pa + kHdrTypeOff);
    const uint32_t cmd  = mem.ReadWord(in_pa + kHdrBytes);
    if (type != cmd) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: header type %u disagrees with payload "
            "command %u", type, cmd);
    }

    if (type == kCtrlCmdNewServer) {
        const uint32_t vers = mem.ReadWord(in_pa + kHdrBytes + kSrvVersOff);
        if (vers == 0u) {
            emu_.Get<Fatal>().Die(
                "msm8255 rpc router peer: the guest announced a server with "
                "version 0, which the router model rejects");
        }
        return 0u;
    }

    if (type != kCtrlCmdHello) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: router control command %u is not modeled",
            type);
    }

    const uint32_t msg_bytes = kHdrBytes + kCtrlMsgBytes;
    if (out_cap < 2u * msg_bytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: the modem fifo has %u contiguous bytes "
            "free, and the hello reply plus server announcement need %u",
            out_cap, 2u * msg_bytes);
    }

    const uint32_t src_pid = mem.ReadWord(in_pa + kHdrSrcPidOff);
    const uint32_t dst_pid = mem.ReadWord(in_pa + kHdrDstPidOff);

    /* Linux arch/arm/mach-msm smd_rpcrouter.c: the hello arm answers with a
       hello of its own, then announces one new-server message per server the
       answering processor hosts. */
    WriteCtrlMsg(out_pa, kCtrlCmdHello, dst_pid, src_pid, 0u, 0u, 0u, 0u);
    WriteCtrlMsg(out_pa + msg_bytes, kCtrlCmdNewServer, dst_pid, src_pid,
                 kNpaRemoteProg, kNpaRemoteVers, dst_pid, kNpaRemoteCid);
    return 2u * msg_bytes;
}

void Msm8255RpcRouterPeer::WriteCtrlMsg(uint32_t out_pa, uint32_t cmd,
                                        uint32_t self_pid, uint32_t peer_pid,
                                        uint32_t prog, uint32_t vers,
                                        uint32_t srv_pid, uint32_t srv_cid) {
    auto& mem = emu_.Get<EmulatedMemory>();

    mem.WriteWord(out_pa + kHdrVersionOff,   kRouterVersion);
    mem.WriteWord(out_pa + kHdrTypeOff,      cmd);
    mem.WriteWord(out_pa + kHdrSrcPidOff,    self_pid);
    mem.WriteWord(out_pa + kHdrSrcCidOff,    kRouterAddress);
    mem.WriteWord(out_pa + kHdrConfirmRxOff, 0u);
    mem.WriteWord(out_pa + kHdrSizeOff,      kCtrlMsgBytes);
    mem.WriteWord(out_pa + kHdrDstPidOff,    peer_pid);
    mem.WriteWord(out_pa + kHdrDstCidOff,    kRouterAddress);

    mem.WriteWord(out_pa + kHdrBytes + 0u,           cmd);
    mem.WriteWord(out_pa + kHdrBytes + kSrvProgOff,  prog);
    mem.WriteWord(out_pa + kHdrBytes + kSrvVersOff,  vers);
    mem.WriteWord(out_pa + kHdrBytes + kSrvPidOff,   srv_pid);
    mem.WriteWord(out_pa + kHdrBytes + kSrvCidOff,   srv_cid);
}

REGISTER_SERVICE(Msm8255RpcRouterPeer);
