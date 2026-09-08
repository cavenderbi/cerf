#pragma once

#include "../core/service.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

class NetworkBackend : public Service {
public:
    using Service::Service;

    using RxFn = std::function<void(const uint8_t* frame, std::size_t len)>;

    enum class ReceiverKind {
        Ethernet,
        PointToPoint,
    };

    virtual void SendFrame(const uint8_t* frame, std::size_t len) = 0;

    std::array<uint8_t, 6> AttachReceiver(const std::string& id, ReceiverKind kind,
                                          RxFn cb);
    void DetachReceiver(const std::string& id);

    std::array<uint8_t, 6> MacForReceiver(const std::string& id, ReceiverKind kind);

    virtual std::array<uint8_t, 6> GuestMacAddress() const = 0;
    virtual std::array<uint8_t, 6> HostGatewayMacAddress() const = 0;

protected:
    void DispatchFrame(const uint8_t* frame, std::size_t len);

private:
    struct Receiver {
        std::array<uint8_t, 6> mac{};
        RxFn                   cb;
    };

    std::array<uint8_t, 6> MacForReceiverLocked(const std::string& id,
                                                ReceiverKind kind);

    std::mutex                     rx_mutex_;
    std::map<std::string, uint8_t> ordinals_;
    std::map<std::string, Receiver> receivers_;
    bool                           configured_mac_taken_ = false;
    uint8_t                        next_ordinal_ = 1;
};
