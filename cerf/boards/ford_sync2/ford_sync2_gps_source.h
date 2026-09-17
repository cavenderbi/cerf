#pragma once
#include "../../core/service.h"
#include <cstdint>
#include <mutex>
#include <optional>

class StateWriter;
class StateReader;

class FordSync2GpsSource : public Service {
public:
    using Service::Service;
    struct Fix {
        int32_t latitude_e7, longitude_e7;
        uint16_t year, heading_hundredths;
        uint8_t month, day, hour, minute, second, satellites;
    };
    struct Snapshot { std::optional<Fix> fix; uint64_t revision; };
    static constexpr int32_t kDearbornLatitudeE7 = 423154100;
    static constexpr int32_t kDearbornLongitudeE7 = -832104400;
    bool ShouldRegister() override;
    void OnReady() override;
    Snapshot Read() const;
    void SetDearborn();
    bool SetCoordinates(double latitude, double longitude);
    void SyncClock();
    void Clear();
    void SaveState(StateWriter& w) const;
    void RestoreState(StateReader& r);
private:
    void SyncClockLocked();
    mutable std::mutex mutex_;
    std::optional<Fix> fix_;
    uint64_t revision_ = 0;
};
