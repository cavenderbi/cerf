#include "ford_sync2_climate_section.h"

#include "ford_sync2_ilp_channel.h"
#include "../../core/cerf_emulator.h"
#include "../../state/state_stream.h"
#include <algorithm>
#include <array>
#include <cwchar>

using Climate = FordSync2ClimateController;
using Field = Climate::Field;

namespace {
/* EA5T-14D544-BA.sec, FEClimate.dll sub_40EB7C28 / sub_40EB9930. */
constexpr const wchar_t* kNames[] = {
    L"Driver setpoint", L"Passenger setpoint", L"Rear left setpoint", L"Rear right status (raw)",
    L"Driver setpoint override", L"Power", L"A/C", L"Max A/C", L"Auto", L"Recirculation",
    L"Dual zone", L"My Temp indicator", L"Front defrost", L"Rear defrost", L"Driver seat",
    L"Passenger seat", L"Steering heat", L"Rear controls", L"Fan speed", L"Fan override",
    L"Rear heat bars", L"Rear fan bars", L"Auto fan", L"Auto airflow", L"Floor / windshield",
    L"Floor", L"Panel / floor", L"Panel", L"Rear power", L"Rear auto", L"Rear cool bars", L"Rear neutral bar"
};
static_assert(std::size(kNames) == Climate::kFieldCount);
}

std::wstring ClimateSection::Describe(Field field, std::optional<uint8_t> value) {
    if (!value) return L"Not reported";
    const unsigned raw = *value;
    /* EA5T-14D544-BA.sec, FEClimate.dll sub_40EBABCC: setpoint display domains. */
    if (field <= Field::RearLeftTemperature || field == Field::DriverTemperatureOverride) {
        if (raw == 0 || raw > 0xB4) return L"Invalid (" + std::to_wstring(raw) + L")";
        if (raw <= 0x1E || (raw > 0x50 && raw <= 0x77)) return L"Low";
        if (raw >= 0xAB || (raw >= 0x3C && raw <= 0x50)) return L"High";
        wchar_t text[32];
        if (raw > 0x50) std::swprintf(text, std::size(text), L"%u °F", raw >> 1);
        else std::swprintf(text, std::size(text), L"%.1f °C", raw * 0.5);
        return text;
    }
    /* Ford Climate.swf, ClimateBaseView: seat and button status encodings. */
    if (field == Field::DriverSeat || field == Field::PassengerSeat) {
        if (raw == 7) return L"Disabled";
        if (raw > 3) return L"Cool " + std::to_wstring(raw - 3);
        if (raw > 0) return L"Heat " + std::to_wstring(raw);
        return L"Off";
    }
    if (Climate::MaxValue(field) <= 3) {
        if (raw == 0) return L"Off";
        if (raw == 1) return L"On";
        return raw == 2 ? L"Disabled" : L"Reserved (3)";
    }
    return std::to_wstring(raw);
}

std::wstring ClimateSection::Label() const {
    const auto snapshot = emu_.Get<Climate>().Read();
    const auto power = snapshot.values[static_cast<std::size_t>(Field::Power)];
    const bool ac = power == 1 && (snapshot.values[static_cast<std::size_t>(Field::Ac)] == 1 ||
        snapshot.values[static_cast<std::size_t>(Field::MaxAc)] == 1);
    return L"Climate: power " + Describe(Field::Power, power) + (ac ? L", A/C enabled" : L"") +
           L"; driver " + Describe(Field::DriverTemperature, snapshot.values[0]) +
           L", passenger " + Describe(Field::PassengerTemperature, snapshot.values[1]);
}

WidgetMenuItem ClimateSection::ValueItem(Field field, uint8_t value, const std::wstring& label) {
    WidgetMenuItem item;
    item.label = label;
    item.checked = emu_.Get<Climate>().Read().values[static_cast<std::size_t>(field)] == value;
    item.on_click = [this, field, value] {
        emu_.Get<FordSync2IlpChannel>().ApplyHostChange([&] { emu_.Get<Climate>().Set(field, value); });
    };
    return item;
}

std::vector<WidgetMenuItem> ClimateSection::RawMenu(Field field) {
    std::vector<WidgetMenuItem> items;
    const unsigned max = Climate::MaxValue(field);
    for (unsigned start = 0; start <= max; start += 16) {
        WidgetMenuItem group;
        const unsigned end = std::min(start + 15, max);
        group.label = std::to_wstring(start) + L" to " + std::to_wstring(end);
        for (unsigned raw = start; raw <= end; ++raw)
            group.submenu.push_back(ValueItem(field, static_cast<uint8_t>(raw), std::to_wstring(raw)));
        if (max < 16) return group.submenu;
        items.push_back(std::move(group));
    }
    return items;
}

