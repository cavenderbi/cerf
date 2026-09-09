#pragma once

#include "../../core/service.h"

#include <cstdint>

class Msm8255ModemPeer : public Service {
public:
    using Service::Service;

    bool ShouldRegister() override;
    void OnReady() override;

    void RingDoorbell(uint32_t mask);

private:
    void SeedProcCommReady();
    void PublishModemState();
    void RunProcComm();
};
