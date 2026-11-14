// Native read-only API enumeration: no XR instance/session, no VkDevice,
// no decoder setup, no network requests, and no runtime configuration writes.
// --v4l2 opens the Iris decoder for QUERYCAP/ENUM_FMT/G_FMT/TRY_FMT only;
// it never sets a format, allocates queues or starts decoding.
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string quote(const char* text) {
    std::ostringstream out;
    out << '"';
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        if (*p == '"' || *p == '\\') {
            out << '\\' << static_cast<char>(*p);
        } else if (*p < 32) {
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                << static_cast<unsigned>(*p) << std::dec;
        } else {
            out << static_cast<char>(*p);
        }
    }
    out << '"';
    return out.str();
}

void vk_check(VkResult result, const char* api) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(api) + " returned " + std::to_string(result));
    }
}

struct Library {
    void* handle;
    ~Library() { if (handle) dlclose(handle); }
};

struct Instance {
    VkInstance handle = VK_NULL_HANDLE;
    ~Instance() { if (handle) vkDestroyInstance(handle, nullptr); }
};

// An extension name alone does not establish that hardware block matching is
// supported. Query the actual feature and format bits before using image-processing APIs.
std::string motion_processing_report(VkPhysicalDevice device,
                                     const std::vector<VkExtensionProperties>& extensions) {
    const bool available = std::any_of(extensions.begin(), extensions.end(), [](const auto& item) {
        return std::strcmp(item.extensionName, VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME) == 0;
    });
    if (!available) return "{\"extension\":false}";
    VkPhysicalDeviceImageProcessingFeaturesQCOM motion{};
    motion.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &motion;
    vkGetPhysicalDeviceFeatures2(device, &features);
    VkPhysicalDeviceImageProcessingPropertiesQCOM limits{};
    limits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_PROPERTIES_QCOM;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &limits;
    vkGetPhysicalDeviceProperties2(device, &properties);
    std::ostringstream out;
    out << "{\"extension\":true,\"texture_block_match\":" << (motion.textureBlockMatch ? "true" : "false")
        << ",\"max_block_width\":" << limits.maxBlockMatchRegion.width
        << ",\"max_block_height\":" << limits.maxBlockMatchRegion.height
        << ",\"formats\":[";
    bool first = true;
    for (auto format : {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT}) {
        VkFormatProperties3 bits{};
        bits.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
        VkFormatProperties2 format_properties{};
        format_properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        format_properties.pNext = &bits;
        vkGetPhysicalDeviceFormatProperties2(device, format, &format_properties);
        if (!first) out << ',';
        first = false;
        out << "{\"format\":" << format << ",\"block_matching\":"
            << ((bits.optimalTilingFeatures & VK_FORMAT_FEATURE_2_BLOCK_MATCHING_BIT_QCOM) ? "true" : "false")
            << ",\"linear_block_matching\":"
            << ((bits.linearTilingFeatures & VK_FORMAT_FEATURE_2_BLOCK_MATCHING_BIT_QCOM) ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

std::string openxr_report() {
    const char* path = "/opt/steamvr/bin/linuxarm64/libopenxr_loader.so";
    Library loader{dlopen(path, RTLD_NOW | RTLD_LOCAL)};
    if (!loader.handle) {
        throw std::runtime_error("cannot load the Frame SteamVR OpenXR loader");
    }
    const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(
        dlsym(loader.handle, "xrEnumerateInstanceExtensionProperties"));
    if (!enumerate) {
        throw std::runtime_error("OpenXR extension enumeration export is missing");
    }
    std::uint32_t count = 0;
    auto result = enumerate(nullptr, 0, &count, nullptr);
    if (result != XR_SUCCESS || count > 4096) {
        throw std::runtime_error("OpenXR extension count query failed: " + std::to_string(result));
    }
    std::vector<XrExtensionProperties> properties(count);
    for (auto& property : properties) property.type = XR_TYPE_EXTENSION_PROPERTIES;
    result = enumerate(nullptr, count, &count, properties.data());
    if (result != XR_SUCCESS) {
        throw std::runtime_error("OpenXR extension enumeration failed: " + std::to_string(result));
    }
    properties.resize(count);
    std::ostringstream out;
    out << "{\"loader\":" << quote(path) << ",\"instance_created\":false,\"extensions\":[";
    bool first = true;
    for (const auto& property : properties) {
        if (!first) out << ',';
        first = false;
        out << "{\"name\":" << quote(property.extensionName)
            << ",\"version\":" << property.extensionVersion << '}';
    }
    out << "]}";
    return out.str();
}

// Necessary capabilities for Iris -> Vulkan sharing. This query does not
// establish that a particular foreign allocation can actually be imported.
std::string nv12_external_memory_report(VkPhysicalDevice device,const std::vector<VkExtensionProperties>& extensions){
    const auto has=[&](const char* name){return std::any_of(extensions.begin(),extensions.end(),
        [&](const auto& item){return !std::strcmp(item.extensionName,name);});};
    const bool available=has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)&&
        has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)&&has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    std::ostringstream out;
    out<<"{\"extensions_available\":"<<(available?"true":"false")<<",\"allocation_or_import_tested\":false,\"formats\":[";
    bool first=true;
    if(available)for(auto format:{VK_FORMAT_R8_UNORM,VK_FORMAT_R8G8_UNORM,VK_FORMAT_G8_B8R8_2PLANE_420_UNORM}){
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier{};
        modifier.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        modifier.drmFormatModifier=0; // DRM_FORMAT_MOD_LINEAR, not a guessed vendor layout.
        modifier.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo external{};
        external.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        external.pNext=&modifier;external.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageFormatInfo2 info{};info.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        info.pNext=&external;info.format=format;info.type=VK_IMAGE_TYPE_2D;
        info.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;info.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties memory{};memory.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 properties{};properties.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;properties.pNext=&memory;
        const auto result=vkGetPhysicalDeviceImageFormatProperties2(device,&info,&properties);
        VkDrmFormatModifierPropertiesListEXT list{};list.sType=VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        VkFormatProperties2 bits{};bits.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;bits.pNext=&list;
        vkGetPhysicalDeviceFormatProperties2(device,format,&bits);
        std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(std::min(list.drmFormatModifierCount,4096u));
        list.drmFormatModifierCount=modifiers.size();list.pDrmFormatModifierProperties=modifiers.data();
        vkGetPhysicalDeviceFormatProperties2(device,format,&bits);
        bool filter=false;
        for(const auto& item:modifiers)if(item.drmFormatModifier==0)
            filter=(item.drmFormatModifierTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)!=0;
        const auto flags=memory.externalMemoryProperties.externalMemoryFeatures;
        if(!first)out<<',';
        first=false;
        out<<"{\"format\":"<<format<<",\"modifier\":0,\"query_result\":"<<result<<",\"importable\":"
            <<(result==VK_SUCCESS&&(flags&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)?"true":"false")
            <<",\"dedicated_only\":"<<((flags&VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)?"true":"false")
            <<",\"linear_filter\":"<<(filter?"true":"false")<<",\"max_width\":"<<properties.imageFormatProperties.maxExtent.width
            <<",\"max_height\":"<<properties.imageFormatProperties.maxExtent.height<<'}';
    }
    out<<"]}";return out.str();
}

std::string vulkan_report() {
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "surfxr-platform-probe";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    Instance instance;
    vk_check(vkCreateInstance(&create, nullptr, &instance.handle), "vkCreateInstance");
    std::uint32_t count = 0;
    vk_check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    vk_check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "vkEnumeratePhysicalDevices");
    devices.resize(count);
    std::ostringstream out;
    out << "{\"device_created\":false,\"physical_devices\":[";
    bool first_device = true;
    for (auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        std::uint32_t extension_count = 0;
        vk_check(vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr),
                 "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(extension_count);
        vk_check(vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data()),
                 "vkEnumerateDeviceExtensionProperties");
        extensions.resize(extension_count);
        if (!first_device) out << ',';
        first_device = false;
        out << "{\"name\":" << quote(properties.deviceName)
            << ",\"api_version\":\"" << VK_VERSION_MAJOR(properties.apiVersion) << '.'
            << VK_VERSION_MINOR(properties.apiVersion) << '.' << VK_VERSION_PATCH(properties.apiVersion)
            << "\",\"vendor_id\":" << properties.vendorID << ",\"device_id\":" << properties.deviceID
            << ",\"extensions\":[";
        bool first_extension = true;
        for (const auto& extension : extensions) {
            if (!first_extension) out << ',';
            first_extension = false;
            out << quote(extension.extensionName);
        }
        out << "],\"motion_processing\":" << motion_processing_report(device, extensions)
            << ",\"nv12_external_memory\":" << nv12_external_memory_report(device,extensions) << '}';
    }
    out << "]}";
    return out.str();
}

