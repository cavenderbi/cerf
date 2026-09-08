#include "../armv7a_coproc_emitter_base.h"

#include "../../core/cerf_emulator.h"
#include "../../boards/board_context.h"
#include "../../jit/arm/cpu_state.h"
#include "../../jit/x86_emit_alu.h"
#include "../../socs/irq_controller.h"

#include <cstddef>

namespace {

class ScorpionCoprocEmitter : public Armv7aCoprocEmitterBase {
public:
    using Armv7aCoprocEmitterBase::Armv7aCoprocEmitterBase;

    bool ShouldRegister() override {
        auto* bd = emu_.TryGet<BoardContext>();
        return bd && bd->GetSoc() == SocFamily::MSM8255;
    }

    /* ARM DDI 0406C.c Figure B3-35 (p. B3-1477): CP15 c9 is CRm {c0-c2} and
       {c5-c8} "Reserved for Branch Predictor, Cache and TCM operations",
       {c12-c14} "Reserved for ARM Performance Monitors Extension", and c15
       "Reserved for IMPLEMENTATION DEFINED performance monitors". Cache and
       TCM lockdown registers, VMSA (p. B4-1753): in the first group "the
       naming and behavior of registers or operations defined in these
       regions is IMPLEMENTATION DEFINED". */
    uint8_t* EmitRegisterTransfer(uint8_t*      cursor,
                                  DecodedInsn*  d,
                                  BlockContext* ctx) override {
        if (d->cp_num == 15u && d->crn == 9u) {
            if (!d->l && d->cp_opc == 0u && d->crm == 0u && d->cp == 6u) {
                return cursor;
            }
            return EmitCoprocUnimplementedFatal(cursor, d, ctx);
        }
        if (d->cp_num == 15u && d->crn == 15u && d->cp_opc == 7u &&
            d->crm == 0u && d->cp == 3u) {
            if (!d->l) return EmitCoprocUnimplementedFatal(cursor, d, ctx);
            using namespace x86;
            EmitMovRegImm32(cursor, kEcx,
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                    &emu_.Get<IrqController>())));
            EmitCall(cursor, reinterpret_cast<void*>(
                &IrqController::ReadPendingVectorHelper));
            EmitMovBaseDisp32Reg(cursor, kStateReg,
                static_cast<int32_t>(offsetof(ArmCpuState, gprs) + d->rd * 4u),
                kEax);
            return cursor;
        }
        return Armv7aCoprocEmitterBase::EmitRegisterTransfer(cursor, d, ctx);
    }
};

}  /* namespace */

REGISTER_SERVICE_AS(ScorpionCoprocEmitter, CoprocEmitter);
