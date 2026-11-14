#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace surfxr {
// Enable interop only as a complete capability set. Plain NV12 remains the
// fallback if an export, layout or import is rejected at runtime.
struct VulkanDmaBufFeatures {
    std::vector<const char*> extensions;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,nullptr,VK_FALSE};
    bool query(VkPhysicalDevice physical){
#ifdef __linux__
        unsigned count=0;
        if(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr)!=VK_SUCCESS||count>4096)return false;
        std::vector<VkExtensionProperties> available(count);
        if(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,available.data())!=VK_SUCCESS)return false;
        for(const auto* name:{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME}){
            if(std::none_of(available.begin(),available.end(),[&](const auto& e){return std::strcmp(e.extensionName,name)==0;}))return false;
            extensions.push_back(name);
        }
        VkPhysicalDeviceFeatures2 info{};info.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;info.pNext=&features;
        vkGetPhysicalDeviceFeatures2(physical,&info);return features.samplerYcbcrConversion==VK_TRUE;
#else
        (void)physical;return false;
#endif
    }
    void attach(VkDeviceCreateInfo& info,std::vector<const char*>& enabled){
        if(info.enabledExtensionCount)enabled.assign(info.ppEnabledExtensionNames,info.ppEnabledExtensionNames+info.enabledExtensionCount);
        enabled.insert(enabled.end(),extensions.begin(),extensions.end());
        features.pNext=const_cast<void*>(info.pNext);info.pNext=&features;
        info.enabledExtensionCount=static_cast<unsigned>(enabled.size());info.ppEnabledExtensionNames=enabled.data();
    }
};
}
