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
    void NotifySmd();
    void ServiceSmdChannel(uint32_t cid, uint32_t rec, uint32_t item);
    void ConsumeAppsSmdFlags(uint32_t cid, uint32_t rec, uint32_t apps_half_pa);
    bool ChannelNameIsDal(uint32_t rec);
    [[noreturn]] void HaltUnroutedSmdChannel(uint32_t cid, uint32_t rec);
    void ServiceSmdData(uint32_t cid, uint32_t rec, uint32_t apps_half_pa);
    void OpenModemSmdHalf(uint32_t modem_half_pa);
    void RunProcComm();
};
