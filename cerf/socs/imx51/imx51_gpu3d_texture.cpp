#include "imx51_gpu3d_texture.h"
#include "imx51_gpu3d_memory.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../boards/board_context.h"
#include <algorithm>
#include <cmath>

REGISTER_SERVICE(Imx51Gpu3dTexture);
bool Imx51Gpu3dTexture::ShouldRegister() {
    auto* board = emu_.TryGet<BoardContext>();
    return board && board->GetSoc() == SocFamily::iMX51;
}

/* Mesa e97ad748, a2xx.xml: A2XX_SQ_TEX; instr-a2xx.h: instr_fetch_tex_t;
   fd2_gmem.c: emit_mem2gmem_surf. */
Imx51Gpu3dVec4 Imx51Gpu3dTexture::Sample(const std::unordered_map<uint32_t,uint32_t>& registers,
    uint32_t mmu_config, uint32_t slot, const Imx51Gpu3dVec4& coordinates,
    std::array<uint32_t,3> instruction) {
    auto fail = [&](const char* reason, uint32_t value) {
        emu_.Get<Fatal>().Die("GPU texture %s slot=%u value=%08X", reason, slot, value);
    };
    if (slot >= 32u) fail("slot", slot);
    std::array<uint32_t,6> state{};
    for (uint32_t i = 0; i < state.size(); ++i) {
        const auto found = registers.find(0x4800u + slot * 6u + i);
        if (found == registers.end()) fail("missing descriptor", i);
        state[i] = found->second;
    }
    const uint32_t format = state[1] & 63u, pitch = ((state[0] >> 22) & 511u) * 32u;
    const uint32_t width = (state[2] & 8191u) + 1u, height = ((state[2] >> 13) & 8191u) + 1u;
    const uint32_t clamp_x = (state[0] >> 10) & 7u, clamp_y = (state[0] >> 13) & 7u;
    if ((state[0] & 0x800003FDu) != 0 || ((state[1] >> 6) & 15u) != 0 || (state[3] & 1u) != 0)
        fail("unsupported tiling/type/sign/endian", state[0]);
    if (((state[5] >> 9) & 3u) != 1u || (state[2] >> 26) != 0 || pitch < width)
        fail("unsupported dimension/pitch", state[5]);
    if ((clamp_x != 0u && clamp_x != 1u && clamp_x != 2u) ||
        (clamp_y != 0u && clamp_y != 1u && clamp_y != 2u)) fail("unsupported clamp", state[0]);
    if ((state[4] & 0x003FFC3Cu) != 0 || (state[3] & 0xFE07E000u) != 0 ||
        (instruction[2] & 0x7FFFFFFDu) != 0) fail("unsupported LOD/offset", state[4]);
    const uint32_t aniso = (instruction[1] >> 18) & 7u, arbitrary = (instruction[1] >> 21) & 7u;
    if ((aniso != 0u && aniso != 7u) || (arbitrary != 0u && arbitrary != 7u) ||
        (instruction[1] & 0x60000000u) != 0) fail("unsupported anisotropy/register LOD",instruction[1]);
    auto filter = [&](uint32_t shift, uint32_t constant_shift) {
        const uint32_t selected = (instruction[1] >> shift) & 3u;
        return selected == 3u ? (state[3] >> constant_shift) & 3u : selected;
    };
    const uint32_t mag = filter(12u,19u), min = filter(14u,21u), mip = filter(16u,23u);
    if (mag > 1u || mag != min || mip != 2u) fail("unsupported filter", instruction[1]);
    if (format != 6u && format != 4u && format != 2u && format != 15u) fail("unsupported format", format);
    const uint32_t bytes = format == 6u ? 4u : (format == 4u || format == 15u) ? 2u : 1u;
    const auto* data = emu_.Get<Imx51Gpu3dMemory>().ReadSpan(state[1] & 0xFFFFF000u,
        uint64_t(height - 1u) * pitch * bytes + uint64_t(width) * bytes, mmu_config);
    double u = coordinates[0], v = coordinates[1];
    if (!std::isfinite(u) || !std::isfinite(v)) fail("nonfinite coordinate", instruction[0]);
    if ((instruction[0] & (1u << 25)) == 0) { u *= width; v *= height; }
    auto reduce = [](double x, uint32_t size, uint32_t clamp) {
        if (clamp == 2u) return std::clamp(x, 0.0, double(size));
        const double period = double(size) * (clamp == 1u ? 2.0 : 1.0);
        return x - std::floor(x / period) * period;
    };
    u = reduce(u,width,clamp_x); v = reduce(v,height,clamp_y);
    auto index = [](int value, uint32_t size, uint32_t clamp) {
        const int n = static_cast<int>(size);
        if (clamp == 2u) return std::clamp(value, 0, n - 1);
        const int period = clamp == 1u ? n * 2 : n;
        int wrapped = value % period; if (wrapped < 0) wrapped += period;
        return wrapped >= n ? period - wrapped - 1 : wrapped;
    };
    auto texel = [&](int x, int y) {
        x = index(x,width,clamp_x); y = index(y,height,clamp_y);
        const uint8_t* p = data + (uint64_t(y) * pitch + static_cast<uint32_t>(x)) * bytes;
        Imx51Gpu3dVec4 raw{};
        if (bytes == 4u) for (unsigned c = 0; c < 4; ++c) raw[c] = float(p[c]) / 255.0f;
        else if (bytes == 2u) {
            const uint32_t packed = uint32_t(p[0]) | (uint32_t(p[1]) << 8);
            if (format == 15u) for (unsigned i=0;i<4;++i) raw[i]=float((packed>>(i*4u))&15u)/15.0f;
            else raw = {float(packed & 31u) / 31.0f,float((packed >> 5) & 63u) / 63.0f,
                   float((packed >> 11) & 31u) / 31.0f,1.0f};
        } else raw = {float(p[0]) / 255.0f,0.0f,0.0f,1.0f};
        Imx51Gpu3dVec4 result{};
        for (unsigned c = 0; c < 4; ++c) {
            const uint32_t swizzle = (state[3] >> (1u + c * 3u)) & 7u;
            if (swizzle > 5u) fail("unsupported swizzle", swizzle);
            result[c] = swizzle < 4u ? raw[swizzle] : swizzle == 5u ? 1.0f : 0.0f;
        }
        return result;
    };
    if (mag == 0u) return texel(static_cast<int>(std::floor(u)),static_cast<int>(std::floor(v)));
    u -= 0.5; v -= 0.5;
    const int x = static_cast<int>(std::floor(u)), y = static_cast<int>(std::floor(v));
    const float fx = static_cast<float>(u - x), fy = static_cast<float>(v - y);
    const auto a = texel(x,y), b = texel(x+1,y), c = texel(x,y+1), d = texel(x+1,y+1);
    Imx51Gpu3dVec4 result{};
    for (unsigned k = 0; k < 4; ++k) result[k] = std::lerp(std::lerp(a[k],b[k],fx),std::lerp(c[k],d[k],fx),fy);
    return result;
}
