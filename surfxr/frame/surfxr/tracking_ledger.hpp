#pragma once
#include "wire.hpp"
#include "surfxr/vr_wire.hpp"
#include <deque>
#include <memory>
#include <mutex>

namespace surfxr::transport {
// decoded video with the exact head/IPD/FOV sent for that timestamp.
class TrackingLedger {
public:
    void sent(std::uint64_t timestamp,const TrackingFrame& frame);
    std::shared_ptr<const VrVideoMetadata> metadata(std::uint64_t timestamp,unsigned width,unsigned height,std::uint64_t received,std::uint64_t image_timestamp=0);
private:
    struct Entry {std::uint64_t timestamp;TrackingFrame frame;};
    std::mutex mutex_;std::deque<Entry> entries_;
};
}
