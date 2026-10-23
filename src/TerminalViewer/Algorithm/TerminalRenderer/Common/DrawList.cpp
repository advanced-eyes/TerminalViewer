#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawList.h"

uint32_t PackColor(const TerminalRgb& rgb, uint8_t alpha)
{
    const auto funcPremultiply = [alpha](uint8_t channel) { return uint32_t((channel * alpha + 127) / 255); };
    return funcPremultiply(rgb.r) | (funcPremultiply(rgb.g) << 8) | (funcPremultiply(rgb.b) << 16) | (uint32_t(alpha) << 24);
}
