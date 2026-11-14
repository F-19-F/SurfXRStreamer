#pragma once
#include "surfxr/build_features.hpp"
namespace surfxr {
// Preserve the HUD call shape without any transport state or implementation.
class BandwidthOverlay {
public:
    struct Summary {};
    Summary summary(unsigned long long)const{return {};}
};
}
