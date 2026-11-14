#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace surfxr {

// The staging source contains full-range RGB, or full-range YUV already
// converted by FrameRender. Encoder metadata alone does not configure swscale.
inline SwsContext* create_software_encoder_scaler(
    int sourceWidth, int sourceHeight, AVPixelFormat sourceFormat, const AVCodecContext& encoder
) {
    int matrix;
    switch (encoder.colorspace) {
    case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
    case AVCOL_SPC_BT2020_NCL: matrix = SWS_CS_BT2020; break;
    default: return nullptr;
    }
    if (encoder.color_range != AVCOL_RANGE_JPEG && encoder.color_range != AVCOL_RANGE_MPEG)
        return nullptr;

    auto* scaler = sws_getContext(
        sourceWidth, sourceHeight, sourceFormat,
        encoder.width, encoder.height, encoder.pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!scaler) return nullptr;

    const int* coefficients = sws_getCoefficients(matrix);
    if (sws_setColorspaceDetails(
            scaler, coefficients, 1, coefficients, encoder.color_range == AVCOL_RANGE_JPEG,
            0, 1 << 16, 1 << 16
        ) < 0) {
        sws_freeContext(scaler);
        return nullptr;
    }
    return scaler;
}

} // namespace surfxr
