#include "ford_sync2_media_status.h"
#include "../board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../core/log.h"
#include "../../state/state_stream.h"

REGISTER_SERVICE(FordSync2MediaStatus);

bool FordSync2MediaStatus::ShouldRegister() {
    auto* board = emu_.TryGet<BoardContext>();
    return board && board->GetBoard() == Board::FordSyncGen2;
}

void FordSync2MediaStatus::OnReady() {
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
}

FordSync2MediaProgress FordSync2MediaStatus::ReadProgress() const {
    const auto s = Snapshot();
    return {FordSync2MediaProgress::Decode(s.values[0], (s.valid & 1) != 0),
        FordSync2MediaProgress::Decode(s.values[1], (s.valid & 2) != 0)};
}

FordSync2IlpChannel::Result FordSync2MediaStatus::Apply(const std::vector<Channel::Write>& writes) {
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
    status_ = next;
    return Channel::Result::Accepted;
}
