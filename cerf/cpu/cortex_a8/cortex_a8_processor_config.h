#pragma once

#include "../armv7a_processor_config.h"

/* Cortex-A8 core invariants identical across every Cortex-A8 SoC. Per-SoC
   concretes override only MIDR, CCSIDR/CLIDR, CpuClockHz, and the timer
   dividers - a per-part value placed here is reported by every Cortex-A8 SoC. */
class CortexA8ProcessorConfigBase : public Armv7aProcessorConfigBase {
public:
    using Armv7aProcessorConfigBase::Armv7aProcessorConfigBase;

    uint32_t CacheLineSize()              const override { return 64; }

    /* Cortex-A8 Cache Type Register reset value (ARM DDI0344K TRM, c0 Cache
       Type Register, page 3-20). */
    uint32_t Ctr()                        const override { return 0x82048004u; }

    bool     HasL1SystemArrayDebug()      const override { return true; }

    /* ARM DDI 0344 §3.2.26 c1, Auxiliary Control Register (p. 3-47). */
    bool     HasAuxControlRegister()      const override { return true; }
    /* ARM DDI 0344 §2.1: "The processor implements the ARMv7-A architecture.
       This includes ... the Security Extensions architecture". */
    bool     HasSecurityExtensions()      const override { return true; }
    bool     HasL2CacheAuxControl()       const override { return true; }

    bool     HasVfp()  const override { return true; }
    bool     HasNeon() const override { return true; }
    uint32_t Fpsid()   const override { return 0x410330C0u; }
    uint32_t Mvfr0()   const override { return 0x11110222u; }
    uint32_t Mvfr1()   const override { return 0x00011111u; }
};
