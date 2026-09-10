#include "../../peripherals/peripheral_base.h"

#include "../../boards/board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../peripherals/peripheral_dispatcher.h"

#include <cstdint>

namespace {

/* Linux arch/arm/mach-msm msm_iomap-7x30.h: MSM7X30_GPIO1_PHYS 0xAC001000. */
constexpr uint32_t kTlmmBase = 0xAC001000u;
constexpr uint32_t kTlmmSize = 0x00002000u;

/* Linux arch/arm/mach-msm board-semc_zeus.c carries this block's revision
   register as hw_revision_addr 0xac001270. */
constexpr uint32_t kHwRevisionNumber = 0x270u;

constexpr uint32_t kRevisionShift = 28u;
constexpr uint32_t kPartNumShift  = 12u;

constexpr uint32_t kRevision = 1u;
constexpr uint32_t kPartNum  = 0x570u;

class Msm8255Tlmm : public Peripheral {
public:
    using Peripheral::Peripheral;

    bool ShouldRegister() override {
        return emu_.Get<BoardContext>().GetSoc() == SocFamily::MSM8255;
    }

    void OnReady() override {
        emu_.Get<PeripheralDispatcher>().Register(this);
    }

    uint32_t MmioBase() const override { return kTlmmBase; }
    uint32_t MmioSize() const override { return kTlmmSize; }

    uint32_t ReadWord(uint32_t addr) override {
        const uint32_t off = addr - MmioBase();
        if (off == kHwRevisionNumber) {
            return (kRevision << kRevisionShift) | (kPartNum << kPartNumShift);
        }
        HaltUnsupportedAccess("ReadWord", addr, 0u);
    }

    void WriteWord(uint32_t addr, uint32_t value) override {
        HaltUnsupportedAccess("WriteWord", addr, value);
    }
};

}

REGISTER_SERVICE(Msm8255Tlmm);
