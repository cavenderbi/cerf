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

/* Linux arch/arm/mach-msm msm_iomap-7x30.h: MSM_SAW_PHYS, MSM_SAW_SIZE. */
constexpr uint32_t kSawBase = 0xC0102000u;
constexpr uint32_t kSawSize = 0x00001000u;

/* Linux arch/arm/mach-msm spm.c msm_spm_reg_offsets:
   MSM_SPM_REG_SAW_CFG, MSM_SPM_REG_SAW_SPM_CTL. */
constexpr uint32_t kRegCfg    = 0x10u;
constexpr uint32_t kRegSpmCtl = 0x14u;

constexpr uint32_t kReg04          = 0x04u;
constexpr uint32_t kReg04AcceptedA = 0u;
constexpr uint32_t kReg04AcceptedB = 0x08000000u;

constexpr uint32_t kRegReset = 0u;

/* Linux arch/arm/mach-msm board-semc_zeus.c msm_spm_platform_data:
   reg_init_values[MSM_SPM_REG_SAW_CFG] 0x05 and
   reg_init_values[MSM_SPM_REG_SAW_SPM_CTL] 0x18. */
constexpr uint32_t kCfgAccepted    = 0x05u;
constexpr uint32_t kSpmCtlAccepted = 0x18u;

constexpr uint32_t kSpmCtlIntermediate = 0x10u;

class Msm8255Saw : public Peripheral {
public:
    using Peripheral::Peripheral;

    bool ShouldRegister() override {
        return emu_.Get<BoardContext>().GetSoc() == SocFamily::MSM8255;
    }

    void OnReady() override {
        emu_.Get<GuestCpuReset>().RegisterResetListener([this](ResetLineKind) {
            reg04_.store(kRegReset, std::memory_order_release);
            cfg_.store(kRegReset, std::memory_order_release);
            spm_ctl_.store(kRegReset, std::memory_order_release);
        });
        emu_.Get<PeripheralDispatcher>().Register(this);
    }

    uint32_t MmioBase() const override { return kSawBase; }
    uint32_t MmioSize() const override { return kSawSize; }

    uint32_t ReadWord(uint32_t addr) override {
        switch (addr - MmioBase()) {
        case kReg04:     return reg04_.load(std::memory_order_acquire);
        case kRegCfg:    return cfg_.load(std::memory_order_acquire);
        case kRegSpmCtl: return spm_ctl_.load(std::memory_order_acquire);
        default:         HaltUnsupportedAccess("ReadWord", addr, 0);
        }
    }

    void WriteWord(uint32_t addr, uint32_t value) override {
        switch (addr - MmioBase()) {
        case kReg04:
            if (value != kReg04AcceptedA && value != kReg04AcceptedB) {
                HaltUnsupportedAccess("WriteWord", addr, value);
            }
            reg04_.store(value, std::memory_order_release);
            return;
        case kRegCfg:
            if (value != kCfgAccepted) {
                HaltUnsupportedAccess("WriteWord", addr, value);
            }
            cfg_.store(value, std::memory_order_release);
            return;
        case kRegSpmCtl:
            if (value != kSpmCtlIntermediate && value != kSpmCtlAccepted) {
                HaltUnsupportedAccess("WriteWord", addr, value);
            }
            spm_ctl_.store(value, std::memory_order_release);
            return;
        default:
            HaltUnsupportedAccess("WriteWord", addr, value);
        }
    }

    void SaveState(StateWriter& w) override {
        w.Write<uint32_t>(reg04_.load(std::memory_order_acquire));
        w.Write<uint32_t>(cfg_.load(std::memory_order_acquire));
        w.Write<uint32_t>(spm_ctl_.load(std::memory_order_acquire));
    }

    void RestoreState(StateReader& r) override {
        uint32_t reg04 = kRegReset;
        uint32_t cfg = kRegReset;
        uint32_t spm_ctl = kRegReset;
        r.Read(reg04);
        r.Read(cfg);
        r.Read(spm_ctl);
        if ((reg04 != kReg04AcceptedA && reg04 != kReg04AcceptedB) ||
            (cfg != kRegReset && cfg != kCfgAccepted) ||
            (spm_ctl != kRegReset && spm_ctl != kSpmCtlIntermediate &&
             spm_ctl != kSpmCtlAccepted)) {
            emu_.Get<Fatal>().Die(
                "msm8255 saw: restored +0x04 0x%08X CFG 0x%08X SPM_CTL 0x%08X "
                "carry values the guest never writes", reg04, cfg, spm_ctl);
        }
        reg04_.store(reg04, std::memory_order_release);
        cfg_.store(cfg, std::memory_order_release);
        spm_ctl_.store(spm_ctl, std::memory_order_release);
    }

private:
    std::atomic<uint32_t> reg04_{kRegReset};
    std::atomic<uint32_t> cfg_{kRegReset};
    std::atomic<uint32_t> spm_ctl_{kRegReset};
};

}

REGISTER_SERVICE(Msm8255Saw);
