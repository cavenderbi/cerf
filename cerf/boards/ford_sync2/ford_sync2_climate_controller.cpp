#include "ford_sync2_climate_controller.h"
#include "../board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../state/state_stream.h"
#include <algorithm>

REGISTER_SERVICE(FordSync2ClimateController);

bool FordSync2ClimateController::ShouldRegister() {
    auto* board = emu_.TryGet<BoardContext>();
    return board && board->GetBoard() == Board::FordSyncGen2;
}

void FordSync2ClimateController::OnReady() {
    SetComfortPreset();
}

FordSync2ClimateController::Snapshot FordSync2ClimateController::Read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {values_, revision_};
}

uint8_t FordSync2ClimateController::MaxValue(Field field) {
    /* EA5T-14D544-BA.sec, SyncGen2.bcf section 5; FEClimate.dll sub_40EB7C28. */
    if (field <= Field::DriverTemperatureOverride) return 0xFF;
    switch (field) {
    case Field::Ac: case Field::Recirculation: case Field::SteeringHeat:
    case Field::RearControls: return 3;
    case Field::DriverSeat: case Field::PassengerSeat: case Field::RearHeatBars:
    case Field::RearFanBars: case Field::RearCoolBars: return 7;
    case Field::Blower: return 63;
    case Field::BlowerOverride: return 31;
    default: return 1;
    }
}

void FordSync2ClimateController::SetValue(Values& values, Field field,
                                         std::optional<uint8_t> value) {
    values[static_cast<std::size_t>(field)] = value;
    if (field == Field::Auto) {
        values[static_cast<std::size_t>(Field::AutoFan)] = value;
        values[static_cast<std::size_t>(Field::AutoMode)] = value;
    }
}

void FordSync2ClimateController::Set(Field field, std::optional<uint8_t> value) {
    if (field >= Field::Count || (value && *value > MaxValue(field))) return;
    std::lock_guard<std::mutex> lock(mutex_);
    SetValue(values_, field, value);
    ++revision_;
}

void FordSync2ClimateController::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    values_ = {};
    ++revision_;
}

void FordSync2ClimateController::SetComfortPreset() {
    std::lock_guard<std::mutex> lock(mutex_);
    values_ = {};
    for (std::size_t i = static_cast<std::size_t>(Field::Power); i < kFieldCount; ++i)
        values_[i] = uint8_t{0};
    /* EA5T-14D544-BA.sec, FEClimate.dll sub_40EBABCC: Celsius setpoint encoding. */
    values_[static_cast<std::size_t>(Field::DriverTemperature)] = uint8_t{42};
    values_[static_cast<std::size_t>(Field::PassengerTemperature)] = uint8_t{42};
    values_[static_cast<std::size_t>(Field::RearLeftTemperature)] = uint8_t{42};
    values_[static_cast<std::size_t>(Field::RearRightTemperature)] = uint8_t{42};
    values_[static_cast<std::size_t>(Field::Power)] = uint8_t{1};
    values_[static_cast<std::size_t>(Field::Blower)] = uint8_t{1};
    values_[static_cast<std::size_t>(Field::BlowerOverride)].reset();
    ++revision_;
}

FordSync2ClimateController::Result FordSync2ClimateController::Toggle(Values& v, Field f) {
    auto& value = v[static_cast<std::size_t>(f)];
    if (!value || *value > 1) return Result::Unavailable;
    SetValue(v, f, static_cast<uint8_t>(*value == 0 ? 1 : 0));
    return Result::Accepted;
}

FordSync2ClimateController::Result FordSync2ClimateController::AdjustTemperature(
    Values& v, Field f, bool up) {
    auto& value = v[static_cast<std::size_t>(f)];
    if (!value) return Result::Unavailable;
    /* EA5T-14D544-BA.sec, FEClimate.dll sub_40EBABCC, sub_40EBA8F4, sub_40EBA914. */
    const unsigned raw = *value;
    if (raw < 1 || raw > 0xB4) return Result::Unavailable;
    const bool fahrenheit = raw > 0x50;
    const unsigned low = fahrenheit ? 0x77 : 0x1E;
    const unsigned high = fahrenheit ? 0xAB : 0x3C;
    const unsigned first = fahrenheit ? 0x78 : 0x1F;
    const unsigned last = fahrenheit ? 0xAA : 0x3B;
    const unsigned step = fahrenheit ? 2 : 1;
    unsigned next = raw;
    if (up) next = raw < first ? first : raw >= last ? high : std::min(raw + step, last);
    else next = raw > last ? last : raw <= first ? low : std::max(raw - step, first);
    if ((!up && raw <= low) || (up && raw >= high)) next = up ? high : low;
    value = static_cast<uint8_t>(next);
    return Result::Accepted;
}

