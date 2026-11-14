#pragma once
#include "dashboard_canvas.hpp"

namespace surfxr::dashboard {
inline void battery_card(cairo_t* cr,const Status& s){
  const bool known=s.raw_battery_percent>=0&&s.raw_battery_percent<=100;
  const Color tint=!known?muted:s.raw_battery_percent<=15?Color{1.,.43,.39}:s.raw_battery_percent<=30?Color{1.,.75,.36}:mint;
  rounded(cr,916,110,308,138,24);color(cr,{.10,.135,.18});cairo_fill(cr);
  text(cr,"HEADSET BATTERY",940,141,16,muted,true);
  rounded(cr,940,161,62,32,6);color(cr,tint);cairo_set_line_width(cr,2);cairo_stroke(cr);
  rounded(cr,1005,171,5,12,2);color(cr,tint);cairo_fill(cr);
  if(known&&s.raw_battery_percent>0){
    rounded(cr,945,166,52.*s.raw_battery_percent/100.,22,2);color(cr,tint);cairo_fill(cr);
  }
  if(s.raw_battery_charging&&known){
    color(cr,{.08,.13,.16});cairo_move_to(cr,974,163);cairo_line_to(cr,963,179);cairo_line_to(cr,973,179);
    cairo_line_to(cr,968,191);cairo_line_to(cr,981,174);cairo_line_to(cr,972,174);cairo_close_path(cr);cairo_fill(cr);
  }
  text(cr,known?std::to_string(s.raw_battery_percent)+"%":"--",1028,192,38,tint,true);
  text(cr,!known?"Battery unavailable":s.raw_battery_charging?"Charging  /  Actual level":"Actual battery level",940,223,16,muted);
}
}
