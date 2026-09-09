#pragma once

#include "../../core/service.h"

#include <cstdint>

class Msm8255Smem : public Service {
public:
    using Service::Service;

    bool ShouldRegister() override;
    void OnReady() override;

    uint32_t SmemPa();
    uint32_t DynamicItemPa(uint32_t id);

private:
    void Seed();
    uint32_t RecordPa(uint32_t index) const;
    void SeedSpeedRecord();
    void SeedPerfLevels();
    void SeedAvsConfig();
    void PublishItem(uint32_t id, uint32_t off, uint32_t size, uint32_t magic);
};
