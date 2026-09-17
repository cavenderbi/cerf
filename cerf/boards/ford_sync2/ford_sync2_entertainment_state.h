#pragma once
#include <array>
#include <cstdint>
#include <vector>

/* Guest contract and source/priority enums:
   https://github.com/cavenderbi/cerf/wiki/SYNC-2-AHU-entertainment-arbitration */
class FordSync2EntertainmentState {
public:
    using Batch = std::array<uint8_t, 9>; // signals 0x02000092..0x0200009A
    struct Request { uint8_t operation, system, source, priority; };
    uint8_t source = 12, priority = 14;

    bool Active() const { return source != 12; }
    static bool Supported(uint8_t source, uint8_t priority) {
        return ((source == 0 || source == 2) && priority == 7) ||
               ((source == 1 || source == 3) && priority == 8) ||
               (source == 8 && priority == 11);
    }
    static Batch Status(uint8_t source, uint8_t priority, uint8_t status) {
        return {priority, source, 0, status, 14, 12, 0, 0, 4};
    }
    Batch Current() const { return Status(source, priority, Active() ? 4 : 1); }

    // One front entertainment owner. Competing/non-entertainment priorities are denied.
    std::vector<Batch> Apply(Request r) {
        if (r.operation == 0) return {};
        const bool all = r.operation == 3 || r.operation == 5;
        const bool supported = r.system == 0 &&
            (all || Supported(r.source, r.priority));
        std::vector<Batch> replies{{14, 12, 0, 0, r.priority, r.source,
            r.system, r.operation, static_cast<uint8_t>(supported ? 1 : 3)}};
        if (!supported) return replies;
        const bool matches = source == r.source && priority == r.priority;
        switch (r.operation) {
        case 1:
            if (Active() && !matches) replies.push_back(Status(source, priority, 1));
            source = r.source; priority = r.priority;
            replies.push_back(Current());
            break;
        case 2:
            // Releasing an already absent tuple is idempotent and cannot release another owner.
            replies.push_back(Status(r.source, r.priority, 1));
            if (matches) { source = 12; priority = 14; }
            if (!Active()) replies.push_back(Current());
            break;
        case 3:
            source = 12; priority = 14;
            replies.push_back(Current());
            break;
        case 4:
            replies.push_back(Status(r.source, r.priority, matches ? 4 : 1));
            break;
        case 5:
            replies.push_back(Current());
            break;
        }
        return replies;
    }
};
