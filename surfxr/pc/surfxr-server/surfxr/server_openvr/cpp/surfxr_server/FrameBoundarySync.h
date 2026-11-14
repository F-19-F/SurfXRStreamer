#pragma once
#include "FrameBoundary.h"
#include <vector>

// One pending room, owned by the existing event loop. No periodic live-room
// repair: external recenter/playspace tools must not start a write/repair loop.
class FrameBoundarySync {
public:
    void request(float width, float depth, const float* points, unsigned count, double now) {
        FrameBoundaryGeometry geometry;
        if (!geometry.build(points,count)) return;
        std::vector<float> key{width,depth};
        key.insert(key.end(),points,points+count*12);
        if (same(key,last_)) { pending_.clear(); return; }
        if (same(key,pending_)) return;
        // The headset has already confirmed two coherent snapshots. Only a new
        // contour or >=20 cm correction reaches this point. Apply immediately.
        pending_=std::move(key);geometry_=geometry;next_attempt_=now;failures_=0;
    }
    bool due(double now) const {
        return !pending_.empty() && now>=next_attempt_;
    }
    template<class Setup>
    bool apply(Setup& setup, double now) {
        if (!due(now)) return false;
        const bool ok=geometry_.commit(setup,pending_[0],pending_[1]);
        if (ok) { last_=std::move(pending_);pending_.clear();failures_=0; }
        else failures_=std::min(failures_+1,5u);
        // Only failed large changes retry; successful or small changes never
        // schedule another write, regardless of elapsed time.
        if (!ok) next_attempt_=now+std::min(30*(1<< (failures_-1)),300);
        return ok;
    }
private:
    static bool same(const std::vector<float>& a,const std::vector<float>& b) {
        if (a.size()!=b.size()) return false;
        for (unsigned i=0;i<a.size();++i) if (std::abs(a[i]-b[i])>=.2f) return false;
        return true;
    }
    FrameBoundaryGeometry geometry_;
    std::vector<float> last_,pending_;
    double next_attempt_=0;
    unsigned failures_=0;
};
