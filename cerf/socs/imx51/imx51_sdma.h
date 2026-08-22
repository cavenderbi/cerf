#pragma once

#include "../freescale_sdma_impl.h"

#include "../irq_controller.h"
#include "imx51_ssi1.h"
#include "imx51_ssi2.h"
#include "imx51_ssi3.h"
#include "imx51_uart1.h"
#include "imx51_uart3.h"
#include "../../core/cerf_emulator.h"

/* SDMA AP interrupt = TZIC source 6 (MCIMX51RM Table 3-2, ARM Domain Interrupt
   Summary). */
constexpr uint32_t kTzicSourceSdma = 6u;

class Imx51Sdma : public cerf_freescale_sdma_detail::FreescaleSdmaBase<0x83FB0000u,
                                                                      SocFamily::iMX51> {
public:
    using FreescaleSdmaBase::FreescaleSdmaBase;

    void OnReady() override {
        FreescaleSdmaBase::OnReady();
        BindUarts();
        BindSsis();
    }

    /* The UART<->SDMA event binding is host-side wiring, not serialized machine
       state; re-assert it after a hibernation restore (RestoreState does not
       carry it), same as PostRestore re-drives the INTC level. */
    void PostRestore() override {
        FreescaleSdmaBase::PostRestore();
        BindUarts();
        BindSsis();
    }

protected:
    void AssertIrqLine()   override { emu_.Get<IrqController>().AssertIrq(kTzicSourceSdma); }
    void DeassertIrqLine() override { emu_.Get<IrqController>().DeAssertIrq(kTzicSourceSdma); }

    uint32_t ChnenblBase()  const override { return kOffChnenblBase; }
    uint32_t ChnenblCount() const override { return kChnenblCount; }

    /* cspddk runs every i.MX51 SDMA channel with extended 12-byte BDs: its
       per-event channel setup (sub_C09C3FAC) sets the EXTD flag for all events,
       and DDKSdmaGet/SetBufDesc index BDs at 12*idx. */
    uint32_t BdStride(uint32_t) const override { return 12u; }

    bool ReadExtra(uint32_t off, uint32_t& out) override {
        if (off == kOffEvtMirror) { out = 0; return true; }   /* no DMA-request events */
        return false;
    }

private:
    static constexpr uint32_t kOffEvtMirror   = 0x60u;
    static constexpr uint32_t kOffChnenblBase = 0x200u;
    static constexpr uint32_t kChnenblCount   = 48u;

    /* UART1 (COM1) and UART3 move serial data over SDMA. The CHNENBL index is the
       MCIMX51RM Table 3-3 event number: UART1 RX=18/TX=19, UART3 RX=43/TX=44. */
    void BindUarts() {
        emu_.Get<Imx51Uart1>().BindSdma(this, 19u, 18u);
        emu_.Get<Imx51Uart3>().BindSdma(this, 44u, 43u);
    }

    /* SSI audio ports, same table: SSI1 TX0=29/TX1=27/RX0=28/RX1=26,
       SSI2 TX0=25/TX1=23/RX0=24/RX1=22, SSI3 TX0=47/TX1=37/RX0=46/RX1=35.
       A bound TX drains as a byte sink so an unclaimed audio HSTART completes
       instead of halting (agent_docs/rules.md audio careful-stub); a bound RX
       arm completes only via SdmaRxDeliver. */
    void BindSsis() {
        auto& ssi1 = emu_.Get<Imx51Ssi1>();
        RegisterSdmaEvent(29u, &ssi1, true);
        RegisterSdmaEvent(27u, &ssi1, true);
        RegisterSdmaEvent(28u, &ssi1, false);
        RegisterSdmaEvent(26u, &ssi1, false);
        auto& ssi2 = emu_.Get<Imx51Ssi2>();
        RegisterSdmaEvent(25u, &ssi2, true);
        RegisterSdmaEvent(23u, &ssi2, true);
        RegisterSdmaEvent(24u, &ssi2, false);
        RegisterSdmaEvent(22u, &ssi2, false);
        auto& ssi3 = emu_.Get<Imx51Ssi3>();
        RegisterSdmaEvent(47u, &ssi3, true);
        RegisterSdmaEvent(37u, &ssi3, true);
        RegisterSdmaEvent(46u, &ssi3, false);
        RegisterSdmaEvent(35u, &ssi3, false);
    }
};
