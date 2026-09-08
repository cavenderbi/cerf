#include "../../boards/board_context.h"
#include "../../boot/guest_cold_boot.h"
#include "../../core/cerf_emulator.h"
#include "../../core/service.h"
#include "../../cpu/emulated_memory.h"

#include <cstdint>

namespace {

/* Linux arch/arm/mach-msm msm_iomap-7x30.h:
   MSM_SHARED_RAM_PHYS 0x00100000, MSM_SHARED_RAM_SIZE SZ_1M. */
constexpr uint32_t kSmemPa   = 0x00100000u;
constexpr uint32_t kSmemSize = 0x00100000u;

/* Linux arch/arm/mach-msm smd_private.h, struct smem_shared:
   struct smem_proc_comm proc_comm[4]; unsigned version[32];
   struct smem_heap_info heap_info; struct smem_heap_entry heap_toc[512].
   struct smem_proc_comm and struct smem_heap_entry are four words each. */
constexpr uint32_t kProcCommBytes = 4u * 16u;
constexpr uint32_t kVersionBytes  = 32u * 4u;
constexpr uint32_t kHeapInfoBytes = 4u * 4u;
constexpr uint32_t kHeapTocBytes  = 512u * 16u;
constexpr uint32_t kHeapInfoOff   = kProcCommBytes + kVersionBytes;
constexpr uint32_t kSharedBytes =
    kHeapInfoOff + kHeapInfoBytes + kHeapTocBytes;

/* Linux arch/arm/mach-msm smd_private.h, struct smem_heap_info:
   unsigned initialized; unsigned free_offset; unsigned heap_remaining;
   unsigned reserved. */
constexpr uint32_t kHeapInitializedOff  = kHeapInfoOff + 0u;
constexpr uint32_t kHeapFreeOffsetOff   = kHeapInfoOff + 4u;
constexpr uint32_t kHeapRemainingOff    = kHeapInfoOff + 8u;
constexpr uint32_t kHeapReservedOff     = kHeapInfoOff + 12u;

constexpr uint32_t kFixedTailBytes = 0xF8u;
constexpr uint32_t kFixedAreaEnd   = kSharedBytes + kFixedTailBytes;

constexpr uint32_t kHeapInitialized = 1u;

class Msm8255Smem : public Service {
public:
    using Service::Service;

    bool ShouldRegister() override {
        auto* bd = emu_.TryGet<BoardContext>();
        return bd && bd->GetSoc() == SocFamily::MSM8255;
    }

    void OnReady() override {
        Seed();
        emu_.Get<GuestColdBoot>().RegisterReplay([this] { Seed(); });
    }

private:
    void Seed() {
        auto& mem = emu_.Get<EmulatedMemory>();
        mem.WriteWord(kSmemPa + kHeapInitializedOff, kHeapInitialized);
        mem.WriteWord(kSmemPa + kHeapFreeOffsetOff,  kFixedAreaEnd);
        mem.WriteWord(kSmemPa + kHeapRemainingOff,   kSmemSize - kFixedAreaEnd);
        mem.WriteWord(kSmemPa + kHeapReservedOff,    0u);
    }
};

}

REGISTER_SERVICE(Msm8255Smem);
