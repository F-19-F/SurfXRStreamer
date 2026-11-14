#pragma once
// Explicit, offline diagnostic: display a tightly packed RGBA8 file in OpenXR.
// No network/decoder code. Running with no arguments only prints help.
// XR_KHR_vulkan_enable2 reference:
// https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable2.html
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace surfxr::diagnostics {
using Clock = std::chrono::steady_clock;
inline volatile std::sig_atomic_t interrupted = 0;
inline void signal_handler(int) { interrupted = 1; }

inline void xr_check(XrResult result, const char* call) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(call) + ": XrResult " + std::to_string(result));
}
inline void vk_check(VkResult result, const char* call) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(call) + ": VkResult " + std::to_string(result));
}
template<class T> T xr_info(XrStructureType type) { T value{}; value.type = type; return value; }
template<class T> T vk_info(VkStructureType type) { T value{}; value.sType = type; return value; }
template<class T> T extension(XrInstance instance, const char* name) {
    PFN_xrVoidFunction value = nullptr;
    xr_check(xrGetInstanceProcAddr(instance, name, &value), name);
    if (!value) throw std::runtime_error(std::string("Missing OpenXR function: ") + name);
    return reinterpret_cast<T>(value);
}

struct Options { std::string input; std::uint32_t width = 0, height = 0, seconds = 0; };
inline std::uint32_t number(const std::string& value, const char* name, std::uint32_t maximum) {
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result == 0 || result > maximum)
        throw std::runtime_error(std::string(name) + " must be an integer in 1.." + std::to_string(maximum));
    return result;
}
inline Options parse(int argc, char** argv) {
    Options result;
    bool input_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (i + 1 >= argc) throw std::runtime_error("Missing value for " + name);
        const std::string value = argv[++i];
        if (name == "--input" && !input_seen) { result.input = value; input_seen = true; }
        else if (name == "--width" && !result.width) result.width = number(value, "width", 8192);
        else if (name == "--height" && !result.height) result.height = number(value, "height", 8192);
        else if (name == "--seconds" && !result.seconds) result.seconds = number(value, "seconds", 30);
        else throw std::runtime_error("Unknown or duplicate argument: " + name);
    }
    if (result.input.empty() || !result.width || !result.height || !result.seconds)
        throw std::runtime_error("All of --input --width --height --seconds are required");
    return result;
}
inline std::vector<std::uint8_t> read_rgba(const Options& options) {
    const std::uint64_t count = std::uint64_t{options.width} * options.height * 4;
    constexpr std::uint64_t limit = 256ULL * 1024 * 1024;
    if (count > limit || count > std::numeric_limits<std::size_t>::max() ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("RGBA image exceeds the 256 MiB input limit");
    std::ifstream file(options.input, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open RGBA input file");
    const auto length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) != count)
        throw std::runtime_error("RGBA file must contain exactly width * height * 4 bytes");
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    if (!file || file.gcount() != static_cast<std::streamsize>(count) ||
        file.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("RGBA file read failed or file changed during reading");
    return bytes;
}

class Presenter {
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    VkInstance vk_instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkan2KHR> images_;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode blend_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool running_ = false, finished_ = false, exit_requested_ = false;
    const Options options_;

    void create_xr();

    void create_vulkan();

    void create_session(std::vector<std::uint8_t>& rgba);

    void create_transfer(const std::vector<std::uint8_t>& rgba);

    void copy_image(std::uint32_t index);

    void events();

    bool frame();

public:
    explicit Presenter(Options options);
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    ~Presenter();
    void initialize(std::vector<std::uint8_t>& rgba);
    void run();
};
}
