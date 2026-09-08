#include "arm_unconditional_space_decoder.h"

#include "../../boards/board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../cpu/arm_processor_config.h"
#include "arm_opcode.h"
#include "cpu_state.h"
#include "decoded_insn.h"
#include "neon_unconditional_decoder.h"
#include "place_fns.h"

REGISTER_SERVICE(ArmUnconditionalSpaceDecoder);

bool ArmUnconditionalSpaceDecoder::ShouldRegister() {
    return emu_.Get<BoardContext>().GetCpuArch() == CpuArch::Arm;
}

void ArmUnconditionalSpaceDecoder::OnReady() {
    processor_config_           = &emu_.Get<ArmProcessorConfig>();
    neon_unconditional_decoder_ = &emu_.Get<NeonUnconditionalDecoder>();
}

bool ArmUnconditionalSpaceDecoder::Decode(DecodedInsn* insn, ArmOpcode op) {
    /* RFE - Return From Exception. ddi0406c B9.3.13 encoding A1:
       1111 100P U0W1 nnnn 0000 1010 0000 0000.
       Extract P (bit 24), U (bit 23), W (bit 21), Rn (bits 19:16).
       Table A5-23 (p. A5-216) marks the RFE and SRS rows v6. */
    if (processor_config_->HasCp15V6() &&
        (op.word & 0xFE50FFFFu) == 0xF8100A00u) {
        const uint32_t rn = (op.word >> 16) & 0xFu;
        if (rn == ArmGpr::kR15) {
            return false;  /* UNPREDICTABLE per spec - fall through to UND. */
        }
        insn->place_fn            = &PlaceRfe;
        insn->p                   = (op.word >> 24) & 0x1u;
        insn->u                   = (op.word >> 23) & 0x1u;
        insn->w                   = (op.word >> 21) & 0x1u;
        insn->rn                  = rn;
        insn->r15_modified        = true;
        insn->is_exception_return = 1;
        return true;
    }

    /* SRS - Store Return State. ddi0406c B9.3.16 encoding A1:
       1111 100P U1W0 1101 0000 0101 000 mode[4:0].
       Extract P (bit 24), U (bit 23), W (bit 21), target_mode (bits 4:0). */
    if (processor_config_->HasCp15V6() &&
        (op.word & 0xFE5FFFE0u) == 0xF84D0500u) {
        insn->place_fn  = &PlaceSrs;
        insn->p         = (op.word >> 24) & 0x1u;
        insn->u         = (op.word >> 23) & 0x1u;
        insn->w         = (op.word >> 21) & 0x1u;
        insn->immediate = op.word & 0x1Fu;  /* target_mode */
        return true;
    }

    /* DSB / DMB / ISB, ddi0406c A8.8.44 / A8.8.43 / A8.8.53 encoding A1:
       1111 0101 0111 (1)x4 (1)x4 (0)x4 01xx option, bits [7:4] = 0100 DSB /
       0101 DMB / 0110 ISB. NOP emit on x86's strong memory model. */
    if (processor_config_->HasBarrierInsn() &&
        (op.word & 0xFFFFFF00u) == 0xF57FF000u) {
        const uint32_t barrier_op = (op.word >> 4) & 0xFu;
        if (barrier_op >= 0x4u && barrier_op <= 0x6u) {
            /* DDI 0406C.c Glossary, "Context synchronization operation": one
               of "Performing an ISB operation ... Taking an exception ...
               Returning from an exception". DSB and DMB are not in that set. */
            insn->context_sync = barrier_op == 0x6u;
            insn->place_fn     = &PlaceNop;
            return true;
        }
    }

    const uint32_t op1 = (op.word >> 20) & 0xFFu;

    /* DDI 0406C.c Table A5-24 (p. A5-217): the Advanced SIMD element and
       structure load/store row is 100xxx0 over op1 = insn[26:20], so
       insn[20] == 1 selects the PLI and memory-hint rows. */
    if (processor_config_->HasNeon() && (op1 & 0xF1u) == 0x40u) {
        return neon_unconditional_decoder_->DecodeLoadStore(insn, op);
    }

    if (processor_config_->HasNeon() && op.neon_data_3reg.marker == 0x1u) {
        return neon_unconditional_decoder_->DecodeData3reg(insn, op);
    }

    /* DDI 0406C.c Table A5-24 (p. A5-217) rows 101x101 PLD (immediate) and
       PLD (literal), 111x101 with op2 xxx0 PLD (register); "Preloading
       caches" (p. A3-158): "The Preload instructions are hints, and so
       implementations can treat them as NOPs". */
    if (processor_config_->HasPreload() &&
        ((op1 & 0xF7u) == 0x55u ||
         ((op1 & 0xF7u) == 0x75u && (op.word & 0x10u) == 0u))) {
        insn->place_fn = &PlaceNop;
        return true;
    }

    /* DDI 0406C.c Table A5-23 (p. A5-216), op1 = insn[27:20]; its 110xxxxx and
       1110xxxx rows are the cond == 0b1111 coprocessor forms. B3.15.2
       (p. B3-1446) makes the CDP2/MCR2/MRC2/MCRR2/MRRC2/LDC2/STC2 forms
       UNDEFINED on CP14 and CP15, A7.5-A7.9 (pp. A7-272 .. A7-279) on cp10 and
       cp11; A2.9 (p. A2-94) reserves CP8, CP9, CP12, CP13; B1.9.2 (p. B1-1206)
       UNDEFs "a coprocessor instruction that is not implemented". */
    if ((op1 & 0xE0u) == 0xC0u || (op1 & 0xF0u) == 0xE0u) {
        return false;
    }
    /* DDI 0406C.c Table A5-23 (p. A5-216) row 101xxxxx is "Branch with Link
       and Exchange"; A8.8.25 encoding A2 (p. A8-348) is 1111 101H imm24 with
       imm32 = SignExtend(imm24:H:'0', 32). */
    if ((op1 & 0xE0u) == 0xA0u) {
        insn->offset       = (static_cast<int32_t>(op.word << 8) >> 6) |
                             static_cast<int32_t>((op.word >> 23) & 2u);
        insn->r15_modified = true;
        insn->place_fn     = &PlaceArmBlxImm;
        return true;
    }
    if ((op1 & 0x80u) == 0x00u) {
        return MarkArmUnimplemented(insn, op.word);
    }
    return false;
}
