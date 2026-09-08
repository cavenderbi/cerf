#include "../armv7a_coproc_emitter_base.h"

#include "../../core/cerf_emulator.h"
#include "../../boards/board_context.h"

namespace {

class CortexA8CoprocEmitter : public Armv7aCoprocEmitterBase {
public:
    using Armv7aCoprocEmitterBase::Armv7aCoprocEmitterBase;

    bool ShouldRegister() override {
        auto* bd = emu_.TryGet<BoardContext>();
        if (!bd) return false;
        const SocFamily soc = bd->GetSoc();
        return soc == SocFamily::OMAP3530 || soc == SocFamily::iMX51;
    }
};

}  /* namespace */

REGISTER_SERVICE_AS(CortexA8CoprocEmitter, CoprocEmitter);
