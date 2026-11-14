#pragma once
#include "surfxr/surfxr_dashboard.hpp"
#include "surfxr/session.hpp"
namespace surfxr::transport {
desktop::Ticket request_desktop(const SessionOptions&,const std::function<bool()>& cancelled);
}
