#pragma once

#include <cstdint>

#include "../jit/arm/coproc_emitter.h"
#include "../jit/arm/decoded_insn.h"
#include "../jit/arm/place_fns.h"

class Armv7aCoprocEmitterBase : public CoprocEmitter {
public:
    using CoprocEmitter::CoprocEmitter;

    uint8_t* EmitRegisterTransfer(uint8_t*      cursor,
                                  DecodedInsn*  d,
                                  BlockContext* ctx) override;

    uint8_t* EmitDataTransfer(uint8_t*      cursor,
                              DecodedInsn*  d,
                              BlockContext* ctx) override;

    uint8_t* EmitDataOperation(uint8_t*      cursor,
                               DecodedInsn*  d,
                               BlockContext* ctx) override;
};
