#include "ford_sync2_gps_source.h"
#include "ford_sync2_ilp_channel.h"
#include "ford_sync2_ilp_signals.h"
#include "../board_context.h"
#include "../../core/cerf_emulator.h"
#include <cstdlib>

namespace {
class FordSync2GpsIlp : public Service {
public:
    using Service::Service;
    bool ShouldRegister() override {
        auto* board = emu_.TryGet<BoardContext>();
        return board && board->GetBoard() == Board::FordSyncGen2;
    }
    void OnReady() override {
        emu_.Get<FordSync2GpsSource>();
        /* EA5T-14D544-BA.sec, VNIGeneralSvc.dll sub_C1598F60, sub_C1597CEC,
           sub_C15980E0, sub_C1598504: each consumer drains one coherent record queue. */
        auto& signals = emu_.Get<FordSync2IlpSignals>();
        signals.RegisterComposite({0, 1, 2, 3, 4, 5, 6, 7});
        signals.RegisterComposite({8, 9, 10});
        signals.RegisterComposite({14, 15, 17});
        signals.RegisterComposite({11, 12, 13, 18, 19, 20, 21, 22, 23, 24});
        /* https://github.com/cavenderbi/cerf/wiki/SYNC-2-ILP-stacked-PR-plan#persistence-boundary */
        emu_.Get<FordSync2IlpChannel>().RegisterDevice({
            "gps", 1,
            [this](bool force) { Refresh(force); },
            [this](uint32_t pets) {
                /* EA5T-14D544-BA.sec, SyncClockSvc.dll sub_40166F34: 30000 ms GPS expiry;
                   sub_4016A090 refreshes the timestamp only from a fresh VNI record queue. */
                if ((pets & 1u) == 0) emu_.Get<FordSync2IlpSignals>().RepublishComposites();
            },
            {},
            {},
            [this](StateWriter& w) { emu_.Get<FordSync2GpsSource>().SaveState(w); },
            [this](StateReader& r) { emu_.Get<FordSync2GpsSource>().RestoreState(r); },
            [this] { emu_.Get<FordSync2GpsSource>().SetDearborn(); }
        });
        Refresh(true);
    }
private:
    uint64_t revision_ = UINT64_MAX;
    void Refresh(bool force) {
        auto& signals = emu_.Get<FordSync2IlpSignals>();
        auto& gps_source = emu_.Get<FordSync2GpsSource>();
        gps_source.SyncClock();
        const auto gps = gps_source.Read();
        if (!force && gps.revision == revision_) return;
        revision_ = gps.revision;
        constexpr uint32_t gps_ids[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                        14, 15, 17, 18, 19, 20, 21, 22, 23, 24};
        if (!gps.fix) {
            for (auto id : gps_ids) signals.ClearReportedValue(id);
            return;
        }
        const auto& fix = *gps.fix;
        /* EA5T-14D544-BA.sec, VNIGeneralSvc.dll sub_C1598F60 (position),
           sub_C1597CEC (date), sub_C15980E0 (time), sub_C1598504 (tracking).
           GPS.dll sub_414C3E28 / sub_414C3ED0, compass table 414C81C8. */
        const int64_t lat_minutes = (std::abs(fix.latitude_e7) % 10000000) * 60LL;
        const int64_t lon_minutes = (std::abs(fix.longitude_e7) % 10000000) * 60LL;
        const uint32_t values[] = {
            static_cast<uint32_t>(lon_minutes % 10000000 / 1000),
            static_cast<uint32_t>(lon_minutes / 10000000),
            static_cast<uint32_t>(fix.longitude_e7 / 10000000 + 0xB3), fix.latitude_e7 < 0 ? 1u : 2u,
            static_cast<uint32_t>(lat_minutes % 10000000 / 1000), fix.longitude_e7 < 0 ? 2u : 1u,
            static_cast<uint32_t>(lat_minutes / 10000000),
            static_cast<uint32_t>(fix.latitude_e7 / 10000000 + 0x59),
            static_cast<uint32_t>(fix.year - 2010), static_cast<uint32_t>(fix.month - 1),
            static_cast<uint32_t>(fix.day - 1), 0, 4, 1, fix.second, fix.minute, fix.hour,
            1, 1, 0, fix.heading_hundredths, 2046, 0, fix.satellites
        };
        static_assert(std::size(gps_ids) == std::size(values));
        for (std::size_t i = 0; i < std::size(gps_ids); ++i)
            signals.SetReportedValue(gps_ids[i], values[i]);
    }
};
REGISTER_SERVICE(FordSync2GpsIlp);
}
