#pragma once

#include "../../core/service.h"

#include <cstdint>

class Msm8255RpcRouterPeer : public Service {
public:
    using Service::Service;

    bool ShouldRegister() override;

    uint32_t Answer(uint32_t in_pa, uint32_t in_avail, uint32_t out_pa,
                    uint32_t out_cap, uint32_t& consumed);
};
