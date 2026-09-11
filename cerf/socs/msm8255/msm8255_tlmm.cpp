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

/* Linux arch/arm/mach-msm gpio_hw.h under CONFIG_ARCH_MSM7X30: banks 0 and 2
   through 7 of MSM_GPIO_OUT_, _OE_, _INT_EDGE_, _INT_POS_, _INT_EN_ and
   _INT_CLEAR_, unbiased, each with that bank's annotated gpio range. */
constexpr Msm8255GpioBank kBanks[] = {
    {0x000u, 0x010u, 0x060u, 0x070u, 0x080u, 0x090u,   0u,  15u},
    {0x004u, 0x014u, 0x064u, 0x074u, 0x084u, 0x094u,  44u,  67u},
    {0x008u, 0x018u, 0x068u, 0x078u, 0x088u, 0x098u,  68u,  94u},
    {0x00Cu, 0x01Cu, 0x06Cu, 0x07Cu, 0x08Cu, 0x09Cu,  95u, 106u},
    {0x050u, 0x054u, 0x0C0u, 0x0BCu, 0x0B8u, 0x0B4u, 107u, 133u},
    {0x0C4u, 0x0C8u, 0x0D0u, 0x0D4u, 0x0D8u, 0x0DCu, 134u, 150u},
    {0x214u, 0x218u, 0x240u, 0x228u, 0x22Cu, 0x230u, 151u, 181u},
};

constexpr uint32_t kBankCount = sizeof(kBanks) / sizeof(kBanks[0]);

constexpr uint32_t kMuxSelectNumber = 0x020u;
constexpr uint32_t kMuxConfigNumber = 0x024u;

class Msm8255Tlmm
    : public cerf_msm8255_gpio_detail::Msm8255GpioWindowBase<
          kTlmmBase, kTlmmSize, kBankCount, kBanks, kMuxSelectNumber,
          kMuxConfigNumber> {
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
