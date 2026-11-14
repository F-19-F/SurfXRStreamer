//! Dashboard-only Windows styling and local system font fallbacks.
use eframe::egui::{self, Color32, CornerRadius, FontData, FontDefinitions, FontFamily, Stroke};

pub fn apply(ctx: &egui::Context) {
    surfxr_gui_common::theme::set_theme(ctx);
    let mut style = (*ctx.global_style()).clone();
    style.spacing.item_spacing = egui::vec2(12.0, 10.0);
    style.spacing.button_padding = egui::vec2(12.0, 7.0);
    style.spacing.interact_size.y = 32.0;
    style.spacing.slider_width = 170.0;
    style.interaction.tooltip_delay = 0.4;
    ctx.set_global_style(style);
    let mut visuals = egui::Visuals::dark();
    visuals.panel_fill = Color32::from_rgb(30, 30, 30);
    visuals.window_fill = Color32::from_rgb(36, 36, 36);
    visuals.override_text_color = Some(Color32::from_rgb(235, 235, 235));
    visuals.weak_text_color = Some(Color32::from_rgb(175, 175, 175));
    visuals.extreme_bg_color = Color32::from_rgb(26, 26, 26);
    visuals.selection.bg_fill = Color32::from_rgb(0, 85, 145);
    visuals.selection.stroke = Stroke::new(1.0, Color32::WHITE);
    visuals.hyperlink_color = Color32::from_rgb(96, 205, 255);
    for widget in [
        &mut visuals.widgets.inactive,
        &mut visuals.widgets.hovered,
        &mut visuals.widgets.active,
        &mut visuals.widgets.open,
    ] {
        widget.corner_radius = CornerRadius::same(4);
    }
    visuals.widgets.active.bg_fill = visuals.selection.bg_fill;
    visuals.widgets.noninteractive.corner_radius = CornerRadius::same(6);
    ctx.set_visuals(visuals);
    install_fonts(ctx);
}

pub fn install_fonts(ctx: &egui::Context) {
    let mut fonts = FontDefinitions::default();
    // Read installed fonts only; Windows font files must not be redistributed.
    #[cfg(windows)]
    if let Some(root) = std::env::var_os("WINDIR") {
        let directory = std::path::PathBuf::from(root).join("Fonts");
        for (name, file, primary) in [
            ("extended-ui", "segoeui.ttf", true),
            ("extended-cjk", "msyh.ttc", false),
            ("extended-symbols", "seguisym.ttf", false),
        ] {
            if let Ok(bytes) = std::fs::read(directory.join(file)) {
                fonts
                    .font_data
                    .insert(name.into(), FontData::from_owned(bytes).into());
                for family in [FontFamily::Proportional, FontFamily::Monospace] {
                    let names = fonts.families.entry(family.clone()).or_default();
                    if primary && family == FontFamily::Proportional {
                        names.insert(0, name.into());
                    } else {
                        names.push(name.into());
                    }
                }
            }
        }
    }
    ctx.set_fonts(fonts);
}
