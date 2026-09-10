#pragma once

#include "../../core/service.h"

#include <cstdint>

class Msm8255OncrpcCodec : public Service {
public:
    using Service::Service;

    bool ShouldRegister() override;

    uint32_t SkipXdrString(uint32_t body, uint32_t size, uint32_t off,
                           uint32_t which);
    uint32_t WriteAcceptedReply(uint32_t out_pa, uint32_t self_pid,
                                uint32_t src_cid, uint32_t peer_pid,
                                uint32_t peer_cid, uint32_t xid,
                                const uint32_t* results,
                                uint32_t result_words);
};
