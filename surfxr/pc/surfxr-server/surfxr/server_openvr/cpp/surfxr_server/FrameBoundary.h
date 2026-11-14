#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

// Included after this translation unit's OpenVR client namespace is selected.
// Driver poses remain in Frame STAGE (= PC raw space). Only SteamVR's standing
// transform moves to the room's area centroid; collision vertices are expressed
// relative to that origin so their physical location is preserved.
struct FrameBoundaryGeometry {
    vr::HmdMatrix34_t standing = {{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    std::array<vr::HmdQuad_t,64> walls{};
    unsigned count = 0;

    bool build(const float* points, unsigned n) {
        if (!points || n < 3 || n > walls.size()) return false;
        double area2=0, xsum=0, zsum=0;
        float floor=200;
        std::array<std::array<float,4>,64> edges{};
        for (unsigned q=0; q<n; ++q) {
            const auto* p=points+q*12;
            for (unsigned i=0; i<12; ++i)
                if (!std::isfinite(p[i]) || std::abs(p[i])>200) return false;
            // A vertical wall has two distinct projected endpoints. Preserve
            // face winding, without weighting densely tessellated wall spans.
            unsigned end=0;
            for (unsigned i=1; i<4; ++i)
                if (std::hypot(p[i*3]-p[0],p[i*3+2]-p[2])>1e-5f) { end=i; break; }
            if (!end) return false;
            edges[q]={p[0],p[2],p[end*3],p[end*3+2]};
            const double cross=double(p[0])*p[end*3+2]-double(p[end*3])*p[2];
            area2+=cross; xsum+=(double(p[0])+p[end*3])*cross;
            zsum+=(double(p[2])+p[end*3+2])*cross;
            for (unsigned i=0; i<4; ++i) floor=std::min(floor,p[i*3+1]);
        }
        // Require closed, consistently wound contours. An incomplete or
        // duplicated wall set has no meaningful polygon area center.
        for (unsigned i=0;i<n;++i) {
            unsigned joins=0;
            for (unsigned j=0;j<n;++j) if(i!=j &&
                std::hypot(edges[i][2]-edges[j][0],edges[i][3]-edges[j][1])<1e-3f) ++joins;
            if(joins!=1) return false;
        }
        if (std::abs(area2)<1e-6) return false;
        const float center[3]={float(xsum/(3*area2)),floor,float(zsum/(3*area2))};
        for (unsigned a=0; a<3; ++a) {
            if (!std::isfinite(center[a]) || std::abs(center[a])>200) return false;
            standing.m[a][3]=center[a];
        }
        count=n;
        for (unsigned q=0; q<n; ++q) for (unsigned p=0; p<4; ++p) for (unsigned a=0; a<3; ++a)
            walls[q].vCorners[p].v[a]=points[q*12+p*3+a]-center[a];
        return true;
    }
    template<class Setup>
    bool commit(Setup& setup, float width, float depth) {
        setup.RevertWorkingCopy();
        setup.SetWorkingStandingZeroPoseToRawTrackingPose(&standing);
        // Boundary sync owns Standing; preserve the user's seated calibration.
        // OVR Advanced Settings treats seated reset events as head recentering.
        setup.SetWorkingPlayAreaSize(width,depth);
        setup.SetWorkingCollisionBoundsInfo(walls.data(),count);
        if (setup.CommitWorkingCopy(vr::EChaperoneConfigFile_Live)) return true;
        setup.RevertWorkingCopy();
        return false;
    }
};
