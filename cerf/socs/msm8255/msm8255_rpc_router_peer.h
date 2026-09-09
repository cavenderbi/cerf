#pragma once

#include "../../core/service.h"

#include <cstdint>

class Msm8255RpcRouterPeer : public Service {
public:
    using Service::Service;

    bool ShouldRegister() override;

    uint32_t Answer(uint32_t in_pa, uint32_t in_avail, uint32_t out_pa,
                    uint32_t out_cap, uint32_t& consumed);

private:
    void WriteCtrlMsg(uint32_t out_pa, uint32_t cmd, uint32_t self_pid,
                      uint32_t peer_pid, uint32_t prog, uint32_t vers,
                      uint32_t srv_pid, uint32_t srv_cid);
};
