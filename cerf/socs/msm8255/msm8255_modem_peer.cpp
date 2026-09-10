#include "msm8255_modem_peer.h"

#include "msm8255_rpc_router_peer.h"
#include "msm8255_smem.h"

#include "../../boards/board_context.h"
#include "../../boot/guest_cold_boot.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../cpu/emulated_memory.h"

#include <cstdint>

namespace {

/* Linux arch/arm/mach-msm proc_comm.c: APP_COMMAND 0x00, MDM_STATUS 0x14. */
constexpr uint32_t kAppCommandOff = 0x00u;
constexpr uint32_t kMdmStatusOff  = 0x14u;

/* Linux arch/arm/mach-msm proc_comm.h: PCOM_READY. */
constexpr uint32_t kPcomReady = 1u;

/* Linux arch/arm/mach-msm smd_private.h: SMEM_SMSM_SHARED_STATE evaluated over
   its enum with SMEM_NUM_SMD_CHANNELS 64, and SMSM_STATE_MODEM from the
   non-MSM7X00A enum smsm_state_item. */
constexpr uint32_t kIdSmsmSharedState = 0x55u;
constexpr uint32_t kSmsmStateModem    = 1u;

/* Linux arch/arm/mach-msm smd_private.h SMSM_V1_SIZE, the eight-entry shared
   state that smd.c smd_core_init accepts. */
constexpr uint32_t kSmsmStateBytes = 32u;

/* Linux arch/arm/mach-msm smd.c publishes SMSM_INIT | SMSM_SMDINIT |
   SMSM_RPCINIT | SMSM_RUN for a processor that is up and running. */
constexpr uint32_t kSmsmModemUp = 0x129u;

/* Linux arch/arm/mach-msm smd_private.h msm_a2m_int writes 1 << irq to
   MSM_GCC_BASE + 0x8; proc_comm.c uses irq 6 and smd.c uses irqs 0 and 5. */
constexpr uint32_t kA2mSmdModem = 1u << 0;
constexpr uint32_t kA2mSmsm     = 1u << 5;
constexpr uint32_t kA2mProcComm = 1u << 6;

/* Linux arch/arm/mach-msm smd_private.h: SMEM_CHANNEL_ALLOC_TBL,
   SMEM_SMD_BASE_ID and SMEM_SMD_FIFO_BASE_ID evaluated over its enum with
   SMEM_NUM_SMD_CHANNELS 64, struct smd_alloc_elm, and SMD_CHANNELS. */
constexpr uint32_t kIdChannelAllocTbl = 13u;
constexpr uint32_t kIdSmdBase         = 14u;
constexpr uint32_t kIdSmdFifoBase     = 338u;

constexpr uint32_t kFifoWindowAlign = 0x1Fu;
constexpr uint32_t kFifoWindowMin   = 0x400u;
constexpr uint32_t kFifoWindowMax   = 0x10000u;
constexpr uint32_t kSmdChannels       = 64u;
constexpr uint32_t kAllocElmBytes     = 32u;
constexpr uint32_t kAllocElmNameBytes = 20u;
constexpr uint32_t kAllocElmCidOff    = 20u;
constexpr uint32_t kAllocElmCtypeOff  = 24u;
constexpr uint32_t kAllocElmRefOff    = 28u;

/* Linux arch/arm/mach-msm smd_private.h: struct smd_half_channel and
   struct smd_shared_v2, whose ch0 and ch1 are one half-channel apart. */
constexpr uint32_t kHalfChannelBytes = 20u;
constexpr uint32_t kSmdSharedBytes   = kHalfChannelBytes * 2u;
constexpr uint32_t kHcFDsrOff        = 4u;
constexpr uint32_t kHcFCtsOff        = 5u;
constexpr uint32_t kHcFCdOff         = 6u;
constexpr uint32_t kHcFHeadOff       = 8u;
constexpr uint32_t kHcFTailOff       = 9u;
constexpr uint32_t kHcFStateOff      = 10u;
constexpr uint32_t kHcTailOff        = 12u;
constexpr uint32_t kHcHeadOff        = 16u;

/* Linux arch/arm/mach-msm smd_private.h: SMD_SS_*, SMD_TYPE_MASK and
   SMD_TYPE_APPS_MODEM. */
constexpr uint32_t kSmdSsClosed       = 0u;
constexpr uint32_t kSmdSsOpening      = 1u;
constexpr uint32_t kSmdSsOpened       = 2u;
constexpr uint32_t kSmdTypeMask       = 0xFFu;
constexpr uint32_t kSmdTypeAppsModem  = 0x00u;

constexpr uint32_t kRpcRouterCid = 2u;

}

