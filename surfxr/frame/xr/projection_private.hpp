#pragma once
#include "surfxr/projection_renderer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace surfxr::projection_detail {
template<class T>T info(VkStructureType type){T v{};v.sType=type;return v;}
inline void check(VkResult r,const char* call){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(call)+": VkResult "+std::to_string(r));}
constexpr unsigned max_sources=4,segments=64,max_vertices=max_sources*segments*6;
// Keep the push-constant layout stable; sampling is reserved. Pipeline
// specialization owns the render mode and no display phase affects pixels.
struct Camera {std::array<float,16> vp;float eye[4];float sampling[4];};
static_assert(sizeof(Camera)==96,"projection push constant layout");
inline unsigned mip_levels(unsigned w,unsigned h){unsigned levels=1;while(w>1||h>1){w=std::max(1u,w/2);h=std::max(1u,h/2);++levels;}return levels;}
inline std::uint64_t mip_bytes(unsigned w,unsigned h){std::uint64_t bytes=0;for(;;){bytes+=std::uint64_t{w}*h*4;if(w==1&&h==1)return bytes;w=std::max(1u,w/2);h=std::max(1u,h/2);}}
}
namespace surfxr {
using namespace projection_detail;
struct ProjectionRenderer::Impl {
    VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};unsigned family{},width{},height{};VkFormat format{};
    unsigned max_samples=4;
    VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT;
    struct Image {VkImage image{};VkDeviceMemory memory{};VkImageView view{};};
    Image color,depth;VkFormat depth_format=VK_FORMAT_D32_SFLOAT;
    VkRenderPass pass{},mip_pass{},nv12_mip_pass{};VkFormat raw_format{};
    VkDescriptorSetLayout descriptor_layout{};VkPipelineLayout layout{};VkPipeline pipeline{},quality_pipeline{},mip_pipeline{},nv12_mip_pipeline{},nv12_linear_mip_pipeline{};
    VkPipeline nv12_base_pipeline{},nv12_linear_base_pipeline{};
    VkSampler sampler{},mip_sampler{};VkDescriptorPool descriptors{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    VkBuffer vertices{};VkDeviceMemory vertex_memory{};void* mapped=nullptr;bool coherent=false;
    std::map<VkImageView,VkFramebuffer> framebuffers;double wait_ms=0;
    struct MipCache {Image image;VkImageView base_view{},raw_view{};VkFramebuffer framebuffer{},raw_framebuffer{};
        unsigned width=0,height=0,levels=0;std::uint64_t generation=0;bool valid=false,initialized=false,raw=false,quality=false;};
    std::array<MipCache,max_sources> caches{};ProjectionSamplingStats stats;
    bool quality_supported=false,image_format_list=false;
    VkQueryPool timestamps{};unsigned timestamp_bits=0,timestamp_interval=0;double timestamp_tick_ns=0;
    unsigned memory_type(unsigned bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred);
    void image(Image& target,VkFormat fmt,VkImageUsageFlags usage,VkImageAspectFlags aspect);
    VkShaderModule shader(const std::uint32_t* data,std::size_t size);
    void initialize();
    void release_cache(MipCache& cache);
    void prepare_cache(MipCache& cache,const ProjectionSource& source,bool quality);
    void barrier(VkImage image,unsigned base,unsigned count,VkImageLayout old_layout,VkImageLayout new_layout,
                 VkAccessFlags before,VkAccessFlags after,VkPipelineStageFlags from,VkPipelineStageFlags to);
    void source_transitions(const std::vector<ProjectionSource>& sources,const std::array<bool,max_sources>& update,bool quality,bool start);
    void update_mips(MipCache& cache,VkDescriptorSet source,const ProjectionSource& input);
    VkFramebuffer framebuffer(VkImageView view);
    void render(const std::vector<ProjectionSource>& sources,const std::array<ProjectionEye,2>& eyes,ProjectionSampling sampling,std::uint64_t display_frame);
    ~Impl();
};
}
