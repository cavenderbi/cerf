#include <cstddef>
#include <cstdint>

#include "../../../cpu/arm_processor_config.h"
#include "../arm_cpu.h"
#include "../arm_emit_services.h"
#include "../arm_vfp.h"
#include "../cpu_state.h"
#include "../place_fns.h"
#include "../../x86_emit_alu.h"

/* DDI 0406C.c B9.3.21 VMRS (p. B9-2014) and B9.3.22 VMSR (p. B9-2016),
   encoding T1/A1: bits[27:20] = 1110 111L, "reg" at bits[19:16], coproc =
   1010, opc2 = (0)(0)(0), CRm = (0)(0)(0)(0). The application-level
   A8.8.348 / A8.8.349 forms hard-fix bits[19:16] and reach FPSCR only. */

uint8_t* EmitVfpSystemRegTransfer(uint8_t*      cursor,
                                  DecodedInsn*  d,
                                  BlockContext* ctx) {
    using namespace x86;
    ArmEmitServices* emit = ctx->emit;
    const int32_t rd_disp =
        static_cast<int32_t>(offsetof(ArmCpuState, gprs) + d->rd * 4u);

    /* CRn values per the ARM_VFP_* constants of QEMU target/arm/cpu.h. */
    constexpr uint32_t kFpsid  = 0;
    constexpr uint32_t kFpscr  = 1;
    constexpr uint32_t kMvfr1  = 6;
    constexpr uint32_t kMvfr0  = 7;
    constexpr uint32_t kFpexc  = 8;

    /* B9.3.21 (p. B9-2014) / B9.3.22 (p. B9-2016): a VMRS/VMSR of a
       Floating-point Extension System Register other than the FPSCR "is
       UNDEFINED if executed in User mode". ARM DDI 0274H Table 3-4 (p. 3-16):
       on VFP11, FPSID / FPSCR / MVFR0 / MVFR1 are "Any mode". VFPv2 is
       MVFR0.FPSP < 0b0010, modelled on QEMU translate-vfp.c trans_VMSR_VMRS. */
    const uint32_t fpsp = (emit->ProcessorConfig()->Mvfr0() >> 4) & 0xFu;
    const bool any_mode = d->crn == kFpscr
                       || (fpsp < 2u && (d->crn == kFpsid ||
                                         d->crn == kMvfr0 || d->crn == kMvfr1));
    if (!any_mode) {
        cursor = EmitRaiseUndIfUserMode(cursor, d, ctx);
    }

    if (d->l) {
        /* VMRS - read VFP system register → Rt. */
        switch (d->crn) {
        case kFpsid:
            if (d->rd == 15) {
                return EmitRaiseUndAndReturn(cursor, d, ctx);
            }
            EmitMovBaseDisp32Imm32(cursor, kStateReg, rd_disp,
                emit->ProcessorConfig()->Fpsid());
            return cursor;

        case kFpscr:
            if (d->rd == 15) {
                EmitPushBaseDisp32(cursor, kStateReg,
                    static_cast<int32_t>(offsetof(ArmCpuState, fpscr)));
                EmitPush32(cursor,
                    static_cast<uint32_t>(
                        reinterpret_cast<uintptr_t>(emit->Cpu())));
                EmitCall(cursor, reinterpret_cast<void*>(
                    &ArmCpu::UpdateNzcvOnlyHelper));
                EmitAddRegImm32(cursor, kEsp, 8);
                return cursor;
            }
            EmitMovRegBaseDisp32(cursor, kEax, kStateReg,
                static_cast<int32_t>(offsetof(ArmCpuState, fpscr)));
            EmitMovBaseDisp32Reg(cursor, kStateReg, rd_disp, kEax);
            return cursor;

        case kMvfr1:
            if (d->rd == 15) {
                return EmitRaiseUndAndReturn(cursor, d, ctx);
            }
            EmitMovBaseDisp32Imm32(cursor, kStateReg, rd_disp,
                emit->ProcessorConfig()->Mvfr1());
            return cursor;

        case kMvfr0:
            if (d->rd == 15) {
                return EmitRaiseUndAndReturn(cursor, d, ctx);
            }
            EmitMovBaseDisp32Imm32(cursor, kStateReg, rd_disp,
                emit->ProcessorConfig()->Mvfr0());
            return cursor;

        case kFpexc:
            if (d->rd == 15) {
                return EmitRaiseUndAndReturn(cursor, d, ctx);
            }
            EmitMovRegBaseDisp32(cursor, kEax, kStateReg,
                static_cast<int32_t>(offsetof(ArmCpuState, fpexc)));
            EmitMovBaseDisp32Reg(cursor, kStateReg, rd_disp, kEax);
            return cursor;
        }
        return EmitRaiseUndAndReturn(cursor, d, ctx);
    }

    /* VMSR - write Rt → VFP system register. */
    switch (d->crn) {
    case kFpsid:
    case kMvfr1:
    case kMvfr0:
        /* Read-only extension system registers: writes are ignored, per the
           model of QEMU target/arm/tcg/translate-vfp.c trans_VMSR_VMRS. */
        return cursor;

    case kFpscr:
        EmitMovRegBaseDisp32(cursor, kEax, kStateReg, rd_disp);
        /* DDI 0406C.c B4.1.58 (p. B4-1571): FPSCR bits[15, 12:8] trap enables
           "are reserved, RAZ/WI, on a system that implements VFPv3 or VFPv4";
           B4.1.108 (p. B4-1656) MVFR0 bits[15:12] "0b0000 Not supported. This
           is the value for VFPv3 and VFPv4". */
        if (((emit->ProcessorConfig()->Mvfr0() >> 12) & 0xFu) == 0u) {
            EmitAndRegImm32(cursor, kEax, ~ArmVfp::kFpscrTrapEnableMask);
        }
        EmitMovBaseDisp32Reg(cursor, kStateReg,
            static_cast<int32_t>(offsetof(ArmCpuState, fpscr)), kEax);
        return cursor;

    case kFpexc:
        EmitMovRegBaseDisp32(cursor, kEax, kStateReg, rd_disp);
        EmitMovBaseDisp32Reg(cursor, kStateReg,
            static_cast<int32_t>(offsetof(ArmCpuState, fpexc)), kEax);
        return cursor;
    }
    return EmitRaiseUndAndReturn(cursor, d, ctx);
}
