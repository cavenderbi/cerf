#include "ford_sync2_gps_source.h"
#include "../board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../state/state_stream.h"
#include <cmath>
#include <ctime>
#include <tuple>

REGISTER_SERVICE(FordSync2GpsSource);

bool FordSync2GpsSource::ShouldRegister() {
    auto* board = emu_.TryGet<BoardContext>();
    return board && board->GetBoard() == Board::FordSyncGen2;
}

void FordSync2GpsSource::OnReady() {
    /* https://github.com/cavenderbi/cerf/wiki/SYNC-2-component-split-and-command-handling#automatic-location-and-coordinate-control */
    SetDearborn();
}

FordSync2GpsSource::Snapshot FordSync2GpsSource::Read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {fix_, revision_};
}

void FordSync2GpsSource::SetDearborn() {
    std::lock_guard<std::mutex> lock(mutex_);
    /* https://www.openstreetmap.org/way/59150914 (Ford Glass House).
       https://www.fromtheroad.ford.com/us/en/articles/2025/the-many-homes-of-ford-on-the-american-road
       EA5T-14D544-BA.sec, VNIGeneralSvc.dll sub_C1597CEC, sub_C15980E0, sub_C1598504. */
    fix_ = Fix{kDearbornLatitudeE7, kDearbornLongitudeE7, 0, 18000, 0, 0, 0, 0, 0, 1};
    SyncClockLocked();
    ++revision_;
}

bool FordSync2GpsSource::SetCoordinates(double latitude, double longitude) {
    /* EA5T-14D544-BA.sec, VNIGeneralSvc.dll sub_C1598F60: signed degrees
       -89..89 / -179..179 and minutes below 60; hemisphere flags cover zero degrees. */
    if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
        std::abs(latitude) >= 90.0 || std::abs(longitude) >= 180.0) return false;
    const auto lat = static_cast<int32_t>(std::llround(latitude * 10000000.0));
    const auto lon = static_cast<int32_t>(std::llround(longitude * 10000000.0));
    if (std::abs(lat) >= 900000000 || std::abs(lon) >= 1800000000) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fix_) fix_ = Fix{lat, lon, 0, 18000, 0, 0, 0, 0, 0, 1};
    fix_->latitude_e7 = lat;
    fix_->longitude_e7 = lon;
    SyncClockLocked();
    ++revision_;
    return true;
}

void FordSync2GpsSource::SyncClockLocked() {
    if (!fix_) return;
    const auto now = std::time(nullptr);
    std::tm utc{};
    if (gmtime_s(&utc, &now) != 0) return;
    auto& f = *fix_;
    auto fields = std::tie(f.year, f.month, f.day, f.hour, f.minute, f.second);
    const auto previous = std::make_tuple(f.year, f.month, f.day, f.hour, f.minute, f.second);
    fields = std::make_tuple(static_cast<uint16_t>(utc.tm_year + 1900),
        static_cast<uint8_t>(utc.tm_mon + 1), static_cast<uint8_t>(utc.tm_mday),
        static_cast<uint8_t>(utc.tm_hour), static_cast<uint8_t>(utc.tm_min),
        static_cast<uint8_t>(utc.tm_sec));
    if (fields != previous) ++revision_;
}

void FordSync2GpsSource::SyncClock() {
    std::lock_guard<std::mutex> lock(mutex_);
    SyncClockLocked();
}

void FordSync2GpsSource::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    fix_.reset();
    ++revision_;
}

void FordSync2GpsSource::SaveState(StateWriter& w) const {
    std::lock_guard<std::mutex> lock(mutex_);
    w.Write<uint8_t>(fix_.has_value());
    if (!fix_) return;
    const auto& f = *fix_;
    w.Write(f.latitude_e7); w.Write(f.longitude_e7); w.Write(f.year);
    w.Write(f.heading_hundredths); w.Write(f.month); w.Write(f.day);
    w.Write(f.hour); w.Write(f.minute); w.Write(f.second); w.Write(f.satellites);
}

void FordSync2GpsSource::RestoreState(StateReader& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t present = 0;
    r.Read(present);
    fix_.reset();
    if (present) {
        Fix f{};
        r.Read(f.latitude_e7); r.Read(f.longitude_e7); r.Read(f.year);
        r.Read(f.heading_hundredths); r.Read(f.month); r.Read(f.day);
        r.Read(f.hour); r.Read(f.minute); r.Read(f.second); r.Read(f.satellites);
        fix_ = f;
    }
    SyncClockLocked();
    ++revision_;
}
