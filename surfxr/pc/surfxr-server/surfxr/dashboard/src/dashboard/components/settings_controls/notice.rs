use eframe::egui::{Ui, Window};

pub fn notice(ui: &mut Ui, text: &str) {
    let id = ui.make_persistent_id(("compatibility_notes", text));
    let mut open = ui
        .ctx()
        .data_mut(|data| data.get_temp::<bool>(id).unwrap_or(false));
    if ui
        .small_button(crate::i18n::tr("Notes"))
        .on_hover_text(crate::i18n::tr("Show compatibility guidance"))
        .clicked()
    {
        open = !open;
    }
    Window::new(crate::i18n::tr("Compatibility notes"))
        .id(id)
        .open(&mut open)
        .collapsible(false)
        .default_width(420.0)
        .show(ui.ctx(), |ui| {
            ui.label(crate::i18n::tr(text));
        });
    ui.ctx().data_mut(|data| data.insert_temp(id, open));
}
