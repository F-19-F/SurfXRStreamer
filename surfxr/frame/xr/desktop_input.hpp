#pragma once
#include "surfxr/tracking.hpp"

namespace surfxr {
// Local desktop aliases only; keep the raw VR snapshot and its wire mapping intact.
inline void apply_desktop_buttons(UiHand& ui,const TrackingController& controller,unsigned hand){
    if(controller.profile!=TrackingProfile::SteamFrame)return;
    const auto down=controller.buttons&controller.buttons_active;
    ui.select=ui.select||(down&(ButtonBumper|ButtonTrigger|(hand?ButtonA:ButtonDpadDown)));
    ui.right_click=ui.right_click||(down&(hand?ButtonB:ButtonDpadLeft));
}
}