std::vector<WidgetMenuItem> ClimateSection::FieldMenu(Field field) {
    std::vector<WidgetMenuItem> items;
    WidgetMenuItem none;
    none.label = L"Not reported";
    none.checked = !emu_.Get<Climate>().Read().values[static_cast<std::size_t>(field)];
    none.on_click = [this, field] {
        emu_.Get<FordSync2IlpChannel>().ApplyHostChange([&] { emu_.Get<Climate>().Set(field, std::nullopt); });
    };
    items.push_back(std::move(none));
    if (field <= Field::RearLeftTemperature || field == Field::DriverTemperatureOverride) {
        const unsigned low = imperial_ ? 0x77 : 0x1E, high = imperial_ ? 0xAB : 0x3C;
        items.push_back(ValueItem(field, static_cast<uint8_t>(low), L"Low"));
        for (unsigned raw = low + 1; raw < high; raw += imperial_ ? 2 : 1)
            items.push_back(ValueItem(field, static_cast<uint8_t>(raw), Describe(field, static_cast<uint8_t>(raw))));
        items.push_back(ValueItem(field, static_cast<uint8_t>(high), L"High"));
    } else if (field == Field::RearRightTemperature) {
        auto raw = RawMenu(field);
        items.insert(items.end(), raw.begin(), raw.end());
        return items;
    } else {
        unsigned max = Climate::MaxValue(field);
        if (field == Field::Blower || field == Field::BlowerOverride) max = 7;
        if (field == Field::RearHeatBars || field == Field::RearCoolBars) max = 4;
        if (max == 3) max = 2;
        for (unsigned raw = 0; raw <= max; ++raw)
            items.push_back(ValueItem(field, static_cast<uint8_t>(raw), Describe(field, static_cast<uint8_t>(raw))));
    }
    WidgetMenuItem raw;
    raw.label = L"Raw status override";
    raw.submenu = RawMenu(field);
    items.push_back(std::move(raw));
    return items;
}

std::vector<WidgetMenuItem> ClimateSection::BuildItems() {
    std::vector<WidgetMenuItem> items;
    WidgetMenuItem preset;
    preset.label = L"Reset climate: all zones 21 °C, power on";
    preset.on_click = [this] {
        emu_.Get<FordSync2IlpChannel>().ApplyHostChange([&] { emu_.Get<Climate>().SetComfortPreset(); });
    };
    items.push_back(std::move(preset));
    const auto snapshot = emu_.Get<Climate>().Read();
    std::array<WidgetMenuItem, 4> groups;
    groups[0].label = L"Setpoints";
    groups[1].label = L"Front climate";
    groups[2].label = L"Seats and steering";
    groups[3].label = L"Rear climate";
    for (std::size_t i = 0; i < Climate::kFieldCount; ++i) {
        const auto field = static_cast<Field>(i);
        WidgetMenuItem item;
        item.label = std::wstring(kNames[i]) + L": " + Describe(field, snapshot.values[i]);
        item.submenu = FieldMenu(field);
        const unsigned group = i <= 4 ? 0 : i <= 13 ? 1 : i <= 16 ? 2 :
            (i == 17 || i == 20 || i == 21 || i >= 28) ? 3 : 1;
        groups[group].submenu.push_back(std::move(item));
    }
    for (auto& group : groups) items.push_back(std::move(group));
    WidgetMenuItem units;
    units.label = L"Set temperatures in °F";
    units.checked = imperial_;
    units.on_click = [this] { imperial_ = !imperial_; };
    items.push_back(std::move(units));
    WidgetMenuItem reset;
    reset.label = L"Stop reporting climate";
    reset.on_click = [this] {
        emu_.Get<FordSync2IlpChannel>().ApplyHostChange([&] { emu_.Get<Climate>().Clear(); });
    };
    items.push_back(std::move(reset));
    return items;
}

bool ClimateSection::PollDirty() {
    const auto revision = emu_.Get<Climate>().Read().revision;
    if (revision == last_revision_) return false;
    last_revision_ = revision;
    return true;
}

void ClimateSection::SaveState(StateWriter& w) const { w.Write<uint8_t>(imperial_); }
void ClimateSection::RestoreState(StateReader& r) {
    uint8_t imperial = 0; r.Read(imperial); imperial_ = imperial != 0;
}
