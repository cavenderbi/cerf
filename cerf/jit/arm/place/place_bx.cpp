#include <cstddef>

#include "../block_context.h"
#include "../cpu_state.h"
#include "../place_fns.h"
#include "../../x86_emit.h"

/* ARM DDI 0406C.c A8.8.27 BX (p. A8-353): BXWritePC(R[m]). */
uint8_t* PlaceBxImpl(uint8_t* cursor, DecodedInsn* d, BlockContext* ctx,
                     bool is_call) {
    using namespace x86;

    const bool pc_source = d->rm == ArmGpr::kR15;

    if (!pc_source) {
        EmitMovRegBaseDisp32(cursor, kEax, kStateReg,
            static_cast<int32_t>(offsetof(ArmCpuState, gprs) + d->rm * 4u));
    }
    if (is_call) {
        /* ARM DDI 0406C.c A8.8.26 BLX (register), p. A8-351; ARM DDI 0100I
           A7.1.18 BLX (2), p. A7-30. */
        EmitMovBaseDisp32Imm32(cursor, kStateReg,
            static_cast<int32_t>(offsetof(ArmCpuState, gprs) +
                                 ArmGpr::kR14 * 4u),
            ctx->thumb ? ((d->guest_address + 2u) | 1u)
                       :  (d->guest_address + 4u));
    }
    if (pc_source) {
        cursor = EmitArmInterworkingPcImm32(cursor, ArmPcReadValue(d, ctx));
    } else {
        cursor = EmitArmInterworkingFullEax(cursor);
        EmitMovBaseDisp32Reg(cursor, kStateReg,
            static_cast<int32_t>(offsetof(ArmCpuState, gprs) + 15u * 4u), kEax);
    }
    return PlaceR15ModifiedHelper(cursor, d, ctx);
}

uint8_t* PlaceBx(uint8_t* cursor, DecodedInsn* d, BlockContext* ctx) {
    return PlaceBxImpl(cursor, d, ctx, false);
}