bool Msm8255ModemPeer::ShouldRegister() {
    auto* bd = emu_.TryGet<BoardContext>();
    return bd && bd->GetSoc() == SocFamily::MSM8255;
}

void Msm8255ModemPeer::OnReady() {
    SeedProcCommReady();
    emu_.Get<GuestColdBoot>().RegisterReplay([this] { SeedProcCommReady(); });
}

void Msm8255ModemPeer::RingDoorbell(uint32_t mask) {
    switch (mask) {
    case kA2mSmdModem: NotifySmd();         return;
    case kA2mSmsm:     PublishModemState(); return;
    case kA2mProcComm: RunProcComm();       return;
    default:
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: apps-to-modem doorbell mask 0x%03X is not "
            "modeled", mask);
    }
}

void Msm8255ModemPeer::SeedProcCommReady() {
    auto& smem = emu_.Get<Msm8255Smem>();
    emu_.Get<EmulatedMemory>().WriteWord(smem.SmemPa() + kMdmStatusOff,
                                         kPcomReady);
}

void Msm8255ModemPeer::PublishModemState() {
    auto& smem = emu_.Get<Msm8255Smem>();
    const uint32_t state = smem.ItemPa(kIdSmsmSharedState, kSmsmStateBytes);
    if (state == 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: the guest rang the SMSM doorbell before "
            "allocating smem item %u", kIdSmsmSharedState);
    }
    emu_.Get<EmulatedMemory>().WriteWord(state + 4u * kSmsmStateModem,
                                         kSmsmModemUp);
}

void Msm8255ModemPeer::NotifySmd() {
    auto& smem = emu_.Get<Msm8255Smem>();
    auto& mem  = emu_.Get<EmulatedMemory>();
    const uint32_t tbl =
        smem.ItemPa(kIdChannelAllocTbl, kAllocElmBytes * kSmdChannels);
    if (tbl == 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: the guest rang the SMD doorbell before "
            "allocating smem item %u", kIdChannelAllocTbl);
    }

    for (uint32_t n = 0; n < kSmdChannels; ++n) {
        const uint32_t rec = tbl + kAllocElmBytes * n;
        if (mem.ReadWord(rec + kAllocElmRefOff) == 0u) continue;
        if (mem.ReadByte(rec) == 0u) continue;

        const uint32_t ctype = mem.ReadWord(rec + kAllocElmCtypeOff);
        if ((ctype & kSmdTypeMask) != kSmdTypeAppsModem) continue;

        const uint32_t cid = mem.ReadWord(rec + kAllocElmCidOff);
        if (cid >= kSmdChannels) {
            emu_.Get<Fatal>().Die(
                "msm8255 modem peer: smd channel %u names cid %u, which is "
                "outside the %u-channel table", n, cid, kSmdChannels);
        }

        const uint32_t item = smem.ItemPa(kIdSmdBase + cid, kSmdSharedBytes);
        if (item == 0u) {
            emu_.Get<Fatal>().Die(
                "msm8255 modem peer: smd channel %u names cid %u, whose smem "
                "item %u is not allocated", n, cid, kIdSmdBase + cid);
        }

        ServiceSmdChannel(cid, rec, item);
    }
}

void Msm8255ModemPeer::ServiceSmdChannel(uint32_t cid, uint32_t rec,
                                         uint32_t item) {
    auto& mem = emu_.Get<EmulatedMemory>();
    const uint32_t apps_half  = item;
    const uint32_t modem_half = item + kHalfChannelBytes;

    const uint32_t apps_state = mem.ReadWord(apps_half);
    if (apps_state == kSmdSsClosed) return;
    if (apps_state != kSmdSsOpening && apps_state != kSmdSsOpened) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: apps smd half-channel state %u is not modeled",
            apps_state);
    }

    if (apps_state == kSmdSsOpened &&
        mem.ReadWord(modem_half) == kSmdSsOpened) {
        ConsumeAppsSmdFlags(cid, rec, apps_half);
    }

    if (apps_state == kSmdSsOpening) {
        mem.WriteWord(apps_half + kHcTailOff, 0u);
    }

    if (mem.ReadWord(modem_half) != kSmdSsOpened) {
        OpenModemSmdHalf(modem_half);
    }
}

void Msm8255ModemPeer::ConsumeAppsSmdFlags(uint32_t cid, uint32_t rec,
                                           uint32_t apps_half_pa) {
    auto& mem = emu_.Get<EmulatedMemory>();
    if (mem.ReadByte(apps_half_pa + kHcFHeadOff) != 0u) {
        mem.WriteByte(apps_half_pa + kHcFHeadOff, 0u);
    }
    if (mem.ReadByte(apps_half_pa + kHcFTailOff) != 0u) {
        mem.WriteByte(apps_half_pa + kHcFTailOff, 0u);
    }
    if (mem.ReadByte(apps_half_pa + kHcFStateOff) != 0u) {
        mem.WriteByte(apps_half_pa + kHcFStateOff, 0u);
    }
    if (mem.ReadWord(apps_half_pa + kHcHeadOff) !=
        mem.ReadWord(apps_half_pa + kHcTailOff)) {
        ServiceSmdData(cid, rec, apps_half_pa);
    }
}

