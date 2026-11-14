#pragma once
#include "network.hpp"
#include "surfxr/surfxr_dashboard.hpp"
#include <optional>

namespace surfxr::transport::desktop {
void run_media_session(const Options&,Control&,Socket,sockaddr_in,const Key&,const Key&,
    bool,const std::optional<Ticket>&,const std::atomic_bool&);
}
