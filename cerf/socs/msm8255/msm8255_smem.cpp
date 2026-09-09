#include "msm8255_smem.h"

#include "../../boards/board_context.h"
#include "../../boot/guest_cold_boot.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../cpu/arm_processor_config.h"
#include "../../cpu/emulated_memory.h"

#include <cstdint>

namespace {

/* Linux arch/arm/mach-msm msm_iomap-7x30.h:
   MSM_SHARED_RAM_PHYS 0x00100000, MSM_SHARED_RAM_SIZE SZ_1M. */
constexpr uint32_t kSmemPa   = 0x00100000u;
constexpr uint32_t kSmemSize = 0x00100000u;

constexpr uint32_t kProcCommBytes = 4u * 16u;
constexpr uint32_t kVersionBytes  = 32u * 4u;
constexpr uint32_t kHeapInfoBytes = 4u * 4u;
constexpr uint32_t kHeapTocEntries = 512u;
constexpr uint32_t kHeapTocBytes   = kHeapTocEntries * 16u;
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

constexpr uint32_t kTocAllocatedOff = 0u;
constexpr uint32_t kTocOffsetOff    = 4u;
constexpr uint32_t kTocSizeOff      = 8u;
constexpr uint32_t kTocAllocated    = 1u;

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

constexpr uint32_t kVddMv = 1250u;

/* Linux arch/arm/mach-msm avs.c AVSDSCR_INPUT, written by avs_reset_delays;
   avs_hw.S encodes that register as mcr p15, 7, Rd, c15, c0, 6. */
constexpr uint32_t kAvsdscr = 0x01004860u;

/* Linux arch/arm/mach-msm avs_hw.S avs_reset_delays: AVSCSR 0x61 enables the
   CPU, V and L2 AVS modules, encoded as mcr p15, 7, Rd, c15, c1, 7. */
constexpr uint32_t kBspAvscsrOff = 15392u;
constexpr uint32_t kAvscsr       = 0x61u;

constexpr uint32_t kBspSawCfgOff = 15380u;
constexpr uint32_t kSawCfgSeed   = 6u;

constexpr uint32_t Align8(uint32_t v) { return (v + 7u) & ~7u; }

constexpr uint32_t kBspOff = Align8(kFixedAreaEnd);
constexpr uint32_t kSrcOff = Align8(kBspOff + kBspBytes);
constexpr uint32_t kHeapUsedEnd = Align8(kSrcOff + kSrcBytes);

}

bool Msm8255Smem::ShouldRegister() {
    auto* bd = emu_.TryGet<BoardContext>();
    return bd && bd->GetSoc() == SocFamily::MSM8255;
}

void Msm8255Smem::OnReady() {
    Seed();
    emu_.Get<GuestColdBoot>().RegisterReplay([this] { Seed(); });
}

uint32_t Msm8255Smem::SmemPa() { return kSmemPa; }

/* Linux arch/arm/mach-msm smd.c smem_find: the item pointer is handed out only
   when the caller's byte count, rounded up to 8, equals the allocated size. */
uint32_t Msm8255Smem::ItemPa(uint32_t id, uint32_t bytes) {
    uint32_t off  = 0u;
    uint32_t size = 0u;
    if (!ReadTocEntry(id, off, size)) {
        return 0u;
    }
    const uint32_t want = Align8(bytes);
    if (size != want) {
        emu_.Get<Fatal>().Die(
            "msm8255 smem: item %u holds %u bytes, and the modem peer models it "
            "as %u", id, size, want);
    }
    return kSmemPa + off;
}

/* Linux arch/arm/mach-msm smd.c smem_item: the caller receives the item's own
   recorded size, in place of declaring the extent it expects. */
bool Msm8255Smem::ItemPaAndSize(uint32_t id, uint32_t& pa, uint32_t& bytes) {
    uint32_t off  = 0u;
    uint32_t size = 0u;
    if (!ReadTocEntry(id, off, size)) {
        return false;
    }
    pa    = kSmemPa + off;
    bytes = size;
    return true;
}

bool Msm8255Smem::ReadTocEntry(uint32_t id, uint32_t& off, uint32_t& size) {
    auto& mem = emu_.Get<EmulatedMemory>();
    const uint32_t toc = TocEntryPa(id);
    if (mem.ReadWord(toc + kTocAllocatedOff) != kTocAllocated) {
        return false;
    }
    off  = mem.ReadWord(toc + kTocOffsetOff);
    size = mem.ReadWord(toc + kTocSizeOff);
    if (off >= kSmemSize || size > kSmemSize - off) {
        emu_.Get<Fatal>().Die(
            "msm8255 smem: item %u claims offset 0x%X and size %u, which leaves "
            "the %u-byte shared window", id, off, size, kSmemSize);
    }
    return true;
}

uint32_t Msm8255Smem::TocEntryPa(uint32_t id) {
    if (id >= kHeapTocEntries) {
        emu_.Get<Fatal>().Die(
            "msm8255 smem: item id %u is outside the %u-entry heap toc",
            id, kHeapTocEntries);
    }
    return kSmemPa + kHeapTocOff + kTocEntryBytes * id;
}

void Msm8255Smem::Seed() {
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

uint32_t Msm8255Smem::RecordPa(uint32_t index) const {
    return kSmemPa + kBspOff + kBspRecordsOff + kBspRecordBytes * index;
}

void Msm8255Smem::SeedSpeedRecord() {
    auto& mem = emu_.Get<EmulatedMemory>();
    const uint32_t rec = RecordPa(kBspRecordIndex);
    mem.WriteWord(rec + 0u, emu_.Get<ArmProcessorConfig>().CpuClockHz());
    mem.WriteWord(rec + kRecKeySrcOff,     kRecKeySrc);
    mem.WriteWord(rec + kRecKeyLevelOff,   kRecKeyLevel);
    mem.WriteWord(rec + kRecVddMvOff,      kVddMv);
    mem.WriteWord(rec + kRecAvsdscrOff,    kAvsdscr);
}

void Msm8255Smem::SeedPerfLevels() {
    auto& mem = emu_.Get<EmulatedMemory>();
    const uint32_t base = kSmemPa + kBspOff + kBspPerfLevelsOff;
    for (uint32_t i = 0; i < kBspPerfLevelCount; ++i) {
        mem.WriteWord(base + kBspPerfLevelStride * i, kBspRecordIndex);
    }
}

void Msm8255Smem::SeedAvsConfig() {
    auto& mem = emu_.Get<EmulatedMemory>();
    mem.WriteWord(kSmemPa + kBspOff + kBspAvscsrOff, kAvscsr);
    mem.WriteWord(kSmemPa + kBspOff + kBspSawCfgOff, kSawCfgSeed);
}

void Msm8255Smem::PublishItem(uint32_t id, uint32_t off, uint32_t size,
                              uint32_t magic) {
    auto& mem = emu_.Get<EmulatedMemory>();
    const uint32_t toc = TocEntryPa(id);
    mem.WriteWord(toc +  0u, 1u);
    mem.WriteWord(toc +  4u, off);
    mem.WriteWord(toc +  8u, size);
    mem.WriteWord(toc + 12u, 0u);
    mem.WriteWord(kSmemPa + off, magic);
}

REGISTER_SERVICE(Msm8255Smem);
