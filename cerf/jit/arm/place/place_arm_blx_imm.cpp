#include <cstddef>

#include "../block_context.h"
#include "../cpu_state.h"
#include "../place_fns.h"
#include "../../x86_emit.h"

/* ARM DDI 0406C.c A8.8.25 BL, BLX (immediate) Operation (p. A8-349): with
   CurrentInstrSet() == InstrSet_ARM, LR = PC - 4; with targetInstrSet ==
   InstrSet_Thumb, targetAddress = PC + imm32 and no Align; SelectInstrSet
   then BranchWritePC. */
uint8_t* PlaceArmBlxImm(uint8_t* cursor, DecodedInsn* d, BlockContext* ctx) {
    using namespace x86;

    EmitMovBaseDisp32Imm32(cursor, kStateReg,
        static_cast<int32_t>(offsetof(ArmCpuState, gprs) + ArmGpr::kR14 * 4u),
        d->guest_address + d->length);
    cursor = EmitArmInterworkingPcImm32(cursor,
        (ArmPcReadValue(d, ctx) + static_cast<uint32_t>(d->offset)) | 1u);
    return PlaceR15ModifiedHelper(cursor, d, ctx);
}
