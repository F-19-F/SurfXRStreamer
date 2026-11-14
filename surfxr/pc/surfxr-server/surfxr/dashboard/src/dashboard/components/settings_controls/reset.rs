use eframe::{
    egui::{self, Button, Layout, Response, Ui},
    emath::Align,
};

pub fn reset_button(ui: &mut Ui, enabled: bool, default_str: &str) -> Response {
    // Reserve a real cell even when a preceding editor uses all available width.
    // A bare right-to-left layout can place Reset on top of that editor.
    let size = egui::vec2(ui.available_width().max(70.0), ui.spacing().interact_size.y);
    ui.allocate_ui_with_layout(size, Layout::right_to_left(Align::Center), |ui| {
        ui.add_space(5.0);

        let height = ui.spacing().interact_size.y;
        ui.add_enabled(
            enabled,
            Button::new(crate::i18n::tr("Reset")).min_size(egui::vec2(height, height)),
        )
        .on_hover_text(format!("{} {}", crate::i18n::tr("Reset to"), crate::i18n::tr(default_str.trim_matches('"'))))
    })
    .inner
}
