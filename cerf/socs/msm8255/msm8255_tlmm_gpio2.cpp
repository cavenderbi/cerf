#include "msm8255_gpio_window_impl.h"

#include <cstdint>

namespace {

/* Linux arch/arm/mach-msm msm_iomap-7x30.h: MSM7X30_GPIO2_PHYS 0xAC101000,
   MSM7X30_GPIO2_SIZE SZ_4K. */
constexpr uint32_t kGpio2Base = 0xAC101000u;
constexpr uint32_t kGpio2Size = 0x00001000u;

/* Linux arch/arm/mach-msm gpio_hw.h under CONFIG_ARCH_MSM7X30: bank 1 of
   MSM_GPIO_OUT_, _OE_, _INT_EDGE_, _INT_POS_, _INT_EN_ and _INT_CLEAR_ is
   MSM_GPIO2_REG(0x00, 0x08, 0x50, 0x58, 0x60, 0x68), which add 0x400. */
constexpr Msm8255GpioBank kBanks[] = {
    {0x400u, 0x408u, 0x450u, 0x458u, 0x460u, 0x468u, 16u, 43u},
};

constexpr uint32_t kBankCount = sizeof(kBanks) / sizeof(kBanks[0]);

constexpr uint32_t kMuxSelectNumber = 0x410u;
constexpr uint32_t kMuxConfigNumber = 0x414u;

class Msm8255TlmmGpio2
    : public cerf_msm8255_gpio_detail::Msm8255GpioWindowBase<
          kGpio2Base, kGpio2Size, kBankCount, kBanks, kMuxSelectNumber,
          kMuxConfigNumber> {
public:
    using Msm8255GpioWindowBase::Msm8255GpioWindowBase;
};

}

REGISTER_SERVICE(Msm8255TlmmGpio2);