std::string read_line(const std::filesystem::path& path) {
    std::ifstream source(path);
    std::string value;
    std::getline(source, value);
    return value;
}

std::string system_report() {
    utsname system{};
    if (uname(&system) < 0) throw std::runtime_error("uname failed");
    const auto machine = read_line("/sys/devices/soc0/machine");
    const auto soc_id = read_line("/sys/devices/soc0/soc_id");
    const auto revision = read_line("/sys/devices/soc0/revision");
    std::ostringstream out;
    out << "{\"kernel\":" << quote(system.release) << ",\"architecture\":" << quote(system.machine)
        << ",\"soc_machine\":" << quote(machine.c_str()) << ",\"soc_id\":" << quote(soc_id.c_str())
        << ",\"soc_revision\":" << quote(revision.c_str()) << '}';
    return out.str();
}

struct Descriptor {
    int fd;
    ~Descriptor() { if (fd >= 0) close(fd); }
};

int ioctl_error(int fd, unsigned long request, void* value) {
    int result;
    do { result = ioctl(fd, request, value); } while (result < 0 && errno == EINTR);
    return result < 0 ? errno : 0;
}

std::string fourcc(std::uint32_t value) {
    const char text[]{static_cast<char>(value & 255), static_cast<char>((value >> 8) & 255),
                      static_cast<char>((value >> 16) & 255), static_cast<char>((value >> 24) & 255), 0};
    return quote(text);
}

