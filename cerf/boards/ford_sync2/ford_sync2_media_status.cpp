#include "ford_sync2_ilp_channel.h"
#include "ford_sync2_media_progress.h"
#include "../board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../core/log.h"
#include "../../host/host_widget.h"
#include "../../host/host_widget_registry.h"
#include "../../host/host_icon_cache.h"
#include "../../state/state_stream.h"
#include <array>
#include <mutex>

namespace {
/* https://github.com/cavenderbi/cerf/wiki/SYNC-2-entertainment-ILP-gaps */
class FordSync2MediaStatus : public Service, public HostWidget {
public:
    using Service::Service;
    bool ShouldRegister() override {
        auto* board = emu_.TryGet<BoardContext>();
        return board && board->GetBoard() == Board::FordSyncGen2;
    }
    void OnReady() override {
        emu_.Get<FordSync2IlpChannel>().RegisterDevice({
            "media_status", 1, [](bool) {}, {},
            [](uint32_t id) { return id >= kFirst && id < kFirst + 4; },
            [this](const std::vector<Channel::Write>& writes) { return Apply(writes); },
            [this](StateWriter& w) {
                const auto s = Snapshot(); w.Write(s.valid);
                for (auto value : s.values) w.Write(value);
            },
            [this](StateReader& r) {
                Status s; r.Read(s.valid);
                for (auto& value : s.values) r.Read(value);
                if (!r.Ok() || s.valid > 15 || s.values[0] > 65535 ||
                    s.values[1] > 65535 || s.values[2] > 65535) {
                    LOG(Caution, "[AHU] invalid media-status snapshot\n");
                    CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
                }
                std::lock_guard lock(mutex_); status_ = s;
            },
            [this] { std::lock_guard lock(mutex_); status_ = {}; }
        });
        emu_.Get<HostWidgetRegistry>().Register(this);
    }
    std::wstring WidgetName() const override { return L"Media status"; }
    WidgetGroup Group() const override { return WidgetGroup::Indicator; }
    void OnPrimaryAction() override {
        ShowFordSync2MediaProgress([this] { return Progress(Snapshot()); });
    }
    void DrawIcon(HDC dc, const RECT& box) const override {
        const auto s = Snapshot();
        emu_.Get<HostIconCache>().DrawCentered(dc, box, s.valid == 0 ? L"ICON_MEDIA_WAITING" :
            s.valid == 15 ? L"ICON_MEDIA_READY" : L"ICON_MEDIA_PARTIAL");
    }
    std::wstring Tooltip() const override {
        const auto s = Snapshot();
        return L"USB · " + Progress(s).Text();
    }
    bool PollDirty() override {
        const auto s = Snapshot();
        auto text = L"USB · " + Progress(s).Text();
        if (text == last_tooltip_ && s.valid == last_valid_) return false;
        last_valid_ = s.valid; last_tooltip_ = std::move(text); return true;
    }
    std::vector<WidgetMenuItem> BuildMenu() override {
        const auto s = Snapshot();
        std::vector<WidgetMenuItem> items;
        auto add = [&](std::wstring text) {
            WidgetMenuItem item; item.label = std::move(text); item.enabled = false;
            items.push_back(std::move(item));
        };
        add(L"USB");
        add(Progress(s).Text());
        return items;
    }
private:
    using Channel = FordSync2IlpChannel;
    static constexpr uint32_t kFirst = 0x02000212;
    struct Status { std::array<uint32_t, 4> values{}; uint8_t valid = 0; };
    mutable std::mutex mutex_;
    Status status_;
    std::wstring last_tooltip_;
    uint8_t last_valid_ = 255;
    Status Snapshot() const { std::lock_guard lock(mutex_); return status_; }
    static FordSync2MediaProgress Progress(const Status& s) {
        return {FordSync2MediaProgress::Decode(s.values[0], (s.valid & 1) != 0),
            FordSync2MediaProgress::Decode(s.values[1], (s.valid & 2) != 0)};
    }
    Channel::Result Apply(const std::vector<Channel::Write>& writes) {
        if (writes.empty()) return Channel::Result::Invalid;
        std::lock_guard lock(mutex_);
        auto next = status_;
        unsigned seen = 0;
        for (const auto& w : writes) {
            if (w.id < kFirst || w.id >= kFirst + 4) return Channel::Result::Invalid;
            const auto index = w.id - kFirst;
            if ((seen & (1u << index)) || w.value > (index == 3 ? UINT32_MAX : 65535u))
                return Channel::Result::Invalid;
            seen |= 1u << index;
            next.values[index] = static_cast<uint32_t>(w.value);
            next.valid |= static_cast<uint8_t>(1u << index);
        }
        status_ = next; MarkRx();
        return Channel::Result::Accepted;
    }
};
REGISTER_SERVICE(FordSync2MediaStatus);
}
