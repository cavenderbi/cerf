# Provisional i.MX51 GPU dithering

This is an **unverified approximation**, not a hardware-accurate Z430 dither model.
`ApproximateDitherQuantize` in `cerf/socs/imx51/imx51_gpu3d_raster.cpp` is the replacement point.
Its inputs are normalized channel value, destination channel maximum, component index,
final target pixel coordinates, and enable state.

RGB uses the [ImageMagick ordered 4×4 thresholds](https://github.com/ImageMagick/ImageMagick/blob/main/config/thresholds.xml#L85-L96):
`1,9,3,11 / 13,5,15,7 / 4,12,2,10 / 16,8,14,6`, divided by 17.
Quantization is `floor(channel * maximum + 1 - threshold)`, clamped to the
destination range. The pattern uses absolute target coordinates, including window
offsets; it does not restart at triangle or memory-tile boundaries.
Alpha retains nearest-integer rounding. Dither-disabled RGBA8888 with color round mode zero truncates; other supported formats retain nearest-integer rounding.

Pixel dithering modes ALWAYS and IF_ALPHA_OFF are accepted within the existing
supported state subset, which requires alpha testing disabled. Subpixel mode,
unknown modes, and resolve-copy dithering remain unsupported. Resolve copies
retain exact existing quantization.

Physical hardware measurements must establish the actual matrix, coordinate phase, rounding,
alpha treatment, and subpixel behavior. Replace this helper when that evidence
is available; no renderer interface or configuration framework is required.
