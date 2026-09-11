#include "imx51_gpu3d_raster.h"
#include "imx51_gpu3d_memory.h"
#include "../../core/cerf_emulator.h"
#include "../../core/fatal.h"
#include "../../state/state_stream.h"
#include "../../boards/board_context.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

REGISTER_SERVICE(Imx51Gpu3dRaster);
bool Imx51Gpu3dRaster::ShouldRegister() {
    auto* board = emu_.TryGet<BoardContext>();
    return board && board->GetSoc() == SocFamily::iMX51;
}
void Imx51Gpu3dRaster::SaveState(StateWriter& writer) {
    writer.Write(gmem_binding_); writer.Write(gmem_pitch_);
    writer.WriteBytes(gmem_.data(),gmem_.size());
}
void Imx51Gpu3dRaster::RestoreState(StateReader& reader) {
    reader.Read(gmem_binding_); reader.Read(gmem_pitch_);
    reader.ReadBytes(gmem_.data(),gmem_.size());
    if (gmem_binding_ != 0xFFFFFFFFu &&
        ((gmem_binding_ & 0xFF0u) != 0u || (gmem_binding_ & 0xFFFFF000u) >= gmem_.size() || gmem_pitch_ == 0 || gmem_pitch_ > 16383u ||
         ((gmem_binding_ & 15u) != 0u && (gmem_binding_ & 15u) != 2u && (gmem_binding_ & 15u) != 5u)))
        emu_.Get<Fatal>().Die("GPU raster invalid saved GMEM binding");
}

/* Mesa e97ad748 a2xx.xml: PA_CL_VTE_CNTL, PA_SU_VTX_CNTL, RB_COLOR_INFO;
   fd2_gmem.c: fmt2swap, fd2_emit_sysmem_prep, fd2_emit_tile_renderprep. */
