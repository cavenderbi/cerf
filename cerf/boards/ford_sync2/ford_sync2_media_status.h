#pragma once
#include "ford_sync2_ilp_channel.h"
#include "ford_sync2_media_progress.h"
#include <array>
#include <mutex>

class FordSync2MediaStatus : public Service {
public:
    using Service::Service;
    bool ShouldRegister() override;
    void OnReady() override;
    FordSync2MediaProgress ReadProgress() const;
private:
    using Channel = FordSync2IlpChannel;
    static constexpr uint32_t kFirst = 0x02000212;
    struct Status { std::array<uint32_t, 4> values{}; uint8_t valid = 0; };
    mutable std::mutex mutex_;
    Status status_;
    Status Snapshot() const { std::lock_guard lock(mutex_); return status_; }
    Channel::Result Apply(const std::vector<Channel::Write>& writes);
};
