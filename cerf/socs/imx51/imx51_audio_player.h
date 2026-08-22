#pragma once

#define NOMINMAX

#include "../../core/service.h"
#include "../../host/wave_out_sink.h"
#include "imx51_sdma.h"

#include <cstdint>
#include <mutex>
#include <vector>

/* Claims the SDMA channel armed on an SSI transmit DMA-request event and plays
   its buffer-descriptor ring on host waveOut, retiring one BD per played page so
   the guest's completion interrupt arrives at real playback pace. */
class Imx51AudioPlayer : public Service {
public:
    using Service::Service;

    bool ShouldRegister() override;
    void OnReady() override;
    void OnShutdown() override;

private:
    static constexpr int      kSlots   = 4;
    static constexpr uint32_t kMaxBds  = 32u;

    struct Slot {
        WAVEHDR              hdr{};
        std::vector<uint8_t> bytes;
        uint32_t             bd_pa    = 0;
        bool                 retire   = false;   /* false => silence page */
        bool                 in_flight = false;
    };

    bool OnChannelClaim(const Imx51Sdma::ChannelStart& s);
    void OnChannelStop(uint32_t channel);
    void OnThreadMessage(const MSG& msg);
    void StartStream();
    void StopStream();
    bool QueuePage();
    void OnPageDone(WAVEHDR* hdr);
    Slot* AllocSlot();

    WaveOutSink sink_;
    std::mutex  mtx_;

    bool     active_    = false;
    uint32_t channel_   = 0;
    uint32_t ssi_       = 0;
    uint32_t rate_hz_   = 0;
    uint32_t next_bd_   = 0;
    std::vector<uint32_t> bd_pas_;

    Slot slots_[kSlots];
};
