#pragma once
#include "windows.hpp"
#include "mouse_activity.hpp"
#include <set>
#include <chrono>

namespace surfxr::transport::desktop {
struct ImeCandidate;
class Input {
public:
    explicit Input(Display display,bool arbitrate=true):display_(display),arbitrate_(arbitrate){desktop_.attach();if(arbitrate_)require(activity_.start(),"desktop_mouse_hook_failed");}
    ~Input(){release();activity_.stop();}
    void apply(const Event&);
    void click(Display region,float u,float v);
    void click(const ImeCandidate&,float u,float v);
    bool tick();
    void prefer_pc(bool value){arbiter_.prefer_pc(value);}
    void release()noexcept;
    void displays(const std::vector<Display>& values){require(!values.empty()&&values.size()<=4,"desktop_input_displays");release();displays_=values;display_=values.front();}
private:
    PointerArbiter arbiter_;MouseActivity activity_{arbiter_};
    InputDesktop desktop_;Display display_;std::set<unsigned> keys_,unicode_;
    bool arbitrate_;
    std::vector<Display> displays_{display_};
    std::array<bool,3> buttons_{};bool position_=false;float u_=0,v_=0;
    std::vector<INPUT> pending_release_;
    bool injection_ready_=false;std::chrono::steady_clock::time_point next_report_{};
    INPUT move()const;
    static INPUT button(unsigned,bool);
    static INPUT key(unsigned,bool);
    bool send(INPUT*,unsigned);
};
}
