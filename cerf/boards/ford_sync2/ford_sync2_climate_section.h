#pragma once

#include "../../host/host_widget.h"
#include "ford_sync2_climate_controller.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class CerfEmulator;
class StateWriter;
class StateReader;

class ClimateSection : public HostMenuSection {
public:
    using Field = FordSync2ClimateController::Field;

    explicit ClimateSection(CerfEmulator& emu) : emu_(emu) {}

    std::wstring Label() const override;
    std::vector<WidgetMenuItem> BuildItems() override;
    bool PollDirty() override;
    void SaveState(StateWriter& w) const override;
    void RestoreState(StateReader& r) override;

private:
    WidgetMenuItem ValueItem(Field field, uint8_t value, const std::wstring& label);
    std::vector<WidgetMenuItem> FieldMenu(Field field);
    std::vector<WidgetMenuItem> RawMenu(Field field);
    static std::wstring Describe(Field field, std::optional<uint8_t> value);

    CerfEmulator& emu_;
    uint64_t last_revision_ = UINT64_MAX;
    bool imperial_ = false;
};
