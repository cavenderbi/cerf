# i.MX51 constant depth16 rendering

The rasterizer supports single-sample depth16 draws with a finite constant viewport-transformed depth in [0,1], including the Navigation clear with `RB_DEPTHCONTROL=0x7E`. It applies existing triangle coverage and scissoring, executes the pixel shader, compares depth, and stages color and depth writes together. All eight comparison functions and the depth write enable are honored. Disabling depth testing suppresses depth writes. A zero color mask does not suppress a depth write.

Depth occupies the existing serialized GMEM byte arena. The current logical address is `depth_base + 2*(y*pitch+x)`, with two-byte color attachments, row-aligned depth bases, checked capacity, and disjoint active color/depth regions. This is an extension of CERF's logical GMEM representation; it does not establish physical SRAM bank addressing. Fractional codes use the existing little-endian logical representation; their physical layout and preservation-copy interpretation remain unvalidated.

Constant depth converts as `min(floor(z_viewport * 65536), 65535)`, retaining double precision through viewport transformation and conversion. The Ford guest already programs normal depth scale/offset with a factor of 65535/65536. Applying another factor of 65535 to that transformed value would be incorrect.

The existing rejection of color attachment format/pitch/base reinterpretation remains. A later depth-preservation copy may reach it. Shared storage and state serialization are implemented; guest depth shadow save/restore through color copies is not yet validated.

Nonconstant or out-of-range incoming depth, 24/8 depth-stencil, stencil operations, MSAA, overlapping active attachments, and additional fragment exports remain explicit failures. Shader discard instructions remain unsupported by the shader interpreter and are not bypassed by a failed depth test. The early-depth bit is accepted, but the host implementation evaluates the supported fragment shader before testing depth. Interpolation precision, physical addressing, and preservation-copy formats require further evidence before these guards can be relaxed.

## Evidence

- [NXP depth registers](https://github.com/nxp-imx/linux-imx/blob/a1638da9d9fda588979360b33390cf612a256829/drivers/mxc/amd-gpu/include/reg/yamato/22/yamato_registers.h#L13212-L13439) and [format/comparison enums](https://github.com/nxp-imx/linux-imx/blob/a1638da9d9fda588979360b33390cf612a256829/drivers/mxc/amd-gpu/include/reg/yamato/22/yamato_enum.h#L1521-L1539).
- Ford SYNC 2 `librenderboy.dll`: clear state at `0x41CD2370–0x41CD2448`, viewport depth at `0x41CD2628–0x41CD2648`, allocation helper `0x41CDA5A4`, depth shadow binding `0x41CDB6E8–0x41CDB778`, preservation chain `0x41CD78D0 → 0x41CD5B00 → 0x41CCADC4`.
- [NXP color/depth GMEM preservation](https://github.com/nxp-imx/linux-imx/blob/a1638da9d9fda588979360b33390cf612a256829/drivers/mxc/amd-gpu/common/gsl_drawctxt.c#L664-L995).
- [Khronos depth comparison semantics](https://registry.khronos.org/OpenGL-Refpages/es2.0/xhtml/glDepthFunc.xml).
- [Mesa depth16 clear conversion](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/e97ad7482b2af1f69176d4af43a26a44c70cdac8/src/gallium/drivers/freedreno/a2xx/fd2_draw.c#478).

Physical Ford SYNC 2 capture `RUN_20260906_174030_00` contains 24 comparison outputs F067-F090 and 24 precision outputs F097-F100, F113-F132. Case118's used primary stream `B007_00/D00056.BIN` programs normal Z_SCALE and Z_OFFSET both `0x3EFFFF00` (0.49999237060546875), while its clear draw uses scale0.5 and offset0. Its nominal depth0.5 passes LESS against clear0.5; the precision sweep passes through nominal0.5+2/262144 and fails from0.5+3/262144. Case100 at nominal0.125-1/262144 also passes. After accounting for the actual viewport, floor with multiplier65536 fits all48 outcomes; floor/nearest/ceil with multiplier65535 and nearest/ceil with multiplier65536 each contradict captured cases. These finite observations establish the supported constant-depth model, not a general interpolation or arithmetic-precision specification. Clear uses a rectangle draw; Mesa's separate clear-value packing must not be substituted for this guest's viewport encoding.

## Validation boundary

A capture-derived harness uses the observed registers, three position exports, and actual six-word pixel program. Shader constants were not captured, so the harness supplies a declared red constant. The captured scissor produces 22,016 color/depth pixels; excluded bytes must remain unchanged. Additional checks cover comparisons with endpoint/interior stored values, write masks, viewport transforms, transactional rejection, and serialization. These tests are retained outside the committed source tree.

Guest acceptance requires repeating Home → Navigation, confirming progress beyond the rejected clear, and examining the next draw or preservation-copy capture. A nonuniform physical depth pattern and backing-memory readback are still required to validate layout beyond this logical GMEM model.

Constant-depth tests adapt the captured viewport and binary depth outcomes to the existing supported two-byte color attachment. This is not full fixed-suite command replay: those captures use four-byte color attachments that still reach an independent guard. Tests cover all65536 logical depth codes, endpoint saturation, captured precision/comparison outcomes, fractional serialization, write masks, and transactional rejection. The defined depth-interpolation cases were not captured in this returned run.