std::string formats_report(int fd, v4l2_buf_type type) {
    std::ostringstream out;
    out << "{\"formats\":[";
    int error = 0;
    unsigned index = 0;
    for (; index < 128; ++index) {
        v4l2_fmtdesc format{};
        format.type = type;
        format.index = index;
        error = ioctl_error(fd, VIDIOC_ENUM_FMT, &format);
        if (error) break;
        if (index) out << ',';
        // Kernel descriptions are fixed arrays, so bound their string length.
        const std::string description(reinterpret_cast<const char*>(format.description),
                                      strnlen(reinterpret_cast<const char*>(format.description),
                                              sizeof(format.description)));
        out << "{\"fourcc\":" << fourcc(format.pixelformat)
            << ",\"description\":" << quote(description.c_str()) << ",\"flags\":" << format.flags << '}';
    }
    out << "],\"enumeration_errno\":" << error << ",\"limit_reached\":"
        << (index == 128 ? "true" : "false") << '}';
    return out.str();
}

std::string try_formats_report(int fd, v4l2_buf_type type) {
    v4l2_format before{};
    before.type = type;
    const auto before_error = ioctl_error(fd, VIDIOC_G_FMT, &before);
    // AV01 is stateful AV1; AV1F is the stateless frame format. Test both
    // without relying on userspace ENUM_FMT validation (e.g. v4l2-ctl).
    const std::vector<std::uint32_t> requested = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
        ? std::vector<std::uint32_t>{V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC, V4L2_PIX_FMT_VP9,
                                    v4l2_fourcc('A','V','0','1'), v4l2_fourcc('A','V','1','F')}
        : std::vector<std::uint32_t>{V4L2_PIX_FMT_NV12, v4l2_fourcc('P','0','1','0')};
    std::ostringstream out;
    out << "{\"attempts\":[";
    bool first = true;
    for (const auto code : requested) {
        v4l2_format format{};
        format.type = type;
        format.fmt.pix_mp.width = 1280;
        format.fmt.pix_mp.height = 720;
        format.fmt.pix_mp.pixelformat = code;
        format.fmt.pix_mp.num_planes = 1;
        const auto error = ioctl_error(fd, VIDIOC_TRY_FMT, &format);
        if (!first) out << ',';
        first = false;
        out << "{\"requested\":" << fourcc(code) << ",\"errno\":" << error << ",\"returned\":";
        if (error) out << "null";
        else out << fourcc(format.fmt.pix_mp.pixelformat);
        out << ",\"exact_format\":" << (!error && format.fmt.pix_mp.pixelformat == code ? "true" : "false") << '}';
    }
    v4l2_format after{};
    after.type = type;
    const auto after_error = ioctl_error(fd, VIDIOC_G_FMT, &after);
    out << "],\"get_format_before_errno\":" << before_error << ",\"get_format_after_errno\":" << after_error
        << ",\"current_format_unchanged\":";
    if (before_error || after_error) out << "null";
    else out << (std::memcmp(&before.fmt.pix_mp, &after.fmt.pix_mp, sizeof(before.fmt.pix_mp)) == 0 ? "true" : "false");
    out << '}';
    return out.str();
}

