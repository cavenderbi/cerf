#pragma once

#include "network_backend.h"

class NullNetworkBackend : public NetworkBackend {
public:
    using NetworkBackend::NetworkBackend;

    bool ShouldRegister() override;

    void SendFrame(const uint8_t* frame, std::size_t len) override;
    std::array<uint8_t, 6> GuestMacAddress() const override;
    std::array<uint8_t, 6> HostGatewayMacAddress() const override;

private:
    std::array<uint8_t, 6> guest_mac_{};
    bool tx_logged_once_ = false;
};
