#include "../../pc/daemon/backend.hpp"
#include "surfxr/surfxr_desktop_layout.hpp"
#include "../shared/protocol/desktop_capture_checks.hpp"
#include <future>
#include <array>

namespace {
using namespace surfxr::transport;
using namespace surfxr::transport::desktop;
Bytes take(surfxr::daemon::Backend& backend){
    std::array<std::uint8_t,128> buffer{};const auto n=backend.receive(buffer.data(),buffer.size());
    require(n>0,"backend_fixture_receive");return Bytes(buffer.begin(),buffer.begin()+n);
}
void backpressure(bool stop){
    surfxr::daemon::Backend backend(true,1,[](const Bytes&){return true;},[](const std::uint8_t*,std::size_t){return true;});
    const auto keepalive=control(2);
    for(unsigned i=0;i<64;++i)require(backend.push(keepalive),"backend_fixture_fill");
    auto producer=std::async(std::launch::async,[&]{return backend.push(control(1));});
    require(producer.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout,"backend_burst_disconnected_or_unbounded");
    if(stop)backend.stop();else require(take(backend)==keepalive,"backend_fifo_first");
    require(producer.wait_for(std::chrono::seconds(1))==std::future_status::ready&&producer.get(),"backend_backpressure_not_released");
    if(!stop){for(unsigned i=0;i<63;++i)require(take(backend)==keepalive,"backend_fifo_changed");require(take(backend)==control(1),"backend_lost_control_edge");}
}
Bytes skeleton(unsigned hand,unsigned stamp){
    Writer body;body.integer(0x5644534b,4);body.integer(1,4);body.integer(hand,4);body.integer(0,4);body.integer(stamp,8);
    Writer record(true);record.integer(11,4);record.blob(body.bytes);return record.bytes;
}
Bytes buttons(bool pressed,float axis){
    Writer record(true);record.integer(6,4);record.integer(2,8);record.integer(1,8);record.integer(0,4);record.boolean(pressed);
    record.integer(2,8);record.integer(1,4);record.real(axis);return record.bytes;
}
void vr_snapshots(){
    surfxr::daemon::Backend b(false,1,[](const Bytes&){return true;},[](const std::uint8_t*,std::size_t){return true;});
    for(unsigned i=0;i<1000;++i){require(b.push(buttons(false,float(i)/1000))&&b.push(skeleton(0,i))&&b.push(skeleton(1,i))&&b.push(control(2))&&b.push(control(1)),"backend_busy_vr_snapshot_burst");}
    require(take(b)==buttons(false,.999f)&&take(b)==skeleton(0,999)&&take(b)==skeleton(1,999)&&take(b)==control(2)&&take(b)==control(1),"backend_vr_snapshot_latest");
    for(const auto& record:{buttons(true,.1f),skeleton(0,1),buttons(true,.2f),buttons(false,.3f),skeleton(0,2)})require(b.push(record),"backend_vr_edge_push");
    for(const auto& record:{buttons(true,.2f),skeleton(0,1),buttons(false,.3f),skeleton(0,2)})require(take(b)==record,"backend_vr_snapshot_crossed_button_edge");
}
}
void daemon_backend_checks(){
    desktop_capture_checks();
    vr_snapshots();
    backpressure(false);backpressure(true);
    surfxr::daemon::Backend backend(true,1,[](const Bytes&){return true;},[](const std::uint8_t*,std::size_t){return true;});
    Event move;move.kind=EventKind::Move;
    for(unsigned i=0;i<1000;++i){move.u=float(i)/1000;require(backend.push(input(move,4)),"backend_move_burst_failed");}
    const auto first=input(move,4);
    Event key;key.kind=EventKind::Key;key.a=30;key.b=1;
    const auto down=input(key,4);key.b=0;const auto up=input(key,4);
    require(backend.push(down)&&backend.push(input(move,4))&&backend.push(up),"backend_key_edges_failed");
    move.stream=1;const auto other_stream=input(move,4),other_layout=input(move,5);
    require(backend.push(other_stream)&&backend.push(other_layout),"backend_move_boundaries_failed");
    for(const auto& expected:{first,down,first,up,other_stream,other_layout})require(take(backend)==expected,"backend_move_crossed_edge_stream_or_layout");
    require(!backend.push(Bytes(4*1024*1024+1)),"backend_byte_budget_not_bounded");
}
