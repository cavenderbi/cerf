#include "../board_context.h"
#include "ford_sync2_media_status.h"
#include "../../core/cerf_emulator.h"
#include "../../core/service.h"
#include "../../host/host_icon_cache.h"
#include "../../host/host_widget.h"
#include "../../host/host_widget_registry.h"

namespace {

class FordSync2CarWidget : public Service, public HostWidget {
public:
    using Service::Service;

    bool ShouldRegister() override {
        auto* board = emu_.TryGet<BoardContext>();
        return board && board->GetBoard() == Board::FordSyncGen2;
    }

    void OnReady() override { emu_.Get<HostWidgetRegistry>().Register(this); }

    std::wstring WidgetName() const override { return L"Car"; }
    WidgetGroup Group() const override { return WidgetGroup::Indicator; }
    std::wstring Tooltip() const override {
        return L"SYNC 2 car controls\nUSB · " + emu_.Get<FordSync2MediaStatus>().ReadProgress().Text();
    }
    bool PollDirty() override {
        const auto current = Tooltip();
        if (current == last_tooltip_) return false;
        last_tooltip_ = current;
        return true;
    }
    bool PrimaryActionOpensMenu() const override { return true; }

    void DrawIcon(HDC dc, const RECT& box) const override {
        emu_.Get<HostIconCache>().DrawCentered(dc, box, L"ICON_CAR");
    }

    std::vector<WidgetMenuItem> BuildMenu() override {
        WidgetMenuItem entertainment;
        entertainment.label = L"Entertainment";
        WidgetMenuItem progress;
        progress.label = L"USB · " + emu_.Get<FordSync2MediaStatus>().ReadProgress().Text();
        progress.on_click = [this] {
            ShowFordSync2MediaProgress([this] { return emu_.Get<FordSync2MediaStatus>().ReadProgress(); });
        };
        entertainment.submenu.push_back(std::move(progress));
        return {
            Section(L"Climate"),
            Section(L"Outside temperature"),
            Section(L"Location"),
            std::move(entertainment),
        };
    }

private:
    std::wstring last_tooltip_;
    static WidgetMenuItem Section(const wchar_t* label) {
        WidgetMenuItem item;
        item.label = label;
        item.enabled = false;
        return item;
    }
};

REGISTER_SERVICE(FordSync2CarWidget);

}
