#include "../board_context.h"

#include "../../core/cerf_emulator.h"

namespace {

class NokiaLumia800Context : public BoardContext {
public:
    using BoardContext::BoardContext;

    Board       GetBoard()   const override { return Board::NokiaLumia800; }
    SocFamily   GetSoc()     const override { return SocFamily::MSM8255; }
    CpuArch     GetCpuArch() const override { return CpuArch::Arm; }
    RomPlacingMode GetRomPlacingMode() const override { return RomPlacingMode::FlatContainer; }

    std::optional<PreferredWindowSize> GetPreferredWindowSize() const override {
        return PreferredWindowSize{ 480, 800 };
    }
};

}

REGISTER_SERVICE_AS(NokiaLumia800Context, BoardContext);
