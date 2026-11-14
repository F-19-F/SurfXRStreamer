#include <openvr.h>
#include "../surfxr_server/FrameBoundarySync.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <limits>

struct Runtime {
    vr::HmdMatrix34_t standing{},seated{};
    std::vector<vr::HmdQuad_t> walls;
    float width=0,depth=0;
    unsigned reverts=0,commits=0,seated_writes=0;
    bool fail=false;
    void RevertWorkingCopy(){++reverts;}
    void SetWorkingStandingZeroPoseToRawTrackingPose(const vr::HmdMatrix34_t* p){standing=*p;}
    void SetWorkingSeatedZeroPoseToRawTrackingPose(const vr::HmdMatrix34_t* p){seated=*p;++seated_writes;}
    void SetWorkingPlayAreaSize(float w,float d){width=w;depth=d;}
    void SetWorkingCollisionBoundsInfo(vr::HmdQuad_t* p,unsigned n){walls.assign(p,p+n);}
    bool CommitWorkingCopy(vr::EChaperoneConfigFile file){assert(file==vr::EChaperoneConfigFile_Live);++commits;return !fail;}
    vr::HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose(){
        auto result=standing;for(unsigned a=0;a<3;++a)result.m[a][3]*=-1;return result;
    }
};
static std::vector<float> room(const std::vector<std::array<float,2>>& corners,float floor=0) {
    std::vector<float> points;
    for(unsigned i=0;i<corners.size();++i){
        auto a=corners[i],b=corners[(i+1)%corners.size()];
        for(float v:{a[0],floor,a[1],a[0],floor+2,a[1],b[0],floor+2,b[1],b[0],floor,b[1]})points.push_back(v);
    }
    return points;
}
static void near(float a,float b){assert(std::abs(a-b)<1e-4f);}
int main(){
    // Origin is deliberately at a corner; unequal wall tessellation must not
    // pull the area centroid toward the more densely sampled wall.
    auto points=room({{0,0},{1,0},{2,0},{4,0},{4,6},{0,6}},.25f);
    FrameBoundaryGeometry geometry;
    assert(geometry.build(points.data(),unsigned(points.size()/12)));
    near(geometry.standing.m[0][3],2);near(geometry.standing.m[1][3],.25f);near(geometry.standing.m[2][3],3);
    Runtime runtime;runtime.seated.m[0][3]=17;runtime.seated.m[1][3]=1.4f;
    assert(geometry.commit(runtime,4,6));assert(runtime.commits==1&&runtime.reverts==1);
    assert(runtime.seated_writes==0);near(runtime.seated.m[0][3],17);near(runtime.seated.m[1][3],1.4f);
    for(unsigned q=0;q<geometry.count;++q)for(unsigned p=0;p<4;++p)for(unsigned a=0;a<3;++a)
        near(runtime.standing.m[a][3]+runtime.walls[q].vCorners[p].v[a],points[q*12+p*3+a]);
    // The head and walls use the same raw->standing translation.
    const auto raw_to_standing=runtime.GetRawZeroPoseToStandingAbsoluteTrackingPose();
    near(2+raw_to_standing.m[0][3],0);near(3+raw_to_standing.m[2][3],0);
    runtime.fail=true;auto reverts=runtime.reverts;assert(!geometry.commit(runtime,4,6)&&runtime.reverts==reverts+2);
    runtime.fail=false;assert(geometry.commit(runtime,4,6));
    FrameBoundarySync sync;Runtime queued;
    auto request=[&](const std::vector<float>& value,double time){sync.request(4,6,value.data(),unsigned(value.size()/12),time);};
    request(points,0);assert(sync.due(0));
    assert(sync.apply(queued,2)&&queued.commits==1);
    queued.standing.m[0][3]+=7; // External recenter cannot start a repair loop.
    for(unsigned i=0;i<10000;++i){request(points,3+i*.001);assert(!sync.due(3+i*.001));}
    for(unsigned i=1;i<10;++i){auto jitter=points;for(unsigned j=0;j<jitter.size();j+=3)jitter[j]+=.02f*i;
        request(jitter,14+i);assert(!sync.due(14+i));}
    auto moved=points;for(unsigned j=0;j<moved.size();j+=3)moved[j]+=.1f;
    request(moved,25);assert(!sync.due(25)&&!sync.due(3600));
    auto newest=moved;for(unsigned j=0;j<newest.size();j+=3)newest[j]+=.15f;
    request(newest,33);assert(sync.due(33));
    queued.fail=true;assert(!sync.apply(queued,33)&&!sync.due(62.99)&&sync.due(63));
    request(newest,50);assert(!sync.due(50)); // Duplicate cannot accelerate a failed write.
    assert(!sync.apply(queued,63)&&!sync.due(122.99)&&sync.due(123));
    queued.fail=false;assert(sync.apply(queued,123));near(queued.standing.m[0][3],2.25f);
    assert(!sync.due(100000));
    request(points,130);request(newest,131);assert(!sync.due(200)); // Change reverted before applying.
    auto relocated=newest;for(unsigned j=0;j<relocated.size();j+=3)relocated[j]+=1;
    request(relocated,132);assert(sync.due(132)&&sync.apply(queued,132));
    near(queued.standing.m[0][3],3.25f); // Large change has no fixed cooldown.
    auto resized=room({{0,0},{4,0},{4,6},{0,6}});
    request(resized,133);assert(sync.due(133)&&sync.apply(queued,133));
    assert(runtime.seated_writes==0&&queued.seated_writes==0);
    // An asymmetric triangle distinguishes area center from vertex selection
    // or an axis-aligned bounding-box midpoint. Winding does not change center.
    for(auto corners:{std::vector<std::array<float,2>>{{10,20},{16,20},{10,23}},
                      std::vector<std::array<float,2>>{{10,23},{16,20},{10,20}}}){
        points=room(corners);assert(geometry.build(points.data(),3));
        near(geometry.standing.m[0][3],12);near(geometry.standing.m[2][3],21);
    }
    points=room({{0,0},{1,0},{2,0}});assert(!geometry.build(points.data(),3));
    points=room({{0,0},{4,0},{4,6},{0,6}});
    assert(!geometry.build(points.data(),3)); // Missing closing wall.
    std::copy(points.begin(),points.begin()+12,points.begin()+12);
    assert(!geometry.build(points.data(),4)); // Duplicate instead of a wall.
    points[0]=std::numeric_limits<float>::quiet_NaN();assert(!geometry.build(points.data(),3));
    assert(!geometry.build(nullptr,3)&&!geometry.build(points.data(),65));
    std::cout<<"Frame boundary center, large-change-only commits and failure backoff checks passed\n";
}
