#include "../../peripherals/peripheral_base.h"

#include "../../boards/board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../peripherals/peripheral_dispatcher.h"
#include "../../state/state_stream.h"
#include "../guest_cpu_reset.h"

#include <atomic>
#include <cstdint>

namespace {

/* Linux arch/arm/mach-msm msm_iomap-7x30.h: MSM_CLK_CTL_SH2_PHYS. */
constexpr uint32_t kSh2Base = 0xABA01000u;

constexpr uint32_t kSh2Size = 0x00002000u;

constexpr uint32_t kReg39C         = 0x39Cu;
constexpr uint32_t kReg39CAccepted = 0x00007000u;
constexpr uint32_t kReg39CReset    = 0u;

class Msm8255ClkCtlSh2 : public Peripheral {
public:
    using Peripheral::Peripheral;

    bool ShouldRegister() override {
        return emu_.Get<BoardContext>().GetSoc() == SocFamily::MSM8255;
    }

    void OnReady() override {
        emu_.Get<GuestCpuReset>().RegisterResetListener([this](ResetLineKind) {
            reg39c_.store(kReg39CReset, std::memory_order_release);
        });
        emu_.Get<PeripheralDispatcher>().Register(this);
    }

    uint32_t MmioBase() const override { return kSh2Base; }
    uint32_t MmioSize() const override { return kSh2Size; }

    uint32_t ReadWord(uint32_t addr) override {
        if (addr - MmioBase() != kReg39C) {
            HaltUnsupportedAccess("ReadWord", addr, 0);
        }
        return reg39c_.load(std::memory_order_acquire);
    }

    void WriteWord(uint32_t addr, uint32_t value) override {
        if (addr - MmioBase() != kReg39C ||
            (value & ~kReg39CAccepted) != 0u) {
            HaltUnsupportedAccess("WriteWord", addr, value);
        }
        reg39c_.store(value, std::memory_order_release);
    }

    void SaveState(StateWriter& w) override {
        w.Write<uint32_t>(reg39c_.load(std::memory_order_acquire));
    }

    void RestoreState(StateReader& r) override {
        uint32_t reg39c = kReg39CReset;
        r.Read(reg39c);
        if ((reg39c & ~kReg39CAccepted) != 0u) {
            emu_.Get<Fatal>().Die(
                "msm8255 clk_ctl_sh2: restored +0x39C value 0x%08X carries "
                "bits the guest never writes", reg39c);
        }
        reg39c_.store(reg39c, std::memory_order_release);
    }

private:
    std::atomic<uint32_t> reg39c_{kReg39CReset};
};

}

REGISTER_SERVICE(Msm8255ClkCtlSh2);
