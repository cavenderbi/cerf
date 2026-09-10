#include "msm8255_gpio_window_impl.h"

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

/* Linux arch/arm/mach-msm gpio_hw.h under CONFIG_ARCH_MSM7X30: MSM_GPIO_OUT_0
   and _2 through _7 with their MSM_GPIO_OE_ partners, reached through an
   unbiased MSM_GPIO1_REG; each mask is that bank's annotated pin range. */
constexpr Msm8255GpioBank kBanks[] = {
    {0x000u, 0x010u, 0x0000FFFFu},
    {0x004u, 0x014u, 0x00FFFFFFu},
    {0x008u, 0x018u, 0x07FFFFFFu},
    {0x00Cu, 0x01Cu, 0x00000FFFu},
    {0x050u, 0x054u, 0x07FFFFFFu},
    {0x0C4u, 0x0C8u, 0x0001FFFFu},
    {0x214u, 0x218u, 0x7FFFFFFFu},
};

constexpr uint32_t kBankCount = sizeof(kBanks) / sizeof(kBanks[0]);

class Msm8255Tlmm
    : public cerf_msm8255_gpio_detail::Msm8255GpioWindowBase<
          kTlmmBase, kTlmmSize, kBankCount, kBanks> {
public:
    using Msm8255GpioWindowBase::Msm8255GpioWindowBase;

    uint32_t ReadWord(uint32_t addr) override {
        if (addr - kTlmmBase == kHwRevisionNumber) {
            return (kRevision << kRevisionShift) | (kPartNum << kPartNumShift);
        }
        return Msm8255GpioWindowBase::ReadWord(addr);
    }
};

}

REGISTER_SERVICE(Msm8255Tlmm);
