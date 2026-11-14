#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace surfxr {
enum class ImageQualityId : std::uint8_t {DesktopFilter=0,VrUpscaling=1,VrSharpening=2,VrSharpeningEnabled=3,VrTimewarp=4};
struct ImageQualityChange {ImageQualityId id=ImageQualityId::DesktopFilter;int value=0;};
struct ImageQualityPreferences {
    int desktop_filter=1; // 0 bilinear; 1 stable mip-filtered sampling.
    bool vr_upscaling=false;
    int vr_sharpening=25; // Strength persists independently of its enable switch.
    bool vr_sharpening_enabled=false; // Opt in to the additional local pass.
    bool vr_timewarp=true; // Preserve the existing SURFXR rotational timewarp default.
};
bool operator==(const ImageQualityPreferences& a,const ImageQualityPreferences& b) noexcept;
bool operator!=(const ImageQualityPreferences& a,const ImageQualityPreferences& b) noexcept;
bool valid_image_quality(const ImageQualityPreferences& preferences) noexcept;
bool valid_image_quality_change(ImageQualityChange change) noexcept;
int image_quality_value(const ImageQualityPreferences& preferences,ImageQualityId id) noexcept;
// Rejected changes leave the existing preferences intact.
bool apply_image_quality_change(ImageQualityPreferences& preferences,ImageQualityChange change) noexcept;
enum class ImageQualityStoreStatus {Ok,Missing,Invalid,IoError,Disabled};
const char* image_quality_store_status_name(ImageQualityStoreStatus status) noexcept;
std::string default_image_quality_path();
bool parse_image_quality(const std::string& text,ImageQualityPreferences& output) noexcept;
std::string encode_image_quality(const ImageQualityPreferences& preferences);
ImageQualityStoreStatus load_image_quality(const std::string& path,ImageQualityPreferences& output) noexcept;
struct ImageQualityStoreStats {
    std::uint64_t submitted=0,coalesced=0,writes=0,failures=0,rejected=0;
    ImageQualityStoreStatus last_status=ImageQualityStoreStatus::Ok;
    bool enabled=false,finished=false;
};
// submit copies/coalesces accepted local values. Filesystem I/O runs on one
// private worker; finish joins it after the last producer has stopped.
class ImageQualityStore {
public:
    explicit ImageQualityStore(std::string path);
    ~ImageQualityStore();
    ImageQualityStore(const ImageQualityStore&)=delete;
    ImageQualityStore& operator=(const ImageQualityStore&)=delete;
    bool submit(const ImageQualityPreferences& preferences) noexcept;
    ImageQualityStoreStats statistics() const;
    ImageQualityStoreStats finish() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace surfxr
