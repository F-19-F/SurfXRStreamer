use super::*;

pub fn automatic_bindings(
    source_set: &HashSet<u64>,
    destination_set: &HashSet<u64>,
    config: &AutomaticButtonMappingConfig,
) -> HashMap<u64, Vec<BindingTarget>> {
    let s_set = source_set;
    let d_set = destination_set;

    let mut bindings = HashMap::new();

    // Menu buttons
    if s_set.contains(&*LEFT_MENU_CLICK_ID) {
        let click = click(*LEFT_MENU_CLICK_ID);
        if d_set.contains(&*LEFT_MENU_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(click, click, config));
        } else if d_set.contains(&*LEFT_SYSTEM_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                click,
                ct(s_set, *LEFT_SYSTEM_CLICK_ID, *LEFT_SYSTEM_TOUCH_ID),
                config,
            ));
        }
    }
    if s_set.contains(&*LEFT_SYSTEM_CLICK_ID) {
        let click = click(*LEFT_SYSTEM_CLICK_ID);
        if d_set.contains(&*LEFT_SYSTEM_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                click,
                ct(s_set, *LEFT_SYSTEM_CLICK_ID, *LEFT_SYSTEM_TOUCH_ID),
                config,
            ));
        } else if d_set.contains(&*LEFT_MENU_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(click, click, config));
        }
    }
    if s_set.contains(&*RIGHT_MENU_CLICK_ID) {
        let click = click(*RIGHT_MENU_CLICK_ID);
        if d_set.contains(&*RIGHT_MENU_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(click, click, config));
        } else if d_set.contains(&*RIGHT_SYSTEM_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                click,
                ct(s_set, *RIGHT_SYSTEM_CLICK_ID, *RIGHT_SYSTEM_TOUCH_ID),
                config,
            ));
        }
    }
    if s_set.contains(&*RIGHT_SYSTEM_CLICK_ID) {
        let click = click(*RIGHT_SYSTEM_CLICK_ID);
        if d_set.contains(&*RIGHT_SYSTEM_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                click,
                ct(s_set, *RIGHT_SYSTEM_CLICK_ID, *RIGHT_SYSTEM_TOUCH_ID),
                config,
            ));
        } else if d_set.contains(&*RIGHT_MENU_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(click, click, config));
        }
    }

    // A/X buttons
    if s_set.contains(&*LEFT_X_CLICK_ID) {
        let source = ct(s_set, *LEFT_X_CLICK_ID, *LEFT_X_TOUCH_ID);
        if d_set.contains(&*LEFT_X_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *LEFT_X_CLICK_ID, *LEFT_X_TOUCH_ID),
                config,
            ));
        } else if d_set.contains(&*LEFT_A_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *LEFT_A_CLICK_ID, *LEFT_A_TOUCH_ID),
                config,
            ));
        } else if d_set.contains(&*LEFT_TRACKPAD_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *LEFT_TRACKPAD_CLICK_ID, *LEFT_TRACKPAD_TOUCH_ID),
                config,
            ));
        }
    }
    if s_set.contains(&*RIGHT_A_CLICK_ID) {
        let source = ct(s_set, *RIGHT_A_CLICK_ID, *RIGHT_A_TOUCH_ID);
        if d_set.contains(&*RIGHT_A_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *RIGHT_A_CLICK_ID, *RIGHT_A_TOUCH_ID),
                config,
            ));
        } else if d_set.contains(&*RIGHT_TRACKPAD_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *RIGHT_TRACKPAD_CLICK_ID, *RIGHT_TRACKPAD_TOUCH_ID),
                config,
            ));
        }
    }

    // B/Y buttons
    if s_set.contains(&*LEFT_Y_CLICK_ID) {
        let source = ct(s_set, *LEFT_Y_CLICK_ID, *LEFT_Y_TOUCH_ID);
        if d_set.contains(&*LEFT_Y_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *LEFT_Y_CLICK_ID, *LEFT_Y_TOUCH_ID),
                config,
            ));
        } else if d_set.contains(&*LEFT_B_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *LEFT_B_CLICK_ID, *LEFT_B_TOUCH_ID),
                config,
            ));
        }
    }
    if s_set.contains(&*RIGHT_B_CLICK_ID) && d_set.contains(&*RIGHT_B_CLICK_ID) {
        bindings.extend(map_button_pair_automatic(
            ct(s_set, *RIGHT_B_CLICK_ID, *RIGHT_B_TOUCH_ID),
            ct(d_set, *RIGHT_B_CLICK_ID, *RIGHT_B_TOUCH_ID),
            config,
        ));
    }

    // Squeeze buttons
    if (s_set.contains(&*LEFT_SQUEEZE_CLICK_ID) || s_set.contains(&*LEFT_SQUEEZE_VALUE_ID))
        && (d_set.contains(&*LEFT_SQUEEZE_CLICK_ID) || d_set.contains(&*LEFT_SQUEEZE_VALUE_ID))
    {
        bindings.extend(map_button_pair_automatic(
            ctvf(
                s_set,
                *LEFT_SQUEEZE_CLICK_ID,
                *LEFT_SQUEEZE_TOUCH_ID,
                *LEFT_SQUEEZE_VALUE_ID,
                *LEFT_SQUEEZE_FORCE_ID,
            ),
            ctvf(
                d_set,
                *LEFT_SQUEEZE_CLICK_ID,
                *LEFT_SQUEEZE_TOUCH_ID,
                *LEFT_SQUEEZE_VALUE_ID,
                *LEFT_SQUEEZE_FORCE_ID,
            ),
            config,
        ));
    }
    if s_set.contains(&*LEFT_SQUEEZE_SENSOR_VALUE_ID)
        && d_set.contains(&*LEFT_SQUEEZE_SENSOR_VALUE_ID)
    {
        bindings.extend(map_button_pair_automatic(
            value(*LEFT_SQUEEZE_SENSOR_VALUE_ID),
            value(*LEFT_SQUEEZE_SENSOR_VALUE_ID),
            config,
        ));
    }
    if (s_set.contains(&*RIGHT_SQUEEZE_CLICK_ID) || s_set.contains(&*RIGHT_SQUEEZE_VALUE_ID))
        && (d_set.contains(&*RIGHT_SQUEEZE_CLICK_ID) || d_set.contains(&*RIGHT_SQUEEZE_VALUE_ID))
    {
        bindings.extend(map_button_pair_automatic(
            ctvf(
                s_set,
                *RIGHT_SQUEEZE_CLICK_ID,
                *RIGHT_SQUEEZE_TOUCH_ID,
                *RIGHT_SQUEEZE_VALUE_ID,
                *RIGHT_SQUEEZE_FORCE_ID,
            ),
            ctvf(
                d_set,
                *RIGHT_SQUEEZE_CLICK_ID,
                *RIGHT_SQUEEZE_TOUCH_ID,
                *RIGHT_SQUEEZE_VALUE_ID,
                *RIGHT_SQUEEZE_FORCE_ID,
            ),
            config,
        ));
    }
    if s_set.contains(&*RIGHT_SQUEEZE_SENSOR_VALUE_ID)
        && d_set.contains(&*RIGHT_SQUEEZE_SENSOR_VALUE_ID)
    {
        bindings.extend(map_button_pair_automatic(
            value(*RIGHT_SQUEEZE_SENSOR_VALUE_ID),
            value(*RIGHT_SQUEEZE_SENSOR_VALUE_ID),
            config,
        ));
    }
    // Trigger buttons
    if (s_set.contains(&*LEFT_TRIGGER_CLICK_ID) || s_set.contains(&*LEFT_TRIGGER_VALUE_ID))
        && (d_set.contains(&*LEFT_TRIGGER_CLICK_ID) || d_set.contains(&*LEFT_TRIGGER_VALUE_ID))
    {
        bindings.extend(map_button_pair_automatic(
            ctv(
                s_set,
                *LEFT_TRIGGER_CLICK_ID,
                *LEFT_TRIGGER_TOUCH_ID,
                *LEFT_TRIGGER_VALUE_ID,
            ),
            ctv(
                d_set,
                *LEFT_TRIGGER_CLICK_ID,
                *LEFT_TRIGGER_TOUCH_ID,
                *LEFT_TRIGGER_VALUE_ID,
            ),
            config,
        ));
    }
    if s_set.contains(&*LEFT_TRIGGER_SENSOR_VALUE_ID)
        && d_set.contains(&*LEFT_TRIGGER_SENSOR_VALUE_ID)
    {
        bindings.extend(map_button_pair_automatic(
            value(*LEFT_TRIGGER_SENSOR_VALUE_ID),
            value(*LEFT_TRIGGER_SENSOR_VALUE_ID),
            config,
        ));
    }
    if (s_set.contains(&*RIGHT_TRIGGER_CLICK_ID) || s_set.contains(&*RIGHT_TRIGGER_VALUE_ID))
        && (d_set.contains(&*RIGHT_TRIGGER_CLICK_ID) || d_set.contains(&*RIGHT_TRIGGER_VALUE_ID))
    {
        bindings.extend(map_button_pair_automatic(
            ctv(
                s_set,
                *RIGHT_TRIGGER_CLICK_ID,
                *RIGHT_TRIGGER_TOUCH_ID,
                *RIGHT_TRIGGER_VALUE_ID,
            ),
            ctv(
                d_set,
                *RIGHT_TRIGGER_CLICK_ID,
                *RIGHT_TRIGGER_TOUCH_ID,
                *RIGHT_TRIGGER_VALUE_ID,
            ),
            config,
        ));
    }
    if s_set.contains(&*RIGHT_TRIGGER_SENSOR_VALUE_ID)
        && d_set.contains(&*RIGHT_TRIGGER_SENSOR_VALUE_ID)
    {
        bindings.extend(map_button_pair_automatic(
            value(*RIGHT_TRIGGER_SENSOR_VALUE_ID),
            value(*RIGHT_TRIGGER_SENSOR_VALUE_ID),
            config,
        ));
    }
    // Thumbsticks
    if s_set.contains(&*LEFT_THUMBSTICK_X_ID) {
        let x = value(*LEFT_THUMBSTICK_X_ID);
        let y = value(*LEFT_THUMBSTICK_Y_ID);
        if d_set.contains(&*LEFT_THUMBSTICK_X_ID) {
            bindings.extend(map_button_pair_automatic(x, x, config));
            bindings.extend(map_button_pair_automatic(y, y, config));
        } else if d_set.contains(&*LEFT_TRACKPAD_X_ID) {
            bindings.extend(map_button_pair_automatic(
                x,
                value(*LEFT_TRACKPAD_X_ID),
                config,
            ));
            bindings.extend(map_button_pair_automatic(
                y,
                value(*LEFT_TRACKPAD_Y_ID),
                config,
            ));
        }
    }
    if s_set.contains(&*LEFT_THUMBSTICK_CLICK_ID) {
        let source = ct(s_set, *LEFT_THUMBSTICK_CLICK_ID, *LEFT_THUMBSTICK_TOUCH_ID);
        if d_set.contains(&*LEFT_THUMBSTICK_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *LEFT_THUMBSTICK_CLICK_ID, *LEFT_THUMBSTICK_TOUCH_ID),
                config,
            ));
        } else if d_set.contains(&*LEFT_TRACKPAD_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *LEFT_TRACKPAD_CLICK_ID, *LEFT_TRACKPAD_TOUCH_ID),
                config,
            ));
        }
    }
    if s_set.contains(&*RIGHT_THUMBSTICK_X_ID) {
        let x = value(*RIGHT_THUMBSTICK_X_ID);
        let y = value(*RIGHT_THUMBSTICK_Y_ID);
        if d_set.contains(&*RIGHT_THUMBSTICK_X_ID) {
            bindings.extend(map_button_pair_automatic(x, x, config));
            bindings.extend(map_button_pair_automatic(y, y, config));
        } else if d_set.contains(&*RIGHT_TRACKPAD_X_ID) {
            bindings.extend(map_button_pair_automatic(
                x,
                value(*RIGHT_TRACKPAD_X_ID),
                config,
            ));
            bindings.extend(map_button_pair_automatic(
                y,
                value(*RIGHT_TRACKPAD_Y_ID),
                config,
            ));
        }
    }
    if s_set.contains(&*RIGHT_THUMBSTICK_CLICK_ID) {
        let source = ct(
            s_set,
            *RIGHT_THUMBSTICK_CLICK_ID,
            *RIGHT_THUMBSTICK_TOUCH_ID,
        );
        if d_set.contains(&*RIGHT_THUMBSTICK_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(
                    d_set,
                    *RIGHT_THUMBSTICK_CLICK_ID,
                    *RIGHT_THUMBSTICK_TOUCH_ID,
                ),
                config,
            ));
        } else if d_set.contains(&*RIGHT_TRACKPAD_CLICK_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                ct(d_set, *RIGHT_TRACKPAD_CLICK_ID, *RIGHT_TRACKPAD_TOUCH_ID),
                config,
            ));
        }
    }

    // Thumbrests
    if s_set.contains(&*LEFT_THUMBREST_TOUCH_ID) {
        let source = value(*LEFT_THUMBREST_TOUCH_ID);
        if d_set.contains(&*LEFT_THUMBREST_TOUCH_ID) {
            bindings.extend(map_button_pair_automatic(source, source, config));
        } else if d_set.contains(&*LEFT_TRACKPAD_TOUCH_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                value(*LEFT_TRACKPAD_TOUCH_ID),
                config,
            ));
        }
    }
    if s_set.contains(&*RIGHT_THUMBREST_TOUCH_ID) {
        let source = value(*RIGHT_THUMBREST_TOUCH_ID);
        if d_set.contains(&*RIGHT_THUMBREST_TOUCH_ID) {
            bindings.extend(map_button_pair_automatic(source, source, config));
        } else if d_set.contains(&*RIGHT_TRACKPAD_TOUCH_ID) {
            bindings.extend(map_button_pair_automatic(
                source,
                value(*RIGHT_TRACKPAD_TOUCH_ID),
                config,
            ));
        }
    }

    bindings
}
