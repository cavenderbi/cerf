#include "msm8255_gpio_window_impl.h"

#include <cstdint>

namespace {

/* Linux arch/arm/mach-msm msm_iomap-7x30.h: MSM7X30_GPIO2_PHYS 0xAC101000,
   MSM7X30_GPIO2_SIZE SZ_4K. */
constexpr uint32_t kGpio2Base = 0xAC101000u;
constexpr uint32_t kGpio2Size = 0x00001000u;

/* Linux arch/arm/mach-msm gpio_hw.h under CONFIG_ARCH_MSM7X30: MSM_GPIO_OUT_1
   and MSM_GPIO_OE_1 are MSM_GPIO2_REG(0x00) and (0x08), which add 0x400 to
   this window's base; the mask is that bank's annotated gpio 43-16. */
constexpr Msm8255GpioBank kBanks[] = {
    {0x400u, 0x408u, 0x0FFFFFFFu},
};

constexpr uint32_t kBankCount = sizeof(kBanks) / sizeof(kBanks[0]);

class Msm8255TlmmGpio2
    : public cerf_msm8255_gpio_detail::Msm8255GpioWindowBase<
          kGpio2Base, kGpio2Size, kBankCount, kBanks> {
public:
    using Msm8255GpioWindowBase::Msm8255GpioWindowBase;
};

}

REGISTER_SERVICE(Msm8255TlmmGpio2);
