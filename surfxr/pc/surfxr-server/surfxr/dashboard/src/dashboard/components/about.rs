use surfxr_common::SURFXR_VERSION;
use surfxr_gui_common::theme;
use eframe::egui::{Frame, RichText, ScrollArea, Ui};

#[derive(Default)]
pub struct AboutTab {
    #[cfg(not(target_arch = "wasm32"))]
    updates: super::updates::Updates,
}
impl AboutTab {
pub fn ui(&mut self, ui: &mut Ui) {
    ui.label(RichText::new(format!("SurfXR v{}", *SURFXR_VERSION)).size(30.0));
    ui.label(crate::i18n::tr("Steam Frame input, Dynamic Foveated Streaming, unified desktop and VR sessions."));
    ui.hyperlink_to(crate::i18n::tr("SurfXR project on GitHub"), "https://github.com/F-19-F/SurfXRStreamer");
    ui.add_space(10.0);
    #[cfg(not(target_arch = "wasm32"))]
    self.updates.ui(ui);
    ui.add_space(10.0);
    ui.label(crate::i18n::tr("License:"));
    Frame::group(ui.style()).fill(theme::DARKER_BG).inner_margin(theme::FRAME_PADDING)
        .show(ui, |ui| {
            ScrollArea::new([false, true]).id_salt("license_scroll")
                .show(ui, |ui| ui.label(include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../../../LICENSE"))))
        });
}
}
