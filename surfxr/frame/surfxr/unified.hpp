#pragma once
#include "socket.hpp"
#include "surfxr/session.hpp"
#include "surfxr/surfxr_dashboard.hpp"
namespace surfxr::transport {
class UnifiedTransport {
public:
    static bool available(const SessionOptions&,const std::function<bool()>&);
    UnifiedTransport(const SessionOptions&,const SessionCallbacks&,const desktop::Ticket&,const std::atomic_bool*);
    ~UnifiedTransport();
    Socket begin(bool desktop,const Bytes& capabilities);
    Socket video();Socket priority();
    Socket tracking(const Bytes& capabilities);
    bool desktop_keyboard()const;
    bool keyboard()const;void keyboard_visibility(bool);
    void end();void check();
    void close();std::string report()const;
    bool audio()const;bool microphone()const;
private:
    struct State;std::shared_ptr<State> state_;
};
}
