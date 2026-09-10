#pragma once

#include "../../peripherals/peripheral_base.h"

#include "../../boards/board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../peripherals/peripheral_dispatcher.h"
#include "../../state/state_stream.h"
#include "../guest_cpu_reset.h"
#include "msm8255_gpio_banks.h"

#include <cstdint>
#include <typeinfo>

namespace cerf_msm8255_gpio_detail {

/* Linux arch/arm/mach-msm gpio_hw.h under CONFIG_ARCH_MSM7X30: the GPIO banks
   are split across two windows, MSM_GPIO1_REG and MSM_GPIO2_REG, each serving
   its own subset of the same output and output-enable register families. */
template <uint32_t kBase, uint32_t kSize, uint32_t kBankCount,
          const Msm8255GpioBank (&kBanks)[kBankCount]>
class Msm8255GpioWindowBase : public Peripheral {
public:
    using Peripheral::Peripheral;

    bool ShouldRegister() override {
        auto* bd = emu_.TryGet<BoardContext>();
        return bd && bd->GetSoc() == SocFamily::MSM8255;
    }

    void OnReady() override {
        emu_.Get<GuestCpuReset>().RegisterResetListener(
            [this](ResetLineKind) { banks_.Reset(); });
        emu_.Get<PeripheralDispatcher>().Register(this);
    }

    uint32_t MmioBase() const override { return kBase; }
    uint32_t MmioSize() const override { return kSize; }

    uint32_t ReadWord(uint32_t addr) override {
        uint32_t value = 0;
        if (banks_.Read(addr - kBase, value)) return value;
        HaltUnsupportedAccess("ReadWord", addr, 0u);
    }

    void WriteWord(uint32_t addr, uint32_t value) override {
        uint32_t pins = 0;
        const Msm8255GpioAccess access = banks_.Write(addr - kBase, value, pins);
        if (access == Msm8255GpioAccess::Served) return;
        if (access == Msm8255GpioAccess::PinsAbsent) {
            emu_.Get<Fatal>().Die(
                "Peripheral '%s': the 0x%08X written to +0x%03X drives pins "
                "outside the 0x%08X that register has",
                typeid(*this).name(), value, addr - kBase, pins);
        }
        HaltUnsupportedAccess("WriteWord", addr, value);
    }

    void SaveState(StateWriter& w) override { banks_.Save(w); }

    void RestoreState(StateReader& r) override {
        uint32_t bad_off   = 0;
        uint32_t bad_value = 0;
        if (!banks_.Restore(r, bad_off, bad_value)) {
            emu_.Get<Fatal>().Die(
                "Peripheral '%s': restored +0x%03X value 0x%08X carries pins "
                "that register does not have",
                typeid(*this).name(), bad_off, bad_value);
        }
    }

private:
    Msm8255GpioBanks<kBankCount> banks_{kBanks};
};

}