FordSync2ClimateController::Result FordSync2ClimateController::ApplyOne(
    Values& v, const Request& request) {
    const uint64_t value = request.value;
    const auto index = [](Field f) { return static_cast<std::size_t>(f); };
    /* EA5T-14D544-BA.sec, VNISending.dll sub_433AACDC, sub_433AADB0. */
    if (request.command == Command::DriverTemperature) {
        if (value == 0xFE) return Result::Accepted;
        if (value < 1 || value > 0xB4) return Result::Invalid;
        v[index(Field::DriverTemperature)] = static_cast<uint8_t>(value);
        v[index(Field::DriverTemperatureOverride)].reset();
        if (v[index(Field::Dual)] == 0) v[index(Field::PassengerTemperature)] = static_cast<uint8_t>(value);
        return Result::Accepted;
    }
    if (request.command == Command::RearFanBars) {
        if (value > 7) return Result::Invalid;
        v[index(Field::RearFanBars)] = static_cast<uint8_t>(value);
        return Result::Accepted;
    }
    if (value == 0) return Result::Accepted;
    /* Ford Climate.swf, Plugins.ClimatePluginConst; ClimateControl_CGEA_1_2.SendEvents.
       EA5T-14D544-BA.sec, FEClimate.dll sub_40EBAF48: front temperature and fan directions. */
    if (request.command == Command::SteeringButton)
        return value == 1 ? Toggle(v, Field::SteeringHeat) : Result::Invalid;
    if (request.command == Command::RearButton) {
        if (value == 1) return Toggle(v, Field::RearPower);
        if (value == 2) return Toggle(v, Field::RearControls);
        if (value == 3) return Toggle(v, Field::RearAuto);
        return Result::Invalid;
    }
    if (request.command == Command::RearTemperatureButton) {
        if (value > 2) return Result::Invalid;
        return AdjustTemperature(v, Field::RearLeftTemperature, value == 1);
    }
    if (request.command == Command::SeatButton) {
        if (value > 4) return Result::Invalid;
        auto& seat = v[index(value <= 2 ? Field::DriverSeat : Field::PassengerSeat)];
        if (!seat || *seat > 6) return Result::Unavailable;
        /* Ford Climate.swf, ClimateBaseView: seat status levels; 2010 MKS manual p.141,
           https://www.fordservicecontent.com/Ford_Content/catalog/owner_guides/10mksog1e.pdf */
        const uint8_t first = (value == 2 || value == 4) ? 4 : 1;
        seat = static_cast<uint8_t>(*seat < first || *seat > first + 2 ? first + 2 : *seat == first ? 0 : *seat - 1);
        return Result::Accepted;
    }
    Field fan = Field::RearFanBars;
    bool increase = value == 1;
    if (request.command == Command::FrontButton) {
        switch (value) {
        case 1: case 2: case 3: case 4:
            /* Ford Climate.swf, ClimatePluginConst; 2013 Flex manual pp.519-520,
               https://www.fordservicecontent.com/Ford_Content/catalog/owner_guides/13flxom1e.pdf */
            v[index(Field::Panel)] = value == 1;
            v[index(Field::PanelFloor)] = value == 2;
            v[index(Field::Floor)] = value == 3;
            v[index(Field::FloorWindscreen)] = value == 4;
            v[index(Field::AutoMode)] = uint8_t{0};
            v[index(Field::Auto)] = uint8_t{0};
            return Result::Accepted;
        case 5: return Toggle(v, Field::Ac);
        case 6: {
            const auto result = Toggle(v, Field::Dual);
            if (result == Result::Accepted && v[index(Field::Dual)] == 0)
                v[index(Field::PassengerTemperature)] = v[index(Field::DriverTemperature)];
            return result;
        }
        case 7: case 8: case 9: case 10: {
            Field field = value <= 8 ? Field::DriverTemperature : Field::PassengerTemperature;
            const auto result = AdjustTemperature(v, field, value == 7 || value == 9);
            if (result != Result::Accepted) return result;
            if (field == Field::DriverTemperature) {
                v[index(Field::DriverTemperatureOverride)].reset();
                if (v[index(Field::Dual)] == 0) v[index(Field::PassengerTemperature)] = v[index(field)];
            }
            return result;
        }
        case 11: case 12: fan = Field::Blower; increase = value == 11; break;
        case 13: return Toggle(v, Field::Auto);
        case 14: return Toggle(v, Field::MaxAc);
        case 15: return Toggle(v, Field::Recirculation);
        case 16: return Toggle(v, Field::FrontDefrost);
        case 17: return Toggle(v, Field::RearDefrost);
        case 18: return Toggle(v, Field::Power);
        default: return Result::Unsupported;
        }
    } else if (request.command != Command::RearFanButton || value > 2) return Result::Invalid;
    auto& speed = v[index(fan)];
    /* Ford Climate.swf, ClimateBaseView.Front_Blower_Req / Rear_Fan_Bars_Displayed. */
    if (!speed || *speed > 7) return Result::Unavailable;
    speed = static_cast<uint8_t>(increase ? std::min<unsigned>(*speed + 1, 7) : *speed == 0 ? 0 : *speed - 1);
    if (fan == Field::Blower) v[index(Field::BlowerOverride)].reset();
    return Result::Accepted;
}

FordSync2ClimateController::Result FordSync2ClimateController::Apply(const std::vector<Request>& requests) {
    std::lock_guard<std::mutex> lock(mutex_);
    Values candidate = values_;
    for (const auto& request : requests) {
        const auto result = ApplyOne(candidate, request);
        if (result != Result::Accepted) return result;
    }
    if (candidate != values_) { values_ = candidate; ++revision_; }
    return Result::Accepted;
}

void FordSync2ClimateController::SaveState(StateWriter& w) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto value : values_) {
        w.Write<uint8_t>(value.has_value());
        w.Write<uint8_t>(value.value_or(0));
    }
}

void FordSync2ClimateController::RestoreState(StateReader& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& value : values_) {
        uint8_t present = 0, raw = 0;
        r.Read(present); r.Read(raw);
        value = present ? std::optional<uint8_t>(raw) : std::nullopt;
    }
    ++revision_;
}
