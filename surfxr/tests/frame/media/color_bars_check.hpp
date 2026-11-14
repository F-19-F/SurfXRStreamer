#pragma once
#include "../../color_bars.hpp"
#include "surfxr/video_decoder.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>

namespace surfxr::tests {
// Only the shared synthetic fixture is accepted. Sampling well inside each
// constant bar excludes encoder chroma filtering at the boundaries.
class ColorBarsCheck {
    static constexpr unsigned radius=8,tolerance=8;
    std::uint64_t frames_=0,channels_=0;
    unsigned max_error_=0;
    std::array<std::array<std::uint64_t,3>,color_bars::colors.size()> last_sums_{};
public:
    void observe(const VideoFrame& frame){
        if(frame.width!=color_bars::width||frame.height!=color_bars::height||
           frame.stride!=color_bars::width*4||frame.rgba_size()!=std::size_t(frame.stride)*frame.height||
           !frame.rgba_data()||frame.nv12_storage||frame.packed_source)
            throw VideoDecodeError("color_bars_requires_shared_rgba_fixture");
        last_sums_={};
        for(unsigned bar=0;bar<color_bars::colors.size();++bar){
            const unsigned center_x=bar*color_bars::bar_width+color_bars::bar_width/2;
            const unsigned center_y=color_bars::height/2;
            for(unsigned y=center_y-radius;y<center_y+radius;++y)
                for(unsigned x=center_x-radius;x<center_x+radius;++x){
                    const auto* pixel=frame.rgba_data()+std::size_t(y)*frame.stride+x*4;
                    if(pixel[3]!=255)throw VideoDecodeError("color_bars_alpha_not_opaque");
                    for(unsigned channel=0;channel<3;++channel){
                        const unsigned actual=pixel[channel],expected=color_bars::colors[bar][channel];
                        const unsigned error=actual>expected?actual-expected:expected-actual;
                        max_error_=std::max(max_error_,error);++channels_;
                        last_sums_[bar][channel]+=actual;
                    }
                }
        }
        ++frames_;
    }
    bool passed()const noexcept{return frames_&&max_error_<=tolerance;}
    void report(std::ostream& out)const{
        out<<",\"color_bars\":{\"passed\":"<<(passed()?"true":"false")
           <<",\"checked_frames\":"<<frames_<<",\"channels_checked\":"<<channels_
           <<",\"max_code_error\":"<<max_error_<<",\"tolerance\":"<<tolerance
           <<",\"last_bar_means\":[";
        for(unsigned bar=0;bar<last_sums_.size();++bar){
            if(bar)out<<',';
            out<<'[';
            for(unsigned channel=0;channel<3;++channel){
                if(channel)out<<',';
                out<<double(last_sums_[bar][channel])/(4*radius*radius);
            }
            out<<']';
        }
        out<<"]}";
    }
};
}
