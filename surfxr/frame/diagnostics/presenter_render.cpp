#include "presenter_private.hpp"
namespace surfxr::diagnostics {
void Presenter::events() {
    for (;;) {
        auto event = xr_info<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        const auto result = xrPollEvent(instance_, &event);
        if (result == XR_EVENT_UNAVAILABLE) return;
        xr_check(result, "xrPollEvent");
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            finished_ = true; return;
        }
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
        if (changed.session != session_) continue;
        state_ = changed.state;
        if (state_ == XR_SESSION_STATE_READY && !running_ && !exit_requested_) {
            auto begin = xr_info<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            xr_check(xrBeginSession(session_, &begin), "xrBeginSession");
            running_ = true;
        } else if (state_ == XR_SESSION_STATE_STOPPING) {
            if (running_) xr_check(xrEndSession(session_), "xrEndSession");
            running_ = false;
            finished_ = true;
        } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
            finished_ = true;
        }
    }
}

bool Presenter::frame() {
    auto wait = xr_info<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
    auto state = xr_info<XrFrameState>(XR_TYPE_FRAME_STATE);
    xr_check(xrWaitFrame(session_, &wait, &state), "xrWaitFrame");
    auto begin = xr_info<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
    xr_check(xrBeginFrame(session_, &begin), "xrBeginFrame");
    auto end = xr_info<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = blend_;
    bool acquired = false, waited = false, end_attempted = false;
    try {
        auto quad = xr_info<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
        const XrCompositionLayerBaseHeader* layer = nullptr;
        if (state.shouldRender && !exit_requested_) {
            auto acquire = xr_info<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
            std::uint32_t index = 0;
            xr_check(xrAcquireSwapchainImage(swapchain_, &acquire, &index), "xrAcquireSwapchainImage");
            acquired = true;
            auto image_wait = xr_info<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
            image_wait.timeout = 100'000'000;
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            XrResult result;
            do { result = xrWaitSwapchainImage(swapchain_, &image_wait); }
            while (result == XR_TIMEOUT_EXPIRED && Clock::now() < deadline && !interrupted);
            if (result == XR_TIMEOUT_EXPIRED) throw std::runtime_error("Swapchain image wait timed out");
            xr_check(result, "xrWaitSwapchainImage");
            waited = true;
            copy_image(index);
            auto release = xr_info<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            xr_check(xrReleaseSwapchainImage(swapchain_, &release), "xrReleaseSwapchainImage");
            acquired = waited = false;
            quad.space = space_;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = swapchain_;
            quad.subImage.imageRect.extent = {static_cast<std::int32_t>(options_.width), static_cast<std::int32_t>(options_.height)};
            quad.pose.orientation.w = 1.0f;
            quad.pose.position.z = -2.0f;
            const float longest = static_cast<float>(std::max(options_.width, options_.height));
            quad.size = {2.0f * options_.width / longest, 2.0f * options_.height / longest};
            layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            end.layerCount = 1;
            end.layers = &layer;
        }
        end_attempted = true;
        xr_check(xrEndFrame(session_, &end), "xrEndFrame");
        return end.layerCount != 0;
    } catch (...) {
        // If work was submitted before an error, finish it before returning an image.
        if (device_) vkDeviceWaitIdle(device_);
        if (acquired && waited) {
            auto release = xr_info<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            xrReleaseSwapchainImage(swapchain_, &release);
        }
        // A failed xrEndFrame has already discarded/processed this frame.
        // Never resubmit it. Only balance a begin that has not reached end.
        if (!end_attempted) {
            end.layerCount = 0;
            end.layers = nullptr;
            xrEndFrame(session_, &end);
        }
        throw;
    }
}

Presenter::Presenter(Options options) : options_(std::move(options)) {}

Presenter::~Presenter() {
    if (device_) vkDeviceWaitIdle(device_);
    if (mapped_) vkUnmapMemory(device_, memory_);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (buffer_) vkDestroyBuffer(device_, buffer_, nullptr);
    if (memory_) vkFreeMemory(device_, memory_, nullptr);
    if (swapchain_) xrDestroySwapchain(swapchain_);
    if (space_) xrDestroySpace(space_);
    if (session_) xrDestroySession(session_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (vk_instance_) vkDestroyInstance(vk_instance_, nullptr);
    if (instance_) xrDestroyInstance(instance_);
}

void Presenter::initialize(std::vector<std::uint8_t>& rgba) {
    create_xr();
    create_vulkan();
    create_session(rgba);
    create_transfer(rgba);
}

void Presenter::run() {
    const auto startup = Clock::now();
    auto started = Clock::time_point{};
    auto exit_deadline = Clock::time_point{};
    std::uint64_t frames = 0;
    while (!finished_) {
        events();
        if (finished_) break;
        const auto now = Clock::now();
        if (running_ && started == Clock::time_point{}) started = now;
        if (interrupted || (started != Clock::time_point{} && now - started >= std::chrono::seconds(options_.seconds))) {
            if (running_ && !exit_requested_) {
                xr_check(xrRequestExitSession(session_), "xrRequestExitSession");
                exit_requested_ = true;
                exit_deadline = now + std::chrono::seconds(3);
            } else if (!running_) break;
        }
        if (exit_requested_ && now >= exit_deadline) throw std::runtime_error("Runtime did not stop the session within 3 seconds");
        if (!running_) {
            if (now - startup > std::chrono::seconds(10)) throw std::runtime_error("Runtime did not make the session ready within 10 seconds");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (frame()) ++frames;
    }
    if (!frames && !interrupted) throw std::runtime_error("Session ended without presenting a quad");
    std::cout << "{\"quad_frames\":" << frames << ",\"width\":" << options_.width
              << ",\"height\":" << options_.height << ",\"requested_seconds\":" << options_.seconds
              << ",\"space\":\"VIEW\",\"z_meters\":-2,\"network_used\":false}\n";
}

}
