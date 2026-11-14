#pragma once
namespace surfxr {
inline const char* keyboard_controller_hint(bool supported,bool active){
    return !supported?"VR KEYBOARD NEEDS UPDATED SurfXR DAEMON + PC INPUT ENABLED":active?
        "TRIGGER / BUMPER: TYPE   |   GRIP: MOVE / TILT   |   GRIP + STICK: DISTANCE / SIZE":
        "INPUT INACTIVE - CLOSE STEAM MENU";
}
}
