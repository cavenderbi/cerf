#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

/* https://github.com/cavenderbi/cerf/wiki/SYNC-2-entertainment-ILP-gaps */
struct FordSync2MediaProgress {
    std::optional<uint32_t> elapsed, duration;
    static std::optional<uint32_t> Decode(uint32_t value, bool reported) {
        return reported && value >= 100 ? std::optional<uint32_t>(value - 100) : std::nullopt;
    }
    static std::wstring Time(std::optional<uint32_t> seconds) {
        if (!seconds) return L"--:--";
        return std::to_wstring(*seconds / 60) + L":" +
            (*seconds % 60 < 10 ? L"0" : L"") + std::to_wstring(*seconds % 60);
    }
    std::wstring Text() const { return Time(elapsed) + L" / " + Time(duration); }
    unsigned Position() const {
        return elapsed && duration && *duration ?
            static_cast<unsigned>(std::min<uint64_t>(*elapsed, *duration) * 1000 / *duration) : 0;
    }
};

void ShowFordSync2MediaProgress(std::function<FordSync2MediaProgress()> read);
