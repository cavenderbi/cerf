#include "msm8255_modem_peer.h"

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

/* Linux arch/arm/mach-msm smd.c publishes SMSM_INIT | SMSM_SMDINIT |
   SMSM_RPCINIT | SMSM_RUN for a processor that is up and running. */
constexpr uint32_t kSmsmModemUp = 0x129u;

/* Linux arch/arm/mach-msm smd_private.h msm_a2m_int writes 1 << irq to
   MSM_GCC_BASE + 0x8; proc_comm.c uses irq 6 and smd.c uses irqs 0 and 5. */
constexpr uint32_t kA2mSmsm     = 1u << 5;
constexpr uint32_t kA2mProcComm = 1u << 6;

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
    const uint32_t state = smem.DynamicItemPa(kIdSmsmSharedState);
    if (state == 0u) {
        emu_.Get<Fatal>().Die(
            "msm8255 modem peer: the guest rang the SMSM doorbell before "
            "allocating smem item %u", kIdSmsmSharedState);
    }
    emu_.Get<EmulatedMemory>().WriteWord(state + 4u * kSmsmStateModem,
                                         kSmsmModemUp);
}

void Msm8255ModemPeer::RunProcComm() {
    auto& smem = emu_.Get<Msm8255Smem>();
    auto& mem  = emu_.Get<EmulatedMemory>();
    const uint32_t cmd = mem.ReadWord(smem.SmemPa() + kAppCommandOff);
    emu_.Get<Fatal>().Die(
        "msm8255 modem peer: proc_comm command %u is not modeled", cmd);
}

REGISTER_SERVICE(Msm8255ModemPeer);
