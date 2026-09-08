#include "arm_neon_2reg_reciprocal.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../../core/cerf_emulator.h"
#include "arm_cpu.h"
#include "arm_vfp.h"

REGISTER_SERVICE(ArmNeon2RegReciprocal);

namespace {

inline uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

inline float FromBits(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

inline uint64_t DpBits(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}

inline double FromDpBits(uint64_t b) {
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

/* ARM DDI 0406C.c UnsignedRecipEstimate() (p. A2-85). */
uint32_t UnsignedRecipEstimate(uint32_t operand) {
    if ((operand & 0x80000000u) == 0u) {
        return 0xFFFFFFFFu;
    }
    /* dp_operand = '0 01111111110' : operand<30:0> : Zeros(21).
       Sign=0, biased exponent=1022 (= 0x3FE), fraction = bits[51:21]. */
    const uint64_t dp_bits =
        (uint64_t{0x3FE} << 52) | (static_cast<uint64_t>(operand & 0x7FFFFFFFu) << 21);
    const double estimate = ArmVfp::RecipEstimate(FromDpBits(dp_bits));
    const uint64_t est_bits = DpBits(estimate);
    /* result = '1' : estimate<51:21>  (31-bit slice). */
    return 0x80000000u | static_cast<uint32_t>((est_bits >> 21) & 0x7FFFFFFFu);
}

/* ARM DDI 0406C.c UnsignedRSqrtEstimate() (p. A2-88). */
uint32_t UnsignedRSqrtEstimate(uint32_t operand) {
    if ((operand & 0xC0000000u) == 0u) {
        /* operand <= 0x3FFFFFFF */
        return 0xFFFFFFFFu;
    }
    uint64_t dp_bits;
    if (operand & 0x80000000u) {
        /* dp_operand = '0 01111111110' : operand<30:0> : Zeros(21). */
        dp_bits = (uint64_t{0x3FE} << 52)
                | (static_cast<uint64_t>(operand & 0x7FFFFFFFu) << 21);
    } else {
        /* operand<31:30> == 01 → dp_operand = '0 01111111101' :
           operand<29:0> : Zeros(22). */
        dp_bits = (uint64_t{0x3FD} << 52)
                | (static_cast<uint64_t>(operand & 0x3FFFFFFFu) << 22);
    }
    const double estimate = ArmVfp::RecipSqrtEstimate(FromDpBits(dp_bits));
    const uint64_t est_bits = DpBits(estimate);
    return 0x80000000u | static_cast<uint32_t>((est_bits >> 21) & 0x7FFFFFFFu);
}

/* ARM DDI 0406C.c FPRecipEstimate() (p. A2-85), whose FPUnpack (p. A2-76) runs
   under StandardFPSCRValue() (p. A2-80) with DN and FZ set, so a denormal
   flushes to zero signalling FPExc_InputDenorm and FPProcessNaN (p. A2-77)
   yields FPDefaultNaN (p. A2-75), FPExc_InvalidOp only for an SNaN. */
uint32_t FPRecipEstimate(uint32_t operand_bits, uint32_t* fpscr) {
    const uint32_t sign_bit = operand_bits & 0x80000000u;
    const uint32_t exp_bits = (operand_bits >> 23) & 0xFFu;
    const uint32_t frac_bits = operand_bits & 0x7FFFFFu;

    if (exp_bits == 0xFFu && frac_bits != 0u) {
        if ((operand_bits & 0x400000u) == 0u) {
            *fpscr |= ArmVfp::kFpscrIocMask;
        }
        return 0x7FC00000u;
    }
    if (exp_bits == 0xFFu) {
        return sign_bit;
    }
    if (exp_bits == 0u) {
        if (frac_bits != 0u) {
            *fpscr |= ArmVfp::kFpscrIdcMask;
        }
        *fpscr |= ArmVfp::kFpscrDzcMask;
        return sign_bit | 0x7F800000u;
    }
    /* "elsif Abs(value) >= 2^126" - biased exponent 253 is exactly 2^126. */
    if (exp_bits >= 253u) {
        *fpscr |= ArmVfp::kFpscrUfcMask;
        return sign_bit;
    }
    /* "scaled = '0 01111111110' : operand<22:0> : Zeros(29);
        result_exp = 253 - UInt(operand<30:23>);
        result = sign : result_exp<7:0> : estimate<51:29>;" */
    const uint64_t scaled = (uint64_t{0x3FE} << 52)
                          | (static_cast<uint64_t>(frac_bits) << 29);
    const double estimate = ArmVfp::RecipEstimate(FromDpBits(scaled));
    const uint64_t est_bits = DpBits(estimate);
    const uint32_t result_exp = 253u - exp_bits;
    return sign_bit
         | ((result_exp & 0xFFu) << 23)
         | static_cast<uint32_t>((est_bits >> 29) & 0x7FFFFFu);
}

/* ARM DDI 0406C.c FPRSqrtEstimate() (p. A2-87), with the same FPUnpack /
   StandardFPSCRValue / FPProcessNaN grounding as FPRecipEstimate above. Its
   FPType_Zero arm precedes the sign arm, so a negative zero or negative
   denormal yields -Inf with FPExc_DivideByZero, never FPExc_InvalidOp. */
uint32_t FPRSqrtEstimate(uint32_t operand_bits, uint32_t* fpscr) {
    const uint32_t sign_bit  = operand_bits & 0x80000000u;
    const uint32_t exp_bits  = (operand_bits >> 23) & 0xFFu;
    const uint32_t frac_bits = operand_bits & 0x7FFFFFu;

    if (exp_bits == 0xFFu && frac_bits != 0u) {
        if ((operand_bits & 0x400000u) == 0u) {
            *fpscr |= ArmVfp::kFpscrIocMask;
        }
        return 0x7FC00000u;
    }
    if (exp_bits == 0u) {
        if (frac_bits != 0u) {
            *fpscr |= ArmVfp::kFpscrIdcMask;
        }
        *fpscr |= ArmVfp::kFpscrDzcMask;
        return sign_bit | 0x7F800000u;
    }
    if (sign_bit != 0u) {
        *fpscr |= ArmVfp::kFpscrIocMask;
        return 0x7FC00000u;
    }
    if (exp_bits == 0xFFu) {
        return 0u;
    }
    /* "if operand<23> == '0' then scaled = '0 01111111110' : operand<22:0> :
        Zeros(29); else scaled = '0 01111111101' : operand<22:0> : Zeros(29);
        result_exp = (380 - UInt(operand<30:23>)) DIV 2;
        result = '0' : result_exp<7:0> : estimate<51:29>;" */
    const uint64_t scaled =
        (((operand_bits & (1u << 23)) == 0u) ? (uint64_t{0x3FE} << 52)
                                             : (uint64_t{0x3FD} << 52))
        | (static_cast<uint64_t>(frac_bits) << 29);
    const double estimate = ArmVfp::RecipSqrtEstimate(FromDpBits(scaled));
    const uint64_t est_bits = DpBits(estimate);
    const uint32_t result_exp = (380u - exp_bits) >> 1;
    return ((result_exp & 0xFFu) << 23)
         | static_cast<uint32_t>((est_bits >> 29) & 0x7FFFFFu);
}

}  /* namespace */

void ArmNeon2RegReciprocal::HandleReciprocal(uint32_t op_sel, uint32_t F,
                                             uint32_t d_idx, uint32_t m_idx,
                                             uint32_t regs) {
    auto* state = emu_.Get<ArmCpu>().State();
    /* esize=32 always (decoder UNDs other sizes); 2 elements per D-reg. */
    for (uint32_t r = 0; r < regs; ++r) {
        const uint8_t* src =
            reinterpret_cast<const uint8_t*>(&state->vfp_d[m_idx + r]);
        uint8_t res[8];
        for (uint32_t e = 0; e < 2u; ++e) {
            uint32_t in;
            std::memcpy(&in, src + e * 4u, 4);
            uint32_t out;
            if (op_sel == kRecpe) {
                out = (F != 0u) ? FPRecipEstimate(in, &state->fpscr)
                                : UnsignedRecipEstimate(in);
            } else {
                out = (F != 0u) ? FPRSqrtEstimate(in, &state->fpscr)
                                : UnsignedRSqrtEstimate(in);
            }
            std::memcpy(res + e * 4u, &out, 4);
        }
        std::memcpy(&state->vfp_d[d_idx + r], res, 8);
    }
}

void __cdecl ArmNeon2RegReciprocal::HandleReciprocalHelper(
        ArmNeon2RegReciprocal* svc, uint32_t op_sel, uint32_t F,
        uint32_t d_idx, uint32_t m_idx, uint32_t regs) {
    svc->HandleReciprocal(op_sel, F, d_idx, m_idx, regs);
}
