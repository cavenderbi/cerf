#pragma once

#include "../../state/state_stream.h"

#include <atomic>
#include <cstdint>

/* Linux arch/arm/mach-msm gpio_hw.h under CONFIG_ARCH_MSM7X30: each bank has an
   output and an output-enable register, and every MSM_GPIO_OUT_n carries the
   contiguous gpio range that bank serves as its annotation. */
struct Msm8255GpioBank {
    uint32_t out;
    uint32_t oe;
    uint32_t lo;
    uint32_t hi;

    constexpr uint32_t Span() const { return hi - lo + 1u; }

    constexpr uint32_t Pins() const {
        return (uint32_t)((((uint64_t)1u) << Span()) - 1u);
    }

    constexpr bool Owns(uint32_t pin) const { return pin >= lo && pin <= hi; }
};

enum class Msm8255GpioAccess { NotMine, PinsAbsent, Served };

template <uint32_t kBankCount>
class Msm8255GpioBanks {
public:
    explicit Msm8255GpioBanks(const Msm8255GpioBank (&banks)[kBankCount])
        : banks_(banks) {}

    bool OwnsPin(uint32_t pin) const {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            if (banks_[i].Owns(pin)) return true;
        }
        return false;
    }

    bool Read(uint32_t off, uint32_t& value) const {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            if (off == banks_[i].out) {
                value = out_[i].load(std::memory_order_acquire);
                return true;
            }
            if (off == banks_[i].oe) {
                value = oe_[i].load(std::memory_order_acquire);
                return true;
            }
        }
        return false;
    }

    Msm8255GpioAccess Write(uint32_t off, uint32_t value, uint32_t& bank_pins) {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            const bool out = off == banks_[i].out;
            if (!out && off != banks_[i].oe) continue;
            bank_pins = banks_[i].Pins();
            if ((value & ~bank_pins) != 0u) {
                return Msm8255GpioAccess::PinsAbsent;
            }
            (out ? out_[i] : oe_[i]).store(value, std::memory_order_release);
            return Msm8255GpioAccess::Served;
        }
        return Msm8255GpioAccess::NotMine;
    }

    void Reset() {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            out_[i].store(0u, std::memory_order_release);
            oe_[i].store(0u, std::memory_order_release);
        }
    }

    void Save(StateWriter& w) const {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            w.Write<uint32_t>(out_[i].load(std::memory_order_acquire));
            w.Write<uint32_t>(oe_[i].load(std::memory_order_acquire));
        }
    }

    bool Restore(StateReader& r, uint32_t& bad_off, uint32_t& bad_value) {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            uint32_t out = 0;
            uint32_t oe  = 0;
            r.Read(out);
            r.Read(oe);
            if ((out & ~banks_[i].Pins()) != 0u) {
                bad_off   = banks_[i].out;
                bad_value = out;
                return false;
            }
            if ((oe & ~banks_[i].Pins()) != 0u) {
                bad_off   = banks_[i].oe;
                bad_value = oe;
                return false;
            }
            out_[i].store(out, std::memory_order_release);
            oe_[i].store(oe, std::memory_order_release);
        }
        return true;
    }

private:
    const Msm8255GpioBank* banks_;

    std::atomic<uint32_t> out_[kBankCount] = {};
    std::atomic<uint32_t> oe_[kBankCount]  = {};
};
