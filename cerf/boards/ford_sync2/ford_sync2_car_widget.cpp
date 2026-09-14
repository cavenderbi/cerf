#include "../../core/service.h"
#include "../../host/host_widget.h"
#include <cstdint>
#include <string>
#include <vector>
#include "ford_sync2_climate_section.h"
#include "ford_sync2_temperature_section.h"
#include "../board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../host/host_icon_cache.h"
#include "../../host/host_widget_registry.h"
#include "../../state/state_stream.h"

namespace {

class FordSync2CarWidget : public Service, public HostWidget {
public:
    explicit FordSync2CarWidget(CerfEmulator& emu)
        : Service(emu), climate_(emu), temperature_(emu), sections_{&climate_, &temperature_} {}
    bool ShouldRegister() override;
    void OnReady() override;
    std::wstring WidgetName() const override { return L"Car"; }
    WidgetGroup Group() const override { return WidgetGroup::Indicator; }
    std::wstring Tooltip() const override;
    std::vector<WidgetMenuItem> BuildMenu() override;
    bool PrimaryActionOpensMenu() const override { return true; }
    void DrawIcon(HDC dc, const RECT& box) const override;
    bool PollDirty() override;
    void SaveWidgetState(StateWriter& w) const override;
    void RestoreWidgetState(StateReader& r) override;
private:
    ClimateSection climate_;
    TemperatureSection temperature_;
    std::vector<HostMenuSection*> sections_;
};

REGISTER_SERVICE(FordSync2CarWidget);

bool FordSync2CarWidget::ShouldRegister() {
    auto* board = emu_.TryGet<BoardContext>();
    return board && board->GetBoard() == Board::FordSyncGen2;
}

void FordSync2CarWidget::OnReady() { emu_.Get<HostWidgetRegistry>().Register(this); }

std::wstring FordSync2CarWidget::Tooltip() const {
    return L"SYNC 2 car controls\n" + climate_.Label() + L"\n" + temperature_.Label();
}

void FordSync2CarWidget::DrawIcon(HDC dc, const RECT& box) const {
    emu_.Get<HostIconCache>().DrawCentered(dc, box, L"ICON_CAR");
}

std::vector<WidgetMenuItem> FordSync2CarWidget::BuildMenu() {
    std::vector<WidgetMenuItem> items;
    for (auto* section : sections_) {
        WidgetMenuItem item;
        item.label = section->Label();
        item.submenu = section->BuildItems();
        items.push_back(std::move(item));
    }
    return items;
}
bool FordSync2CarWidget::PollDirty() {
    bool dirty = false;
    for (auto* section : sections_) dirty |= section->PollDirty();
    return dirty;
}
void FordSync2CarWidget::SaveWidgetState(StateWriter& w) const {
    climate_.SaveState(w);
    temperature_.SaveState(w);
}
void FordSync2CarWidget::RestoreWidgetState(StateReader& r) {
    climate_.RestoreState(r);
    temperature_.RestoreState(r);
}
}
