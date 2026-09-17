#include "ford_sync2_entertainment_state.h"
#include "ford_sync2_ilp_channel.h"
#include "ford_sync2_ilp_signals.h"
#include "../board_context.h"
#include "../../core/cerf_emulator.h"
#include "../../core/log.h"
#include "../../state/state_stream.h"

namespace {
class FordSync2Entertainment : public Service {
public:
    using Service::Service;
    bool ShouldRegister() override {
        auto* board = emu_.TryGet<BoardContext>();
        return board && board->GetBoard() == Board::FordSyncGen2;
    }
    void OnReady() override {
        emu_.Get<FordSync2IlpSignals>().RegisterComposite({
            0x02000092, 0x02000093, 0x02000094, 0x02000095, 0x02000096,
            0x02000097, 0x02000098, 0x02000099, 0x0200009A}, false);
        emu_.Get<FordSync2IlpChannel>().RegisterDevice({
            "entertainment", 1,
            [this](bool force) { Refresh(force); }, {},
            [](uint32_t id) { return id >= 0x0200007E && id <= 0x02000081; },
            [this](const std::vector<FordSync2IlpChannel::Write>& writes) { return Apply(writes); },
            [this](StateWriter& w) { Save(w); },
            [this](StateReader& r) { Restore(r); },
            [this] { state_ = {}; pending_.clear(); }
        });
        Refresh(true);
    }
private:
    using Channel = FordSync2IlpChannel;
    using State = FordSync2EntertainmentState;
    State state_;
    std::vector<State::Batch> pending_;
    static constexpr uint32_t kFirst = 0x02000092;
    static constexpr size_t kMaxPending = 48;

    Channel::Result Apply(const std::vector<Channel::Write>& writes) {
        // VNISending!433AE050 sends one complete operation/system/source/priority tuple.
        if (writes.size() != 4) return Channel::Result::Invalid;
        std::array<uint8_t, 4> values{};
        unsigned seen = 0;
        for (const auto& write : writes) {
            if (write.id < 0x0200007E || write.id > 0x02000081 || write.value > 255)
                return Channel::Result::Invalid;
            const unsigned index = write.id - 0x0200007E;
            if (seen & (1u << index)) return Channel::Result::Invalid;
            seen |= 1u << index;
            values[index] = static_cast<uint8_t>(write.value);
        }
        if (values[0] > 14 || values[1] > 15 || values[2] > 1 || values[3] > 5)
            return Channel::Result::Invalid;
        State next = state_;
        auto replies = next.Apply({values[3], values[2], values[1], values[0]});
        if (pending_.size() + replies.size() > kMaxPending) return Channel::Result::Unavailable;
        state_ = next;
        pending_.insert(pending_.end(), replies.begin(), replies.end());
        LOG(Board, "[AHU] operation=%u system=%u source=%u priority=%u replies=%zu\n",
            values[3], values[2], values[1], values[0], replies.size());
        return Channel::Result::Accepted;
    }
    void Report(const State::Batch& batch) {
        auto& signals = emu_.Get<FordSync2IlpSignals>();
        for (size_t i = 0; i < batch.size(); ++i) signals.SetReportedValue(kFirst + i, batch[i]);
    }
    void Refresh(bool force) {
        auto& signals = emu_.Get<FordSync2IlpSignals>();
        if (force) Report(state_.Current());
        // Do not flush a partial composite while the guest is still registering its filters.
        for (uint32_t id = kFirst; id <= 0x0200009A; ++id)
            if (signals.SubscriberCount(id) == 0) return;
        for (const auto& batch : pending_) {
            Report(batch);
            // Each call completes a distinct nine-field frame. Coalescing loses acceptance.
            emu_.Get<Channel>().PublishPending();
        }
        pending_.clear();
    }
    void Save(StateWriter& w) const {
        w.Write(state_.source); w.Write(state_.priority);
        w.Write<uint32_t>(static_cast<uint32_t>(pending_.size()));
        for (const auto& batch : pending_) w.WriteBytes(batch.data(), batch.size());
    }
    void Restore(StateReader& r) {
        r.Read(state_.source); r.Read(state_.priority);
        uint32_t count = 0; r.Read(count);
        if (!r.Ok() || count > kMaxPending ||
            !(State::Supported(state_.source, state_.priority) ||
              (state_.source == 12 && state_.priority == 14))) {
            LOG(Caution, "[AHU] invalid entertainment snapshot\n");
            CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
        }
        pending_.resize(count);
        for (auto& batch : pending_) {
            r.ReadBytes(batch.data(), batch.size());
            if (!r.Ok() || batch[0] > 14 || batch[1] > 15 || batch[2] > 1 || batch[3] > 5 ||
                batch[4] > 14 || batch[5] > 15 || batch[6] > 1 || batch[7] > 5 ||
                batch[8] < 1 || batch[8] > 4) {
                LOG(Caution, "[AHU] invalid pending reply in entertainment snapshot\n");
                CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
            }
        }
    }
};
REGISTER_SERVICE(FordSync2Entertainment);
}