std::string v4l2_report() {
    std::vector<std::filesystem::path> devices;
    const std::filesystem::path root("/sys/class/video4linux");
    if (std::filesystem::exists(root)) {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (read_line(entry.path() / "name") == "qcom-iris-decoder")
                devices.push_back(std::filesystem::path("/dev") / entry.path().filename());
        }
    }
    std::sort(devices.begin(), devices.end());
    std::ostringstream out;
    out << "{\"decoder_started\":false,\"set_format_used\":false,\"buffers_allocated\":false,\"devices\":[";
    bool first = true;
    for (const auto& path : devices) {
        if (!first) out << ',';
        first = false;
        out << "{\"path\":" << quote(path.c_str());
        Descriptor device{open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC)};
        const auto open_error = device.fd < 0 ? errno : 0;
        out << ",\"open_errno\":" << open_error;
        if (!open_error) {
            v4l2_capability capability{};
            const auto error = ioctl_error(device.fd, VIDIOC_QUERYCAP, &capability);
            out << ",\"querycap_errno\":" << error;
            if (!error) {
                const auto caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                    ? capability.device_caps : capability.capabilities;
                out << ",\"driver_version\":" << capability.version << ",\"device_caps\":" << caps;
                if ((caps & V4L2_CAP_VIDEO_M2M_MPLANE) && (caps & V4L2_CAP_STREAMING)) {
                    out << ",\"compressed_input\":" << formats_report(device.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
                        << ",\"decoded_output\":" << formats_report(device.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                        << ",\"try_compressed_input\":" << try_formats_report(device.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
                        << ",\"try_decoded_output\":" << try_formats_report(device.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
                }
            }
        }
        out << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const bool probe_v4l2 = argc == 2 && std::string(argv[1]) == "--v4l2";
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: surfxr-platform-probe [--v4l2]\n"
                         "Enumerate OpenXR/Vulkan and SoC; --v4l2 also queries Iris formats.\n"
                         "No XR session, decoding, queue allocation or format changes.\n";
            return 0;
        }
        if (argc != 1 && !probe_v4l2) throw std::runtime_error("expected no arguments or --v4l2");
        // Build the report before writing so a failed probe cannot emit partial JSON.
        const auto xr = openxr_report();
        const auto vk = vulkan_report();
        const auto system = system_report();
        const auto video = probe_v4l2 ? v4l2_report() : "null";
        std::cout << "{\"schema_version\":1,\"system\":" << system << ",\"openxr\":" << xr
                  << ",\"vulkan\":" << vk << ",\"v4l2\":" << video << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "platform probe failed: " << error.what() << '\n';
        return 1;
    }
}