void Imx51Gpu3dRaster::Triangle(const std::array<Imx51Gpu3dShaderState,3>& vertices,
    const std::unordered_map<uint32_t,uint32_t>& registers,
    std::span<const uint32_t> pixel_program, uint32_t mmu_config) {
    auto fail = [&](const char* reason, uint32_t value) {
        emu_.Get<Fatal>().Die("GPU raster unsupported %s value=%08X",reason,value);
    };
    for (const auto& vertex : vertices)
        if ((vertex.export_mask & (uint64_t{1} << 62)) == 0) fail("missing position",0);
    const auto& p0 = vertices[0].exports[62];
    const auto& p1 = vertices[1].exports[62];
    const auto& p2 = vertices[2].exports[62];
    if (p0 == p1 || p1 == p2 || p2 == p0) return;
    auto reg = [&](uint32_t index) {
        const auto i = registers.find(index);
        if (i == registers.end()) fail("missing register",index);
        return i->second;
    };
    const uint32_t raster = reg(0x2205);
    /* Mesa e97ad748 fd2_emit.c:169-215, fd2_emit_state_binning;
       NXP a1638da9 yamato_registers.h:400, FACE_KILL_ENABLE. */
    if ((raster & 0xC0000000u) == 0x40000000u) return;
    const uint32_t vte = reg(0x2206), control = reg(0x2202);
    const uint32_t clip = reg(0x2204);
    if (clip != 0u && clip != 0x10000u) fail("clip controls",clip);
    if (vte != 0x43Fu && vte != 0xB00u && vte != 0x30Fu) fail("viewport format",vte);
    if ((raster & 0xC000B81Fu) != 0) fail("cull/MSAA/polygon/faceness",raster);
    if (reg(0x2200) != 0) fail("depth/stencil",reg(0x2200));
    if ((control & ~7u) != 0x20u && (control & ~7u) != 0xC20u) fail("blend/alpha/ROP/dither",control);
    const uint32_t mode = reg(0x2208);
    if (mode != 4u && mode != 6u) fail("render mode",mode);
    const bool resolve = mode == 6u;
    /* NXP a1638da9 gsl_drawctxt.c:960-989, build_sys2gmem_cmds: VTE=B00, mode=4. */
    if (vte == 0xB00u && !resolve && clip != 0x10000u) fail("window-space clipping",clip);
    const uint32_t info = reg(0x2001), surface = reg(0x2000), format = info & 15u;
    if ((surface & ~0x3FFFu) != 0 || (surface & 0x3FFFu) == 0) fail("surface/MSAA",surface);
    if (format != 0u && format != 2u && format != 5u) fail("color format",format);
    if ((info & 0x180u) != 0 || ((info >> 9) & 3u) > 1u) fail("endian/swap",info);
    const bool gmem = (info & 0x40u) == 0;
    const uint32_t binding = info & 0xFFFFF00Fu;
    if (gmem) {
        if ((mmu_config & 1u) && mmu_config != 1u) fail("GMEM MMU mode",mmu_config);
        if (reg(0xF02) != 3u) fail("GMEM configuration",reg(0xF02));
        if ((info & 0xFFFFF000u) >= gmem_.size()) fail("tiled system target",info);
        if (gmem_binding_ != 0xFFFFFFFFu && (gmem_binding_ != binding || gmem_pitch_ != (surface & 0x3FFFu)))
            fail("GMEM format/pitch/base reinterpretation",info);
    }
    if (resolve && (!gmem || gmem_binding_ == 0xFFFFFFFFu)) fail("uninitialized resolve source",info);
    const uint32_t pitch = surface & 0x3FFFu, bytes = format == 5u ? 4u : 2u;
    uint32_t color_mask = reg(0x2104), target_info = info, target_pitch = pitch, offset_x = 0, offset_y = 0;
    uint32_t target_base = info & 0xFFFFF000u;
    /* NXP a1638da9 gsl_drawctxt.c:735-819, build_gmem2sys_cmds;
       Mesa e97ad748 fd2_gmem.c:70-112, emit_gmem2mem_surf. */
    if (resolve) {
        const uint32_t copy = reg(0x231B), offset = reg(0x231C);
        if (reg(0x2318) != 0u || (copy & 7u) != 0u || !(copy & 8u) ||
            ((copy >> 4) & 15u) != format || ((copy >> 8) & 3u) != 0u || (info & 0x600u) != 0u || (copy & 0xFFFFFC00u & ~0x3C000u))
            fail("resolve clear/sample/format/tiling",copy);
        if (offset & 0xFC000000u) fail("resolve offset",offset);
        target_base = reg(0x2319); target_pitch = reg(0x231A) * 32u;
        if ((target_base & 4095u) || reg(0x231A) > 511u || !target_pitch) fail("resolve destination",target_base);
        color_mask = (copy >> 14) & 15u; target_info = (copy & 0x300u) << 1;
        offset_x = offset & 8191u; offset_y = (offset >> 13) & 8191u;
    }
    if ((color_mask & ~15u) != 0) fail("color mask",color_mask);
    const uint32_t vtx = reg(0x2302);
    if (vtx != 5u) fail("pixel center/quantization",vtx);
    struct Point { double x, y, inverse_w; };
    std::array<Point,3> points{};
    for (unsigned i = 0; i < 3; ++i) {
        const auto& p = vertices[i].exports[62];
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) ||
            !std::isfinite(p[3]) || (vte != 0xB00u && p[3] <= 0.0f)) fail("homogeneous clipping",i);
        /* NXP a1638da9 gsl_drawctxt.c:731, premultiplied XY/Z; native VTE=30F, W=1. */
        if (vte == 0x30Fu && p[3] != 1.0f) fail("premultiplied nonunit W",i);
        if (vte == 0xB00u && !resolve && p[3] != 1.0f) fail("window-space nonunit W",i);
        const float clip_limit = vte == 0x30Fu ? 1.0f : p[3];
        if (vte != 0xB00u && clip == 0u && (std::abs(p[0]) > clip_limit || std::abs(p[1]) > clip_limit || std::abs(p[2]) > clip_limit))
            fail("clip-plane intersection",i);
        const double inverse_w = vte == 0xB00u ? 1.0 : vte == 0x30Fu ? 1.0 : 1.0 / p[3];
        const double xy_scale = vte == 0x43Fu ? inverse_w : 1.0;
        const double x = vte == 0xB00u ? p[0] : p[0] * xy_scale * std::bit_cast<float>(reg(0x210F)) + std::bit_cast<float>(reg(0x2110));
        const double y = vte == 0xB00u ? p[1] : p[1] * xy_scale * std::bit_cast<float>(reg(0x2111)) + std::bit_cast<float>(reg(0x2112));
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 32768 || std::abs(y) > 32768)
            fail("viewport coordinate range",i);
        points[i] = {std::nearbyint(x * 16.0) / 16.0,std::nearbyint(y * 16.0) / 16.0,inverse_w};
    }
    auto edge = [](const Point& a, const Point& b, double x, double y) {
        return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x);
    };
    double area = edge(points[0],points[1],points[2].x,points[2].y);
    if (area == 0.0) return;
    const double sign = area < 0.0 ? -1.0 : 1.0;
    area *= sign;
    const uint32_t offset = reg(0x2080), window_tl = reg(0x2081), window_br = reg(0x2082);
    auto signed15 = [](uint32_t x) { return static_cast<int>((x & 0x7FFFu) ^ 0x4000u) - 0x4000; };
    const int ox = (raster & 0x10000u) ? signed15(offset) : 0;
    const int oy = (raster & 0x10000u) ? signed15(offset >> 16) : 0;
    for (auto& p : points) { p.x += ox; p.y += oy; }
    const int wx = (window_tl & 0x80000000u) ? 0 : ox, wy = (window_tl & 0x80000000u) ? 0 : oy;
    const uint32_t screen_tl = reg(0x200E), screen_br = reg(0x200F);
    int left = (std::max)(int(screen_tl & 0x7FFFu),int(window_tl & 0x7FFFu)+wx);
    int top = (std::max)(int((screen_tl >> 16) & 0x7FFFu),int((window_tl >> 16) & 0x7FFFu)+wy);
    int right = (std::min)(int(screen_br & 0x7FFFu),int(window_br & 0x7FFFu)+wx);
    int bottom = (std::min)(int((screen_br >> 16) & 0x7FFFu),int((window_br >> 16) & 0x7FFFu)+wy);
    left = (std::max)(left,int(std::floor((std::min)({points[0].x,points[1].x,points[2].x}))));
    top = (std::max)(top,int(std::floor((std::min)({points[0].y,points[1].y,points[2].y}))));
    right = (std::min)(right,int(std::ceil((std::max)({points[0].x,points[1].x,points[2].x}))));
    bottom = (std::min)(bottom,int(std::ceil((std::max)({points[0].y,points[1].y,points[2].y}))));
    if (left >= right || top >= bottom || color_mask == 0) return;
    if (left < 0 || top < 0 || right > static_cast<int>(pitch)) fail("target bounds",pitch);
    if (!resolve && pixel_program.empty()) fail("missing pixel shader",0);
    const uint64_t extent = (uint64_t(bottom-1)*pitch+right)*bytes;
    const uint64_t base = info & 0xFFFFF000u;
    if (gmem && base+extent > gmem_.size()) fail("GMEM capacity",static_cast<uint32_t>(base));
    if (uint64_t(right)+offset_x > target_pitch) fail("resolve row bounds",target_pitch);
    auto* target = gmem && !resolve ? gmem_.data()+base : emu_.Get<Imx51Gpu3dMemory>().WriteSpan(target_base,
        (uint64_t(bottom-1+offset_y)*target_pitch+right+offset_x)*bytes,mmu_config);
    struct Pixel { uint64_t offset; std::array<uint8_t,4> data; };
    std::vector<Pixel> writes;
    auto top_left = [&](const Point& a, const Point& b) {
        const double dx = (b.x-a.x)*sign, dy = (b.y-a.y)*sign;
        return dy < 0.0 || (dy == 0.0 && dx > 0.0);
    };
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
        const double a = edge(points[1],points[2],x+0.5,y+0.5)*sign;
        const double b = edge(points[2],points[0],x+0.5,y+0.5)*sign;
        const double c = edge(points[0],points[1],x+0.5,y+0.5)*sign;
        if (a < 0 || b < 0 || c < 0 || (a == 0 && !top_left(points[1],points[2])) ||
            (b == 0 && !top_left(points[2],points[0])) || (c == 0 && !top_left(points[0],points[1]))) continue;
        std::array<double,3> weights{a/area,b/area,c/area};
        if ((raster & 0x100000u) == 0) {
            double total = 0;
            for (unsigned i = 0; i < 3; ++i) { weights[i] *= points[i].inverse_w; total += weights[i]; }
            for (auto& weight : weights) weight /= total;
        }
        Imx51Gpu3dVec4 color{};
        if (resolve) {
            const auto* p = gmem_.data()+base+(uint64_t(y)*pitch+x)*bytes;
            if (bytes == 4u) for (unsigned i=0;i<4;++i) color[i]=float(p[i])/255.0f;
            else {
                const uint32_t packed=uint32_t(p[0])|(uint32_t(p[1])<<8);
                if (format == 0u) for (unsigned i=0;i<4;++i) color[i]=float((packed>>(i*4u))&15u)/15.0f;
                else color={float(packed&31u)/31.0f,float((packed>>5)&63u)/63.0f,float((packed>>11)&31u)/31.0f,1.0f};
            }
        } else {
            Imx51Gpu3dShaderState fragment{};
            const uint64_t varyings = vertices[0].export_mask & vertices[1].export_mask & vertices[2].export_mask;
            for (unsigned slot = 0; slot < 32; ++slot) if (varyings & (uint64_t{1} << slot))
                for (unsigned component = 0; component < 4; ++component)
                    for (unsigned i = 0; i < 3; ++i)
                        fragment.registers[slot][component] += static_cast<float>(weights[i]*vertices[i].exports[slot][component]);
            emu_.Get<Imx51Gpu3dShader>().Run(pixel_program,true,registers,mmu_config,fragment);
            if (!fragment.memory_exports.empty()) fail("pixel memory export",0);
            if (fragment.killed) continue;
            if ((fragment.export_mask & 1u) == 0) fail("missing fragment color",0);
            color = fragment.exports[0];
        }
        for (auto& channel : color) {
            if (!std::isfinite(channel)) fail("nonfinite fragment",0);
            channel = std::clamp(channel,0.0f,1.0f);
        }
        const uint64_t address = (uint64_t(y+offset_y)*target_pitch+x+offset_x)*bytes;
        Pixel pixel{address,{}};
        if (((target_info >> 9) & 3u) == 1u) std::swap(color[0],color[2]);
        uint32_t mask = color_mask;
        if (((target_info >> 9) & 3u) == 1u) mask = (mask & 10u) | ((mask & 1u) << 2) | ((mask & 4u) >> 2);
        if (bytes == 4u) for (unsigned i = 0; i < 4; ++i)
            pixel.data[i] = (mask & (1u << i)) ? static_cast<uint8_t>(std::lround(color[i]*255.0f)) : target[address+i];
        else {
            uint32_t packed = uint32_t(target[address]) | (uint32_t(target[address+1]) << 8);
            const std::array<uint32_t,4> shifts = format == 0u ? std::array<uint32_t,4>{0,4,8,12} : std::array<uint32_t,4>{0,5,11,0};
            const std::array<uint32_t,4> maxima = format == 0u ? std::array<uint32_t,4>{15,15,15,15} : std::array<uint32_t,4>{31,63,31,0};
            for (unsigned i = 0; i < (format == 0u ? 4u : 3u); ++i) if (mask & (1u << i))
                packed = (packed & ~(maxima[i] << shifts[i])) | (static_cast<uint32_t>(std::lround(color[i]*maxima[i])) << shifts[i]);
            pixel.data[0] = static_cast<uint8_t>(packed); pixel.data[1] = static_cast<uint8_t>(packed >> 8);
        }
        writes.push_back(pixel);
    }
    if (gmem && !resolve && !writes.empty()) { gmem_binding_ = binding; gmem_pitch_ = pitch; }
    for (const auto& pixel : writes) for (unsigned i = 0; i < bytes; ++i) target[pixel.offset+i] = pixel.data[i];
}
