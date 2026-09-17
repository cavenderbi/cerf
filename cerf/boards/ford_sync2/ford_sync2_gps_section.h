#pragma once

#include "../../host/host_widget.h"
#include <cstdint>
#include <string>
#include <vector>

class CerfEmulator;

class GpsSection : public HostMenuSection {
public:
    explicit GpsSection(CerfEmulator& emu) : emu_(emu) {}

    std::wstring Label() const override;
    std::vector<WidgetMenuItem> BuildItems() override;
    bool PollDirty() override;

    /* Tooltip line only: coordinates plus the GPS-derived UTC timestamp. */
    std::wstring TooltipText() const;

private:
    void EditCoordinates();
    static INT_PTR CALLBACK CoordinateDialog(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
    static std::wstring Degrees(int32_t value);
    static bool ParseDegrees(HWND dlg, int id, double& value);

    CerfEmulator& emu_;
    uint64_t last_revision_ = UINT64_MAX;
};
