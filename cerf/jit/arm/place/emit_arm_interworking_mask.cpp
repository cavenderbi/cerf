#include <cstddef>

#include "../cpu_state.h"
#include "../place_fns.h"
#include "../../x86_emit.h"
#include "../../x86_emit_alu.h"

uint8_t* EmitArmInterworkingMaskEax(uint8_t* cursor) {
    using namespace x86;

    EmitTestRegImm32(cursor, kEax, 1);
    uint8_t* jz_to_rejoin = EmitJzLabel(cursor);

    EmitOrBaseDisp32Imm32(cursor, kStateReg,
                          static_cast<int32_t>(offsetof(ArmCpuState, cpsr)),
                          0x00000020u);
    EmitAndRegImm32(cursor, kEax, 0xFFFFFFFEu);

    FixupLabel(jz_to_rejoin, cursor);
    return cursor;
}

uint8_t* EmitArmInterworkingFullEax(uint8_t* cursor) {
    using namespace x86;

    /* BXWritePC (DDI 0406C A2.3.2, p. A2-47): bit0==1 -> Thumb at
       address<31:1>:'0'; else bit1==0 -> ARM; else UNPREDICTABLE
       (implemented as force-align to <31:2>:'00'). */
    EmitTestRegImm32(cursor, kEax, 1);
    uint8_t* jz_to_arm = EmitJzLabel(cursor);

    EmitOrBaseDisp32Imm32(cursor, kStateReg,
                          static_cast<int32_t>(offsetof(ArmCpuState, cpsr)),
                          0x00000020u);
    EmitAndRegImm32(cursor, kEax, 0xFFFFFFFEu);
    uint8_t* jmp_done = EmitJmpLabel(cursor);

    FixupLabel(jz_to_arm, cursor);
    EmitAndBaseDisp32Imm32(cursor, kStateReg,
                           static_cast<int32_t>(offsetof(ArmCpuState, cpsr)),
                           ~0x20u);
    EmitAndRegImm32(cursor, kEax, 0xFFFFFFFCu);

    FixupLabel(jmp_done, cursor);
    return cursor;
}

/* BXWritePC (DDI 0406C.c A2.3.2, p. A2-47): address<0>=='1' ->
   SelectInstrSet(Thumb) and BranchTo(address<31:1>:'0'); address<1>=='0' ->
   SelectInstrSet(ARM) and BranchTo(address); address<1:0>=='10' is
   UNPREDICTABLE (implemented as force-align to <31:2>:'00'). */
uint8_t* EmitArmInterworkingPcImm32(uint8_t* cursor, uint32_t address) {
    using namespace x86;

    const bool thumb = (address & 1u) != 0u;

    if (thumb) {
        EmitOrBaseDisp32Imm32(cursor, kStateReg,
                              static_cast<int32_t>(offsetof(ArmCpuState, cpsr)),
                              0x00000020u);
    } else {
        EmitAndBaseDisp32Imm32(cursor, kStateReg,
                               static_cast<int32_t>(offsetof(ArmCpuState, cpsr)),
                               ~0x20u);
    }
    EmitMovBaseDisp32Imm32(cursor, kStateReg,
        static_cast<int32_t>(offsetof(ArmCpuState, gprs) + ArmGpr::kR15 * 4u),
        address & (thumb ? 0xFFFFFFFEu : 0xFFFFFFFCu));
    return cursor;
}
