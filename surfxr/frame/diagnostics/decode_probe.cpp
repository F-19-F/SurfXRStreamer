// Offline diagnostic decoder. Input must be an explicit bounded local elementary
// stream. No sockets, display output, account input, or runtime settings.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hash.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

constexpr std::uintmax_t kMaxInputBytes = 256u * 1024u * 1024u;
constexpr std::int64_t kMaxPixels = 8192ll * 8192ll;

struct Options {
    std::string codec;
    std::string input;
    std::string framing = "annexb";
    std::string decoder = "software";
    std::string rgba_output;
    unsigned max_frames = 1;
};

struct Failure : std::runtime_error {
    int ffmpeg_code;
    explicit Failure(const char* stage, int code = 0) : std::runtime_error(stage), ffmpeg_code(code) {}
};

void check(int result, const char* stage) {
    if (result < 0) throw Failure(stage, result);
}

Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 == argc) throw Failure("missing_option_value");
        const std::string value = argv[++i];
        if (key == "--codec") result.codec = value;
        else if (key == "--input") result.input = value;
        else if (key == "--format") result.framing = value;
        else if (key == "--decoder") result.decoder = value;
        else if (key == "--rgba-output") result.rgba_output = value;
        else if (key == "--max-frames") {
            if (value.empty() || value.size() > 2 ||
                !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
                throw Failure("invalid_frame_limit");
            result.max_frames = static_cast<unsigned>(std::stoul(value));
        } else throw Failure("unknown_option");
    }
    if (result.codec != "h264" && result.codec != "hevc" && result.codec != "av1")
        throw Failure("codec_must_be_h264_hevc_or_av1");
    if (result.input.empty()) throw Failure("input_path_required");
    if (result.max_frames == 0 || result.max_frames > 60) throw Failure("frame_limit_must_be_1_to_60");
    if (result.framing != "annexb" && !(result.codec == "av1" && result.framing == "obu"))
        throw Failure("invalid_stream_framing");
    if (result.decoder != "software" && result.decoder != "v4l2")
        throw Failure("decoder_must_be_software_or_v4l2");
    if (result.decoder == "v4l2" && result.codec == "av1") throw Failure("av1_v4l2_not_supported");
    return result;
}

struct Format {
    AVFormatContext* value = nullptr;
    ~Format() { avformat_close_input(&value); }
};
struct Codec {
    AVCodecContext* value = nullptr;
    ~Codec() { avcodec_free_context(&value); }
};
struct Frame {
    AVFrame* value = av_frame_alloc();
    Frame() { if (!value) throw Failure("frame_allocation_failed"); }
    ~Frame() { av_frame_free(&value); }
};
struct Packet {
    AVPacket* value = av_packet_alloc();
    Packet() { if (!value) throw Failure("packet_allocation_failed"); }
    ~Packet() { av_packet_free(&value); }
};
struct Hash {
    AVHashContext* value = nullptr;
    Hash() { check(av_hash_alloc(&value, "sha256"), "hash_allocation_failed"); }
    ~Hash() { av_hash_freep(&value); }
};

struct FrameReport {
    int width;
    int height;
    std::string format;
    int bytes;
    std::string hash;
    bool transferred;
    bool rgba_created = false;
    std::size_t rgba_bytes = 0;
    std::string rgba_hash;
};

std::string pixel_hash(const std::uint8_t* data, std::size_t size) {
    Hash hash;
    av_hash_init(hash.value);
    av_hash_update(hash.value, data, size);
    std::array<std::uint8_t, 65> hex{};
    av_hash_final_hex(hash.value, hex.data(), hex.size());
    return reinterpret_cast<const char*>(hex.data());
}

