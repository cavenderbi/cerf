#include "ford_sync2_climate_controller.h"
#include "ford_sync2_ilp_channel.h"
#include "ford_sync2_ilp_signals.h"
#include "../board_context.h"
#include "../../core/cerf_emulator.h"

namespace {
class FordSync2ClimateIlp : public Service {
public:
    using Service::Service;
    bool ShouldRegister() override {
        auto* board = emu_.TryGet<BoardContext>();
        return board && board->GetBoard() == Board::FordSyncGen2;
    }
    void OnReady() override {
        emu_.Get<FordSync2ClimateController>();
        emu_.Get<FordSync2IlpChannel>().RegisterDevice({
            "climate", 1,
            [this](bool force) { Refresh(force); },
            {},
            [](uint32_t id) { return MapCommand(id).has_value(); },
            [this](const std::vector<FordSync2IlpChannel::Write>& writes) { return Apply(writes); },
            [this](StateWriter& w) { emu_.Get<FordSync2ClimateController>().SaveState(w); },
            [this](StateReader& r) { emu_.Get<FordSync2ClimateController>().RestoreState(r); },
            [this] { emu_.Get<FordSync2ClimateController>().SetComfortPreset(); }
        });
        Refresh(true);
    }
private:
    uint64_t revision_ = UINT64_MAX;
    using Climate = FordSync2ClimateController;
    using Command = Climate::Command;
    /* EA5T-14D544-BA.sec, VNISending.dll sub_433AACDC, sub_433AADB0,
       sub_433A57A4, sub_433A58FC..sub_433A5A50. */
    static std::optional<Command> MapCommand(uint32_t id) {
        Command command{};
        switch (id) {
        case 0x020001DC: command = Command::SteeringButton; break;
        case 0x020001DD: command = Command::DriverTemperature; break;
        case 0x020001DE: command = Command::SeatButton; break;
        case 0x020001E0: command = Command::RearTemperatureButton; break;
        case 0x020001E2: command = Command::RearFanButton; break;
        case 0x020001E3: command = Command::RearButton; break;
        case 0x020001E4: command = Command::RearFanBars; break;
        case 0x020001E5: command = Command::FrontButton; break;
        default: return std::nullopt;
        }
        return command;
    }
    FordSync2IlpChannel::Result Apply(const std::vector<FordSync2IlpChannel::Write>& writes) {
        std::vector<Climate::Request> requests;
        for (const auto& write : writes) {
            const auto command = MapCommand(write.id);
            if (!command) return FordSync2IlpChannel::Result::Unsupported;
            requests.push_back({*command, write.value});
        }
        return emu_.Get<Climate>().Apply(requests);
    }
    void Refresh(bool force) {
        auto& signals = emu_.Get<FordSync2IlpSignals>();
        const auto climate = emu_.Get<FordSync2ClimateController>().Read();
        if (force || climate.revision != revision_) {
            /* EA5T-14D544-BA.sec, VNIClimateSvc.dll sub_C15366B0;
               FEClimate.dll sub_40EB7C28, sub_40EB9930. */
            constexpr uint32_t ids[] = {
                0x020001BE, 0x020001BD, 0x020001BC, 0x020001BB, 0x020001DB,
                0x020001F9, 0x020001F4, 0x020001EE, 0x020001F8, 0x020001ED,
                0x020001F7, 0x020001EC, 0x020001FA, 0x020001EB, 0x020001E9,
                0x020001E8, 0x020001EA, 0x02000201, 0x020001C8, 0x020001DA,
                0x020001EF, 0x020001F1, 0x020001FB, 0x020001FC, 0x020001FD,
                0x020001FE, 0x020001FF, 0x02000200, 0x020001F0, 0x020001F3,
                0x020001F6, 0x020001F5
            };
            static_assert(std::size(ids) == FordSync2ClimateController::kFieldCount);
            for (std::size_t i = 0; i < std::size(ids); ++i) {
                if (climate.values[i]) signals.SetReportedValue(ids[i], *climate.values[i]);
                else signals.ClearReportedValue(ids[i]);
            }
            revision_ = climate.revision;
        }
    }
};
REGISTER_SERVICE(FordSync2ClimateIlp);
}
