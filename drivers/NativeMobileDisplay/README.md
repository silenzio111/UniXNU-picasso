# Picasso CPU/GOP display

Adapted from [NativeMobileDisplay / GopFramebuffer](https://github.com/UniXNU/GopFramebuffer/tree/8666a9950c9699b368931d18cac92c0fdb2ce9c0),
the component revision used by the fixed UniXNU pin. `LICENSE` and original
runtime class/symbol names are retained. The base-header template follows the
original Legacy ABI declaration in UniXNU-sheng's display builder; its complete
compiled vtable is checked against Picasso's pinned 21H461 kernelcache.

Only `NativeLegacyDisplay.cpp` is built. The driver uses the original
IOMobileFramebufferLegacy lifecycle, user client and IOSurface getters. Its
board adapter is `package/PicassoDisplay.cpp`; the builder is
`scripts/build-picasso-display.py`. `CONFIG_XNU_NATIVE_MOBILE_DISPLAY` stays off.

Differences from the pinned component:

- CPU copies only; GPU presenter, early retirement and differential shadow
  allocations are removed. Linear BGRA is accepted; compressed `&BGA` is refused.
- The retained firmware framebuffer becomes the main display regardless of
  the boot console's text/graphics flag. XNU releases its text console before
  the first frame copy. CDC remains the independent diagnostic channel.
- Admission checks the actual PE video format, dimensions, stride, alignment,
  rotation, offset and extent within Picasso's reserved display memory.
- A full validation table refuses a new request instead of forgetting a live
  request. The original queue releases a frame only after the copy/barrier.
- Frame scans and event logs are bounded. Seven unclassified backend leaves
  still panic with the actual slot and caller; optional telemetry returns
  Unsupported. No missing operation reports successful completion.

The firmware owns panel scanout and power. A 60 Hz software notification timer
does not measure panel vblank. There is no GPU, backlight or physical panel
power implementation. See [the investigation](../../docs/display-gop-investigation.md)
for evidence and remaining validation.