void write_rgba(AVFrame* frame, const std::string& path, FrameReport& report) {
    const auto count = static_cast<std::size_t>(frame->width) * frame->height * 4;
    std::vector<std::uint8_t> rgba(count);
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> converter(
        sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                       frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr),
        sws_freeContext);
    if (!converter) throw Failure("rgba_converter_failed");
    int matrix = SWS_CS_DEFAULT;
    if (frame->colorspace == AVCOL_SPC_BT709) matrix = SWS_CS_ITU709;
    if (frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL)
        matrix = SWS_CS_BT2020;
    check(sws_setColorspaceDetails(converter.get(), sws_getCoefficients(matrix),
                                  frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
                                  sws_getCoefficients(matrix), 1, 0, 1 << 16, 1 << 16),
          "rgba_colorspace_setup_failed");
    std::uint8_t* planes[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int strides[4] = {frame->width * 4, 0, 0, 0};
    const int lines = sws_scale(converter.get(), frame->data, frame->linesize, 0, frame->height, planes, strides);
    if (lines != frame->height) throw Failure("rgba_conversion_incomplete");
    struct Output {
        int fd;
        ~Output() { if (fd >= 0) close(fd); }
    } output{open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (output.fd < 0) throw Failure("rgba_output_must_be_new_writable_file");
    std::size_t offset = 0;
    while (offset < rgba.size()) {
        const auto written = write(output.fd, rgba.data() + offset, rgba.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw Failure("rgba_output_write_failed");
        offset += static_cast<std::size_t>(written);
    }
    report.rgba_created = true;
    report.rgba_bytes = rgba.size();
    report.rgba_hash = pixel_hash(rgba.data(), rgba.size());
}

FrameReport describe(AVFrame* frame, const std::string* rgba_path) {
    Frame download;
    bool transferred = false;
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (!desc) throw Failure("unknown_pixel_format");
    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        check(av_hwframe_transfer_data(download.value, frame, 0), "hardware_frame_download_failed");
        check(av_frame_copy_props(download.value, frame), "hardware_frame_properties_failed");
        frame = download.value;
        transferred = true;
    }
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 8192 || frame->height > 8192 ||
        std::int64_t{frame->width} * frame->height > kMaxPixels)
        throw Failure("decoded_dimensions_exceed_policy");
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const int size = av_image_get_buffer_size(format, frame->width, frame->height, 1);
    check(size, "pixel_buffer_size_failed");
    if (size <= 0 || size > 512 * 1024 * 1024) throw Failure("pixel_buffer_exceeds_policy");
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size));
    check(av_image_copy_to_buffer(pixels.data(), size, frame->data, frame->linesize, format,
                                  frame->width, frame->height, 1), "pixel_buffer_copy_failed");
    const char* name = av_get_pix_fmt_name(format);
    if (!name) throw Failure("pixel_format_name_missing");
    FrameReport report{frame->width, frame->height, name, size,
                       pixel_hash(pixels.data(), pixels.size()), transferred, false, 0, {}};
    if (rgba_path) write_rgba(frame, *rgba_path, report);
    return report;
}

const AVCodec* select_decoder(const Options& opts, AVCodecID id) {
    const AVCodec* decoder = nullptr;
    if (opts.decoder == "v4l2") {
        decoder = avcodec_find_decoder_by_name(opts.codec == "h264" ? "h264_v4l2m2m" : "hevc_v4l2m2m");
    } else if (id == AV_CODEC_ID_AV1) {
        decoder = avcodec_find_decoder_by_name("libdav1d");
        if (!decoder) decoder = avcodec_find_decoder_by_name("libaom-av1");
        if (!decoder) decoder = avcodec_find_decoder_by_name("av1");
    } else {
        decoder = avcodec_find_decoder_by_name(opts.codec.c_str());
    }
    if (!decoder || decoder->id != id) throw Failure("requested_decoder_unavailable");
    return decoder;
}

