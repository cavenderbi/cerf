#include "../../boards/board_context.h"
#include "../../boot/guest_cold_boot.h"
#include "../../core/cerf_emulator.h"
#include "../../core/service.h"
#include "../../cpu/arm_processor_config.h"
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

constexpr uint32_t kHeapTocOff    = kHeapInfoOff + kHeapInfoBytes;
constexpr uint32_t kTocEntryBytes = 16u;

/* Linux arch/arm/mach-msm smd_private.h: SMEM_CLKREGIM_BSP and
   SMEM_CLKREGIM_SOURCES, evaluated over that enum with
   SMEM_NUM_SMD_CHANNELS 64. */
constexpr uint32_t kIdClkregimBsp     = 336u;
constexpr uint32_t kIdClkregimSources = 337u;

constexpr uint32_t kBspBytes  = 20456u;
constexpr uint32_t kSrcBytes  = 208u;
constexpr uint32_t kBspMagic  = 0xCCEE0003u;
constexpr uint32_t kSrcMagic  = 0xCCEE0002u;

constexpr uint32_t kBspRecordsOff  = 13168u;
constexpr uint32_t kBspRecordBytes = 52u;
constexpr uint32_t kBspRecordIndex = 1u;

constexpr uint32_t kRecKeySrcOff   = 4u;
constexpr uint32_t kRecKeyLevelOff = 8u;

constexpr uint32_t kRecKeySrc   = 9u;
constexpr uint32_t kRecKeyLevel = 1u;

constexpr uint32_t kRecVddMvOff    = 40u;
constexpr uint32_t kRecAvsdscrOff  = 48u;

constexpr uint32_t kBspPerfLevelsOff   = 13104u;
constexpr uint32_t kBspPerfLevelCount  = 8u;
constexpr uint32_t kBspPerfLevelStride = 8u;

/* Linux arch/arm/mach-msm acpuclock-7x30.c: VDD_RAW(mv) is
   ((mv / V_STEP) - 30) | VREG_DATA with V_STEP 25 and VREG_DATA
   VREG_CONFIG | (VREF_SEL << 5) = 0xE0, so vdd_mv 1250 pairs with the
   acpu_freq_tbl row acpu_clk_khz 1401600. */
constexpr uint32_t kVddMv = 1250u;

/* Linux arch/arm/mach-msm avs.c AVSDSCR_INPUT, written by avs_reset_delays;
   avs_hw.S encodes that register as mcr p15, 7, Rd, c15, c0, 6. */
constexpr uint32_t kAvsdscr = 0x01004860u;

/* Linux arch/arm/mach-msm avs_hw.S avs_reset_delays: AVSCSR 0x61 enables the
   CPU, V and L2 AVS modules, encoded as mcr p15, 7, Rd, c15, c1, 7. */
constexpr uint32_t kBspAvscsrOff = 15392u;
constexpr uint32_t kAvscsr       = 0x61u;

/* Linux arch/arm/mach-msm board-semc_zeus.c msm_spm_platform_data sets
   MSM_SPM_REG_SAW_CFG to 0x05 on this SoC, and spm.c maps that register at
   SAW+0x10. */
constexpr uint32_t kBspSawCfgOff = 15380u;
constexpr uint32_t kSawCfgSeed   = 6u;

constexpr uint32_t Align8(uint32_t v) { return (v + 7u) & ~7u; }

constexpr uint32_t kBspOff = Align8(kFixedAreaEnd);
constexpr uint32_t kSrcOff = Align8(kBspOff + kBspBytes);
constexpr uint32_t kHeapUsedEnd = Align8(kSrcOff + kSrcBytes);

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
        mem.WriteWord(kSmemPa + kHeapFreeOffsetOff,  kHeapUsedEnd);
        mem.WriteWord(kSmemPa + kHeapRemainingOff,   kSmemSize - kHeapUsedEnd);
        mem.WriteWord(kSmemPa + kHeapReservedOff,    0u);

        PublishItem(kIdClkregimBsp,     kBspOff, kBspBytes, kBspMagic);
        PublishItem(kIdClkregimSources, kSrcOff, kSrcBytes, kSrcMagic);

        SeedSpeedRecord();
        SeedPerfLevels();
        SeedAvsConfig();
    }

    uint32_t RecordPa(uint32_t index) const {
        return kSmemPa + kBspOff + kBspRecordsOff + kBspRecordBytes * index;
    }

    void SeedSpeedRecord() {
        auto& mem = emu_.Get<EmulatedMemory>();
        const uint32_t rec = RecordPa(kBspRecordIndex);
        mem.WriteWord(rec + 0u,                emu_.Get<ArmProcessorConfig>().CpuClockHz());
        mem.WriteWord(rec + kRecKeySrcOff,     kRecKeySrc);
        mem.WriteWord(rec + kRecKeyLevelOff,   kRecKeyLevel);
        mem.WriteWord(rec + kRecVddMvOff,      kVddMv);
        mem.WriteWord(rec + kRecAvsdscrOff,    kAvsdscr);
    }

    void SeedPerfLevels() {
        auto& mem = emu_.Get<EmulatedMemory>();
        const uint32_t base = kSmemPa + kBspOff + kBspPerfLevelsOff;
        for (uint32_t i = 0; i < kBspPerfLevelCount; ++i) {
            mem.WriteWord(base + kBspPerfLevelStride * i, kBspRecordIndex);
        }
    }

    void SeedAvsConfig() {
        auto& mem = emu_.Get<EmulatedMemory>();
        mem.WriteWord(kSmemPa + kBspOff + kBspAvscsrOff, kAvscsr);
        mem.WriteWord(kSmemPa + kBspOff + kBspSawCfgOff, kSawCfgSeed);
    }

    void PublishItem(uint32_t id, uint32_t off, uint32_t size,
                     uint32_t magic) {
        auto& mem = emu_.Get<EmulatedMemory>();
        const uint32_t toc = kSmemPa + kHeapTocOff + kTocEntryBytes * id;
        mem.WriteWord(toc +  0u, 1u);
        mem.WriteWord(toc +  4u, off);
        mem.WriteWord(toc +  8u, size);
        mem.WriteWord(toc + 12u, 0u);
        mem.WriteWord(kSmemPa + off, magic);
    }
};

}

REGISTER_SERVICE(Msm8255Smem);
