#pragma once

#include "../../state/state_stream.h"

#include <atomic>
#include <cstdint>

/* Linux arch/arm/mach-msm gpio_hw.h under CONFIG_ARCH_MSM7X30: a bank has one
   register in each family, and every MSM_GPIO_OUT_n carries the contiguous gpio
   range that bank serves as its annotation. */
struct Msm8255GpioBank {
    uint32_t out;
    uint32_t oe;
    uint32_t int_edge;
    uint32_t int_pos;
    uint32_t int_en;
    uint32_t int_clear;
    uint32_t lo;
    uint32_t hi;

    constexpr uint32_t Span() const { return hi - lo + 1u; }

    constexpr uint32_t Pins() const {
        return (uint32_t)((((uint64_t)1u) << Span()) - 1u);
    }

    constexpr bool Owns(uint32_t pin) const { return pin >= lo && pin <= hi; }
};

enum class Msm8255GpioAccess { NotMine, PinsAbsent, Served };

inline constexpr uint32_t kMsm8255GpioFamilies = 5u;

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
        uint32_t bank = 0;
        uint32_t fam  = 0;
        if (!Locate(off, bank, fam)) return false;
        value = regs_[bank][fam].load(std::memory_order_acquire);
        return true;
    }

    Msm8255GpioAccess Write(uint32_t off, uint32_t value, uint32_t& bank_pins) {
        uint32_t bank = 0;
        uint32_t fam  = 0;
        if (Locate(off, bank, fam)) {
            bank_pins = banks_[bank].Pins();
            if ((value & ~bank_pins) != 0u) {
                return Msm8255GpioAccess::PinsAbsent;
            }
            regs_[bank][fam].store(value, std::memory_order_release);
            return Msm8255GpioAccess::Served;
        }
        for (uint32_t i = 0; i < kBankCount; ++i) {
            if (off != banks_[i].int_clear) continue;
            bank_pins = banks_[i].Pins();
            if ((value & ~bank_pins) != 0u) {
                return Msm8255GpioAccess::PinsAbsent;
            }
            return Msm8255GpioAccess::Served;
        }
        return Msm8255GpioAccess::NotMine;
    }

    void Reset() {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            for (uint32_t f = 0; f < kMsm8255GpioFamilies; ++f) {
                regs_[i][f].store(0u, std::memory_order_release);
            }
        }
    }

    void Save(StateWriter& w) const {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            for (uint32_t f = 0; f < kMsm8255GpioFamilies; ++f) {
                w.Write<uint32_t>(regs_[i][f].load(std::memory_order_acquire));
            }
        }
    }

    bool Restore(StateReader& r, uint32_t& bad_off, uint32_t& bad_value) {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            for (uint32_t f = 0; f < kMsm8255GpioFamilies; ++f) {
                uint32_t value = 0;
                r.Read(value);
                if ((value & ~banks_[i].Pins()) != 0u) {
                    bad_off   = FamilyOffset(banks_[i], f);
                    bad_value = value;
                    return false;
                }
                regs_[i][f].store(value, std::memory_order_release);
            }
        }
        return true;
    }

private:
    static_assert(kMsm8255GpioFamilies == 5u,
                  "FamilyOffset enumerates exactly the read-write families");

    static constexpr uint32_t FamilyOffset(const Msm8255GpioBank& b,
                                           uint32_t fam) {
        switch (fam) {
        case 0u: return b.out;
        case 1u: return b.oe;
        case 2u: return b.int_edge;
        case 3u: return b.int_pos;
        default: return b.int_en;
        }
    }

    bool Locate(uint32_t off, uint32_t& bank, uint32_t& fam) const {
        for (uint32_t i = 0; i < kBankCount; ++i) {
            for (uint32_t f = 0; f < kMsm8255GpioFamilies; ++f) {
                if (off == FamilyOffset(banks_[i], f)) {
                    bank = i;
                    fam  = f;
                    return true;
                }
            }
        }
        return false;
    }

    const Msm8255GpioBank* banks_;

    std::atomic<uint32_t> regs_[kBankCount][kMsm8255GpioFamilies] = {};
};
