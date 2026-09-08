#include "../armv7a_processor_config.h"

#include "../../core/cerf_emulator.h"
#include "../../boards/board_context.h"

namespace {

class ScorpionProcessorConfig : public Armv7aProcessorConfigBase {
public:
    using Armv7aProcessorConfigBase::Armv7aProcessorConfigBase;

    bool ShouldRegister() override {
        auto* bd = emu_.TryGet<BoardContext>();
        return bd && bd->GetSoc() == SocFamily::MSM8255;
    }

    /* Linux arch/arm/include/asm/cputype.h ARM_CPU_PART_SCORPION 0x510002d0
       under mask 0xff00fff0. ARM DDI 0406C.c B4.1.105: Implementer 0x51 = "Q"
       Qualcomm (p. B4-1649); Architecture bits[19:16] 0xF = defined by CPUID
       scheme, Table B4-11 (p. B4-1650). */
    uint32_t Midr() const override { return 0x510F02D0u; }

    /* Qualcomm MSM8X55/APQ8055 Snapdragon CPU Processors product brief,
       feature table, "CPU Processor" row: "Scorpion - up to 1.4GHz". Linux
       arch/arm/mach-msm acpuclock-7x30.c acpu_freq_tbl gives that top
       operating point as acpu_clk_khz 1401600 at vdd_mv 1250. */
    uint32_t CpuClockHz() const override { return 1401600u * 1000u; }

    /* Same product brief, "Security and DRM" row: "SecureMSM v4 with
       Trustzone". */
    bool HasSecurityExtensions() const override { return true; }

    /* Linux arch/arm/mm/proc-v7.S:267 reads and :277 writes the Auxiliary
       Control Register c1,c0,1 under CONFIG_ARCH_MSM_SCORPION. */
    bool HasAuxControlRegister() const override { return true; }

    /* ARM DDI 0406C.c B4.1.42 (p. B4-1556): format bits[31:29] 0b100 = ARMv7,
       CWG bits[27:24] and ERG bits[23:20] 0b0000 = not provided. Table B4-3
       (p. B4-1557): L1Ip bits[15:14] 0b00 Reserved, 0b11 = "Physical Index,
       Physical Tag (PIPT)". */
    uint32_t Ctr()           const override { return 0x8003C003u; }
    uint32_t CacheLineSize() const override { return 32u; }

    /* ARM DDI 0406C.c B4.1.20 Table B4-2: Ctype 0b011 = separate instruction
       and data caches (p. B4-1530), 0b100 = unified (p. B4-1531). LoUU 0 would
       mean no level needs cleaning to the point of unification (p. B2-1276).
       Linux arch/arm/mm/proc-v7.S:264 writes Scorpion L2CR1 under
       CONFIG_ARCH_MSM_SCORPION. */
    uint32_t Clidr() const override { return 0x0A000023u; }

    /* ARM DDI 0406C.c B4.1.19 (p. B4-1529): NumSets, Associativity and LineSize
       are the architecturally visible parameters for Set/Way maintenance, "not
       guaranteed to represent the actual microarchitectural features of a
       design". WT bit[31] / WB bit[30] / RA bit[29] / WA bit[28] per
       Table B4-1. */
    uint32_t Ccsidr(uint32_t csselr) const override {
        const uint32_t level = (csselr >> 1) & 0x7u;
        const uint32_t ind   =  csselr       & 0x1u;
        if (level == 0) {
            return ind ? 0x201FE019u : 0xE01FE019u;
        }
        if (level == 1) {
            return 0xF07FE039u;
        }
        return 0u;
    }

    /* ARM DDI 0406C.c B4.1.59: Implementer bits[31:24] uses the MIDR codes and
       SW bit[23] 0 = hardware floating-point (p. B4-1573); Subarchitecture
       bits[22:16] must set bit[22] for a non-ARM designer and is numbered from
       0x40, while Part / Variant / Revision bits[15:0] are IMPLEMENTATION
       DEFINED (p. B4-1574). Linux arch/arm/vfp/vfpmodule.c VFP_bounce takes the
       synchronous path only on subarchitecture 1 with FPSCR_IXE. */
    uint32_t Fpsid() const override { return 0x51400000u; }

    bool     HasVfp()  const override { return true; }
    bool     HasNeon() const override { return true; }

    /* ARM DDI 0406C.c B4.1.108 (p. B4-1655): VFP rounding modes bits[31:28]
       0b0001 = "All rounding modes supported"; short vectors bits[27:24],
       square root bits[23:20] and divide bits[19:16] 0b0001 = supported.
       Trapping bits[15:12] 0b0000 is the VFPv3/VFPv4 value, double-precision
       bits[11:8] 0b0010 (p. B4-1656); single-precision bits[7:4] 0b0010 =
       "Supported, VFPv3 or VFPv4" and A_SIMD registers bits[3:0] 0b0010 =
       "Supported, 32 x 64-bit registers" (p. B4-1657). */
    uint32_t Mvfr0()   const override { return 0x11110222u; }

    /* ARM DDI 0406C.c B4.1.109: A_SIMD FMAC bits[31:28] and both HPFP fields
       0b0000 = not implemented (p. B4-1658); A_SIMD SPFP / integer /
       load-store bits[19:8] 0b0001 = implemented, SPFP 0b0001 "permitted only
       if the A_SIMD integer field is 0b0001"; D_NaN mode bits[7:4] 0b0001 =
       "Hardware supports propagation of NaN values" and FtZ mode bits[3:0]
       0b0001 = "Hardware supports full denormalized number arithmetic"
       (p. B4-1659). */
    uint32_t Mvfr1()   const override { return 0x00011111u; }
};

}  /* namespace */

REGISTER_SERVICE_AS(ScorpionProcessorConfig, ArmProcessorConfig);
