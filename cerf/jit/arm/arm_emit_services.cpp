#include "arm_emit_services.h"

#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../cpu/arm_processor_config.h"
#include "arm_cpu.h"
#include "arm_exception_frame.h"
#include "arm_interrupt_channel.h"
#include "arm_mmu.h"
#include "arm_mmu_probe.h"
#include "arm_neon.h"
#include "arm_neon_2reg_bitcount.h"
#include "arm_neon_2reg_bitwise_not.h"
#include "arm_neon_2reg_compare_zero.h"
#include "arm_neon_2reg_cvt_half_single.h"
#include "arm_neon_2reg_cvt_int_fp.h"
#include "arm_neon_2reg_narrow.h"
#include "arm_neon_2reg_pairwise_add_long.h"
#include "arm_neon_2reg_reciprocal.h"
#include "arm_neon_2reg_reverse.h"
#include "arm_neon_2reg_sat_abs_neg.h"
#include "arm_neon_2reg_scalar_mul.h"
#include "arm_neon_2reg_shuffle.h"
#include "arm_neon_2reg_swap.h"
#include "arm_neon_2reg_unary_arith.h"
#include "arm_neon_2regscalar.h"
#include "arm_neon_3difflen.h"
#include "arm_neon_3same_fp_abs_compare.h"
#include "arm_neon_3same_fp_arith.h"
#include "arm_neon_3same_fp_compare.h"
#include "arm_neon_3same_fp_fma.h"
#include "arm_neon_3same_fp_min_max.h"
#include "arm_neon_3same_fp_mul_acc.h"
#include "arm_neon_3same_fp_pair_add.h"
#include "arm_neon_3same_fp_pair_min_max.h"
#include "arm_neon_3same_fp_recip_step.h"
#include "arm_neon_one_reg_imm.h"
#include "arm_neon_sat.h"
#include "arm_neon_scalar_move.h"
#include "arm_neon_shift_imm.h"
#include "arm_neon_simd_3same.h"
#include "arm_neon_vext.h"
#include "arm_neon_vtbl.h"
#include "arm_routed_access.h"
#include "arm_translation_cache.h"
#include "arm_vfp.h"
#include "arm_vfp_memory.h"
#include "coproc_emitter.h"

REGISTER_SERVICE(ArmEmitServices);

void ArmEmitServices::OnReady() {
    fatal_     = &emu_.Get<Fatal>();
    cpu_       = &emu_.Get<ArmCpu>();
    cpu_state_ = cpu_->State();
    mmu_       = &emu_.Get<ArmMmu>();
    mmu_probe_ = &emu_.Get<ArmMmuProbe>();

    coproc_           = &emu_.Get<CoprocEmitter>();
    processor_config_ = &emu_.Get<ArmProcessorConfig>();

    translation_cache_ = &emu_.Get<ArmTranslationCache>();
    interrupt_channel_ = &emu_.Get<ArmInterruptChannel>();
    exception_frame_   = &emu_.Get<ArmExceptionFrame>();
    routed_access_     = &emu_.Get<ArmRoutedAccess>();

    neon_                        = &emu_.Get<ArmNeon>();
    simd3same_                   = &emu_.Get<ArmNeonSimd3Same>();
    neon_sat_                    = &emu_.Get<ArmNeonSat>();
    neon_shift_imm_              = &emu_.Get<ArmNeonShiftImm>();
    neon_one_reg_imm_            = &emu_.Get<ArmNeonOneRegImm>();
    neon_scalar_move_            = &emu_.Get<ArmNeonScalarMove>();
    neon_vext_                   = &emu_.Get<ArmNeonVext>();
    neon_vtbl_                   = &emu_.Get<ArmNeonVtbl>();
    neon_2reg_scalar_            = &emu_.Get<ArmNeon2RegScalar>();
    neon_3difflen_               = &emu_.Get<ArmNeon3DiffLen>();
    neon_2reg_bitcount_          = &emu_.Get<ArmNeon2RegBitcount>();
    neon_2reg_bitwise_not_       = &emu_.Get<ArmNeon2RegBitwiseNot>();
    neon_2reg_compare_zero_      = &emu_.Get<ArmNeon2RegCompareZero>();
    neon_2reg_cvt_half_single_   = &emu_.Get<ArmNeon2RegCvtHalfSingle>();
    neon_2reg_cvt_int_fp_        = &emu_.Get<ArmNeon2RegCvtIntFp>();
    neon_2reg_narrow_            = &emu_.Get<ArmNeon2RegNarrow>();
    neon_2reg_pairwise_add_long_ = &emu_.Get<ArmNeon2RegPairwiseAddLong>();
    neon_2reg_reciprocal_        = &emu_.Get<ArmNeon2RegReciprocal>();
    neon_2reg_reverse_           = &emu_.Get<ArmNeon2RegReverse>();
    neon_2reg_sat_abs_neg_       = &emu_.Get<ArmNeon2RegSatAbsNeg>();
    neon_2reg_scalar_mul_        = &emu_.Get<ArmNeon2RegScalarMul>();
    neon_2reg_shuffle_           = &emu_.Get<ArmNeon2RegShuffle>();
    neon_2reg_swap_              = &emu_.Get<ArmNeon2RegSwap>();
    neon_2reg_unary_arith_       = &emu_.Get<ArmNeon2RegUnaryArith>();
    neon_3same_fp_abs_compare_   = &emu_.Get<ArmNeon3SameFpAbsCompare>();
    neon_3same_fp_arith_         = &emu_.Get<ArmNeon3SameFpArith>();
    neon_3same_fp_compare_       = &emu_.Get<ArmNeon3SameFpCompare>();
    neon_3same_fp_fma_           = &emu_.Get<ArmNeon3SameFpFma>();
    neon_3same_fp_min_max_       = &emu_.Get<ArmNeon3SameFpMinMax>();
    neon_3same_fp_mul_acc_       = &emu_.Get<ArmNeon3SameFpMulAcc>();
    neon_3same_fp_pair_add_      = &emu_.Get<ArmNeon3SameFpPairAdd>();
    neon_3same_fp_pair_min_max_  = &emu_.Get<ArmNeon3SameFpPairMinMax>();
    neon_3same_fp_recip_step_    = &emu_.Get<ArmNeon3SameFpRecipStep>();
    vfp_                         = &emu_.Get<ArmVfp>();
    vfp_mem_                     = &emu_.Get<ArmVfpMemory>();
}
