#pragma once

#include <cmath>

namespace ColorSpace {
inline float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

inline float scrgb_white_scale(float paper_white_nits, bool hdr_display) {
    // In SDR, 1 represents display reference white; convert to absolute luminance only on HDR displays.
    return hdr_display ? paper_white_nits / 80.0f : 1.0f;
}
}