void Msm8255ModemPeer::HaltUnroutedSmdChannel(uint32_t cid, uint32_t rec) {
    auto& mem = emu_.Get<EmulatedMemory>();

    char name[kAllocElmNameBytes + 1] = {};
    for (uint32_t i = 0; i < kAllocElmNameBytes; ++i) {
        name[i] = static_cast<char>(mem.ReadByte(rec + i));
    }

    emu_.Get<Fatal>().Die(
        "msm8255 modem peer: smd channel %u \"%s\" carries data, and only the "
        "rpc router channel %u is modeled", cid, name, kRpcRouterCid);
}

void Msm8255ModemPeer::ServiceSmdData(uint32_t cid, uint32_t rec,
                                      uint32_t apps_half_pa) {
    if (cid != kRpcRouterCid) HaltUnroutedSmdChannel(cid, rec);

    auto& mem = emu_.Get<EmulatedMemory>();
    uint32_t fifo_pa    = 0u;
    uint32_t fifo_bytes = 0u;
    if (!emu_.Get<Msm8255Smem>().ItemPaAndSize(kIdSmdFifoBase + cid, fifo_pa,
                                               fifo_bytes)) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: smd channel %u carries data and its fifo smem "
            "item %u is not allocated", cid, kIdSmdFifoBase + cid);
    }
    const uint32_t half = fifo_bytes / 2u;
    if ((fifo_bytes & 1u) != 0u || (half & kFifoWindowAlign) != 0u ||
        half < kFifoWindowMin || half > kFifoWindowMax) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: smd fifo smem item %u is %u bytes, so each "
            "direction gets %u, which the channel binding rejects",
            kIdSmdFifoBase + cid, fifo_bytes, half);
    }
    const uint32_t head = mem.ReadWord(apps_half_pa + kHcHeadOff);
    const uint32_t tail = mem.ReadWord(apps_half_pa + kHcTailOff);
    if (head >= half || tail >= half) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: smd fifo indices (head=%u tail=%u) leave the "
            "%u-byte half the channel binding gives each direction",
            head, tail, half);
    }
    if (tail > head) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: the guest's smd fifo wrapped (head=%u tail=%u) "
            "and the wrapped read is not modeled", head, tail);
    }

    const uint32_t modem_half = apps_half_pa + kHalfChannelBytes;
    uint32_t out_head = mem.ReadWord(modem_half + kHcHeadOff);
    if (out_head >= half) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: the modem write index %u leaves the %u-byte "
            "half the channel binding gives each direction", out_head, half);
    }
    uint32_t cursor   = tail;
    uint32_t produced = 0u;
    while (cursor < head) {
        uint32_t consumed = 0u;
        const uint32_t sent = emu_.Get<Msm8255RpcRouterPeer>().Answer(
            fifo_pa + cursor, head - cursor, fifo_pa + half + out_head,
            half - out_head, consumed);
        cursor   += consumed;
        out_head += sent;
        produced += sent;
    }

    mem.WriteWord(apps_half_pa + kHcTailOff, cursor);
    mem.WriteByte(modem_half + kHcFTailOff, 1u);
    if (produced != 0u) {
        mem.WriteWord(modem_half + kHcHeadOff, out_head % half);
        mem.WriteByte(modem_half + kHcFHeadOff, 1u);
    }
}

void Msm8255ModemPeer::OpenModemSmdHalf(uint32_t modem_half_pa) {
    auto& mem = emu_.Get<EmulatedMemory>();
    mem.WriteByte(modem_half_pa + kHcFDsrOff, 1u);
    mem.WriteByte(modem_half_pa + kHcFCtsOff, 1u);
    mem.WriteByte(modem_half_pa + kHcFCdOff, 1u);
    mem.WriteWord(modem_half_pa, kSmdSsOpened);
    mem.WriteByte(modem_half_pa + kHcFStateOff, 1u);
}

void Msm8255ModemPeer::RunProcComm() {
    auto& smem = emu_.Get<Msm8255Smem>();
    auto& mem  = emu_.Get<EmulatedMemory>();
    const uint32_t cmd = mem.ReadWord(smem.SmemPa() + kAppCommandOff);
    emu_.Get<Fatal>().Die(
        "msm8255 modem peer: proc_comm command %u is not modeled", cmd);
}

REGISTER_SERVICE(Msm8255ModemPeer);
