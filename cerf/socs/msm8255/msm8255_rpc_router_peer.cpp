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
constexpr uint32_t kSrvVersOff   = 8u;

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

    if (out_cap < kHdrBytes + kCtrlMsgBytes) {
        emu_.Get<Fatal>().Die(
            "msm8255 rpc router peer: the modem fifo has %u contiguous bytes "
            "free, and the reply needs %u",
            out_cap, kHdrBytes + kCtrlMsgBytes);
    }

    const uint32_t src_pid = mem.ReadWord(in_pa + kHdrSrcPidOff);
    const uint32_t dst_pid = mem.ReadWord(in_pa + kHdrDstPidOff);

    mem.WriteWord(out_pa + kHdrVersionOff,   kRouterVersion);
    mem.WriteWord(out_pa + kHdrTypeOff,      kCtrlCmdHello);
    mem.WriteWord(out_pa + kHdrSrcPidOff,    dst_pid);
    mem.WriteWord(out_pa + kHdrSrcCidOff,    kRouterAddress);
    mem.WriteWord(out_pa + kHdrConfirmRxOff, 0u);
    mem.WriteWord(out_pa + kHdrSizeOff,      kCtrlMsgBytes);
    mem.WriteWord(out_pa + kHdrDstPidOff,    src_pid);
    mem.WriteWord(out_pa + kHdrDstCidOff,    kRouterAddress);

    mem.WriteWord(out_pa + kHdrBytes, kCtrlCmdHello);
    for (uint32_t i = 4u; i < kCtrlMsgBytes; i += 4u) {
        mem.WriteWord(out_pa + kHdrBytes + i, 0u);
    }
    return kHdrBytes + kCtrlMsgBytes;
}

REGISTER_SERVICE(Msm8255RpcRouterPeer);
