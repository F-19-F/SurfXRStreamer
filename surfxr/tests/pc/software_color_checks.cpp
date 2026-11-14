#include "SoftwareColorConversion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
constexpr int width = 16, height = 16;
using Frame = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
using Scaler = std::unique_ptr<SwsContext, decltype(&sws_freeContext)>;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Frame frame(AVPixelFormat format) {
    Frame result(av_frame_alloc(), [](AVFrame* value) { av_frame_free(&value); });
    require(bool(result), "frame allocation failed");
    result->width = width;
    result->height = height;
    result->format = format;
    require(av_frame_get_buffer(result.get(), 32) >= 0, "frame buffer allocation failed");
    return result;
}
int sample(const AVFrame& value, int plane) {
    const int x = plane ? width / 4 : width / 2;
    const int y = plane ? height / 4 : height / 2;
    const auto* row = value.data[plane] + y * value.linesize[plane];
    if (value.format == AV_PIX_FMT_YUV420P10)
        return reinterpret_cast<const std::uint16_t*>(row)[x];
    return row[x];
}
void convert(SwsContext* scaler, const AVFrame& source, AVFrame& output) {
    require(sws_scale(scaler, source.data, source.linesize, 0, height,
                      output.data, output.linesize) == height, "software color conversion failed");
}
void check_sdr(AVPixelFormat format) {
    AVCodecContext encoder{};
    encoder.width = width;
    encoder.height = height;
    encoder.pix_fmt = format;
    encoder.colorspace = AVCOL_SPC_BT709;
    encoder.color_range = AVCOL_RANGE_JPEG;
    Scaler scaler(surfxr::create_software_encoder_scaler(width, height, AV_PIX_FMT_RGBA, encoder),
                  sws_freeContext);
    require(bool(scaler), "SDR scaler initialization failed");
    auto source = frame(AV_PIX_FMT_RGBA), output = frame(format);
    const int maximum = format == AV_PIX_FMT_YUV420P10 ? 1023 : 255;
    // Uniform patches isolate matrix/range from chroma subsampling and scaling.
    const std::array<std::array<int, 3>, 9> colors = {{{0, 0, 0}, {255, 255, 255},
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {128, 128, 128},
        {0, 255, 255}, {255, 0, 255}, {255, 255, 0}}};
    for (const auto& rgb : colors) {
        for (int y = 0; y < height; ++y) {
            auto* row = source->data[0] + y * source->linesize[0];
            for (int x = 0; x < width; ++x) {
                for (int channel = 0; channel < 3; ++channel)
                    row[x * 4 + channel] = static_cast<std::uint8_t>(rgb[channel]);
                row[x * 4 + 3] = 255;
            }
        }
        convert(scaler.get(), *source, *output);
        const double luma = .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2];
        const std::array<double, 3> expected = {luma / 255.0 * maximum,
            ((rgb[2] - luma) / (2 * (1 - .0722)) + 128) / 255.0 * maximum,
            ((rgb[0] - luma) / (2 * (1 - .2126)) + 128) / 255.0 * maximum};
        for (int plane = 0; plane < 3; ++plane) {
            const auto reference = std::min(double(maximum), std::max(0.0, expected[plane]));
            require(std::abs(sample(*output, plane) - reference) <= (maximum == 255 ? 2 : 5),
                    "SDR output does not match BT709 full-range values");
        }
        if (rgb == colors[0]) require(sample(*output, 0) == 0, "SDR black lifted above zero");
        if (rgb == colors[1]) require(sample(*output, 0) >= maximum - 3, "SDR white compressed");
    }
}
void check_planar_full_range_preserved() {
    // This validates the scaler's full-YUV range contract only. It does not
    // exercise or certify the separate D3D HDR source layout in VideoEncoderSW.
    AVCodecContext encoder{};
    encoder.width = width;
    encoder.height = height;
    encoder.pix_fmt = AV_PIX_FMT_YUV420P10;
    encoder.colorspace = AVCOL_SPC_BT2020_NCL;
    encoder.color_range = AVCOL_RANGE_JPEG;
    Scaler scaler(surfxr::create_software_encoder_scaler(width, height, encoder.pix_fmt, encoder),
                  sws_freeContext);
    require(bool(scaler), "full-YUV scaler initialization failed");
    auto source = frame(encoder.pix_fmt), output = frame(encoder.pix_fmt);
    for (const int code : {0, 64, 512, 940, 1023}) {
        for (int plane = 0; plane < 3; ++plane) {
            for (int y = 0; y < (plane ? height / 2 : height); ++y) {
                auto* row = reinterpret_cast<std::uint16_t*>(source->data[plane] + y * source->linesize[plane]);
                for (int x = 0; x < (plane ? width / 2 : width); ++x)
                    row[x] = static_cast<std::uint16_t>(code);
            }
        }
        convert(scaler.get(), *source, *output);
        for (int plane = 0; plane < 3; ++plane)
            require(sample(*output, plane) == code, "full-YUV scaler changed existing code values");
    }
}
}
int main() {
    try {
        check_sdr(AV_PIX_FMT_YUV420P);
        check_sdr(AV_PIX_FMT_YUV420P10);
        check_planar_full_range_preserved();
        std::cout << "software encoder color: BT709 full-range SDR 8/10 bit and planar full-YUV range passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
