#pragma once

#include "../../core/service.h"
#include "ford_sync2_ilp_channel.h"
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

class StateReader;
class StateWriter;

class FordSync2ClimateController : public Service {
public:
    using Service::Service;
    enum class Field : uint8_t {
        DriverTemperature, PassengerTemperature, RearLeftTemperature, RearRightTemperature,
        DriverTemperatureOverride, Power, Ac, MaxAc, Auto, Recirculation, Dual, MyTemp,
        FrontDefrost, RearDefrost, DriverSeat, PassengerSeat, SteeringHeat, RearControls,
        Blower, BlowerOverride, RearHeatBars, RearFanBars, AutoFan, AutoMode,
        FloorWindscreen, Floor, PanelFloor, Panel, RearPower, RearAuto, RearCoolBars,
        RearNeutralBar, Count
    };
    enum class Command : uint8_t {
        DriverTemperature, FrontButton, SeatButton, SteeringButton, RearTemperatureButton,
        RearFanButton, RearButton, RearFanBars
    };
    struct Request { Command command; uint64_t value; };
    using Result = FordSync2IlpChannel::Result;
    static constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);
    using Values = std::array<std::optional<uint8_t>, kFieldCount>;
    struct Snapshot { Values values; uint64_t revision; };

    bool ShouldRegister() override;
    void OnReady() override;
    Snapshot Read() const;
    void Set(Field field, std::optional<uint8_t> value);
    void Clear();
    void SetComfortPreset();
    Result Apply(const std::vector<Request>& requests);
    void SaveState(StateWriter& w) const;
    void RestoreState(StateReader& r);
    static uint8_t MaxValue(Field field);

private:
    static Result ApplyOne(Values& values, const Request& request);
    static Result AdjustTemperature(Values& values, Field field, bool up);
    static Result Toggle(Values& values, Field field);
    static void SetValue(Values& values, Field field, std::optional<uint8_t> value);
    mutable std::mutex mutex_;
    Values values_{};
    uint64_t revision_ = 0;
};