std::string decode(const Options& opts) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(opts.input, filesystem_error) || filesystem_error)
        throw Failure("input_must_be_regular_local_file");
    const auto bytes = std::filesystem::file_size(opts.input, filesystem_error);
    if (filesystem_error || bytes > kMaxInputBytes) throw Failure("input_exceeds_256_mib_policy");
    if (bytes == 0) throw Failure("empty_input_no_frames");

    const auto id = opts.codec == "h264" ? AV_CODEC_ID_H264 :
                    opts.codec == "hevc" ? AV_CODEC_ID_HEVC : AV_CODEC_ID_AV1;
    const char* demux_name = opts.codec == "av1" ?
        (opts.framing == "obu" ? "obu" : "av1") : opts.codec.c_str();
    const AVInputFormat* demuxer = av_find_input_format(demux_name);
    if (!demuxer) throw Failure("raw_demuxer_unavailable");
    Format input;
    AVDictionary* open_options = nullptr;
    av_dict_set(&open_options, "protocol_whitelist", "file", 0);
    const int open_result = avformat_open_input(&input.value, opts.input.c_str(), demuxer, &open_options);
    av_dict_free(&open_options);
    check(open_result, "open_raw_stream_failed");
    if (input.value->nb_streams != 1 || input.value->streams[0]->codecpar->codec_id != id)
        throw Failure("unexpected_raw_stream_layout");
    if (opts.decoder == "v4l2") {
        // Device-backed wrappers can need dimensions before avcodec_open2.
        // Raw software decoders instead obtain them from the submitted headers.
        input.value->probesize = 1024 * 1024;
        input.value->max_analyze_duration = AV_TIME_BASE;
        check(avformat_find_stream_info(input.value, nullptr), "hardware_stream_parameters_failed");
    }

    const AVCodec* decoder = select_decoder(opts, id);
    Codec codec{avcodec_alloc_context3(decoder)};
    if (!codec.value) throw Failure("codec_allocation_failed");
    check(avcodec_parameters_to_context(codec.value, input.value->streams[0]->codecpar),
          "codec_parameters_failed");
    codec.value->thread_count = 1;
    codec.value->max_pixels = kMaxPixels;
    codec.value->err_recognition = AV_EF_EXPLODE;
    check(avcodec_open2(codec.value, decoder, nullptr), "decoder_open_failed");

    std::vector<FrameReport> frames;
    Frame frame;
    Packet packet;
    bool input_eof = false;
    bool decoder_eof = false;
    std::size_t packets = 0;
    auto at_limit = [&] { return frames.size() >= opts.max_frames; };
    auto receive = [&]() -> std::size_t {
        const auto before = frames.size();
        while (!at_limit()) {
            const int result = avcodec_receive_frame(codec.value, frame.value);
            if (result == AVERROR(EAGAIN)) break;
            if (result == AVERROR_EOF) { decoder_eof = true; break; }
            check(result, "decode_receive_failed");
            frames.push_back(describe(frame.value, frames.empty() && !opts.rgba_output.empty() ? &opts.rgba_output : nullptr));
            av_frame_unref(frame.value);
        }
        return frames.size() - before;
    };
    auto send = [&](AVPacket* value) {
        while (!at_limit()) {
            const int result = avcodec_send_packet(codec.value, value);
            if (result == AVERROR(EAGAIN)) {
                const auto produced = receive();
                if (at_limit()) return;
                if (produced == 0 || decoder_eof) throw Failure("decoder_send_receive_made_no_progress");
                continue; // Retry exactly the same unaccepted packet after draining.
            }
            if (result == AVERROR_EOF && value == nullptr) { decoder_eof = true; return; }
            check(result, "decode_send_failed");
            if (value != nullptr) ++packets;
            receive();
            return;
        }
    };

    while (!at_limit()) {
        const int result = av_read_frame(input.value, packet.value);
        if (result == AVERROR_EOF) { input_eof = true; break; }
        check(result, "read_raw_packet_failed");
        if (packet.value->stream_index != 0 || packet.value->size <= 0 ||
            packet.value->size > 64 * 1024 * 1024)
            throw Failure("invalid_raw_packet");
        send(packet.value);
        av_packet_unref(packet.value);
    }
    if (input_eof && !at_limit()) {
        // av_read_frame drains the raw demuxer's parser at EOF. Only after those
        // final packets have been sent do we flush delayed decoder output.
        send(nullptr);
        if (!at_limit() && !decoder_eof) throw Failure("decoder_did_not_finish_after_flush");
    }
    if (frames.empty()) throw Failure("no_frames_decoded");

    std::ostringstream out;
    out << "{\"status\":\"decoded\",\"codec\":\"" << opts.codec << "\",\"framing\":\"" << opts.framing
        << "\",\"decoder\":\"" << decoder->name << "\",\"input_bytes\":" << bytes
        << ",\"packets_sent\":" << packets << ",\"decoded_frames\":" << frames.size()
        << ",\"stopped_at_limit\":" << (at_limit() ? "true" : "false")
        << ",\"input_eof\":" << (input_eof ? "true" : "false")
        << ",\"decoder_eof\":" << (decoder_eof ? "true" : "false")
        << ",\"pixel_hash_algorithm\":\"sha256\",\"pixel_layout\":\"packed_planes_align1\",\"frames\":[";
    bool first = true;
    for (const auto& report : frames) {
        if (!first) out << ',';
        first = false;
        out << "{\"width\":" << report.width << ",\"height\":" << report.height
            << ",\"pixel_format\":\"" << report.format << "\",\"pixel_bytes\":" << report.bytes
            << ",\"pixel_sha256\":\"" << report.hash << "\",\"hardware_download\":"
            << (report.transferred ? "true" : "false");
        if (report.rgba_created)
            out << ",\"rgba_output_created\":true,\"rgba_bytes\":" << report.rgba_bytes
                << ",\"rgba_sha256\":\"" << report.rgba_hash << '"';
        out << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_QUIET);
    try {
        std::cout << decode(options(argc, argv)) << '\n';
        return 0;
    } catch (const Failure& error) {
        // Stages are fixed literals; paths, stream bytes and library log text are omitted.
        std::cerr << "{\"status\":\"decode_failed\",\"stage\":\"" << error.what()
                  << "\",\"ffmpeg_code\":" << error.ffmpeg_code << "}\n";
        return 1;
    } catch (const std::exception&) {
        std::cerr << "{\"status\":\"decode_failed\",\"stage\":\"resource_or_argument_failure\"}\n";
        return 1;
    }
}
