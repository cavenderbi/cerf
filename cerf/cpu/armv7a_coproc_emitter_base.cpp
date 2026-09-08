#include "armv7a_coproc_emitter_base.h"

uint8_t* Armv7aCoprocEmitterBase::EmitRegisterTransfer(uint8_t*      cursor,
                                                       DecodedInsn*  d,
                                                       BlockContext* ctx) {
    if (d->cp_num == 15) {
        return EmitCp15RegisterTransfer(cursor, d, ctx);
    }
    if (d->cp_num == 10 || d->cp_num == 11) {
        return EmitVfpRegisterTransfer(cursor, d, ctx);
    }
    /* ARM DDI 0406C.c C6.4 "The CP14 debug register interface"
       (p. C6-2123). */
    if (d->cp_num == 14) {
        return EmitCoprocUnimplementedFatal(cursor, d, ctx);
    }
    return EmitRaiseUndAndReturn(cursor, d, ctx);
}

uint8_t* Armv7aCoprocEmitterBase::EmitDataTransfer(uint8_t*      cursor,
                                                   DecodedInsn*  d,
                                                   BlockContext* ctx) {
    if (d->cp_num == 10 || d->cp_num == 11) {
        return EmitVfpDataTransfer(cursor, d, ctx);
    }
    /* DDI 0406C.c B3.15.2 (p. B3-1446) makes "all CDP, LDC and STC
       operations to CP14 and CP15" UNDEFINED, "except for the LDC access
       to DBGDTRTXint and the STC access to DBGDTRRXint specified in CP14
       debug register interface accesses on page C6-2124"; C6.4
       (p. C6-2124) gives those as "STC p14, c5, <addr_mode>" and
       "LDC p14, c5, <addr_mode>". */
    if (d->cp_num == 14 && d->crd == 5u) {
        return EmitCoprocDataTransferUnimplementedFatal(cursor, d, ctx);
    }
    return EmitRaiseUndAndReturn(cursor, d, ctx);
}

uint8_t* Armv7aCoprocEmitterBase::EmitDataOperation(uint8_t*      cursor,
                                                    DecodedInsn*  d,
                                                    BlockContext* ctx) {
    if (d->cp_num == 10 || d->cp_num == 11) {
        return EmitVfpDataOperation(cursor, d, ctx);
    }
    return EmitRaiseUndAndReturn(cursor, d, ctx);
}
