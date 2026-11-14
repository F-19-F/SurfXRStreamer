//! Shared confirmation for panel and tray exit requests; only confirmation permits teardown.
use eframe::egui::{self, Color32, Context, RichText, Ui, ViewportCommand};

#[derive(Default)]
pub struct ExitConfirmation {
    open: bool,
}

pub fn warning_button(ui: &mut Ui, label: &str) -> egui::Response {
    ui.add(
        egui::Button::new(
            RichText::new(crate::i18n::tr(label)).color(Color32::from_rgb(255, 180, 180)),
        )
        .fill(Color32::from_rgb(85, 30, 35))
        .stroke(egui::Stroke::new(1.0, Color32::from_rgb(175, 65, 70))),
    )
}

impl ExitConfirmation {
    pub fn request(&mut self, ctx: &Context) {
        self.open = true;
        ctx.send_viewport_cmd(ViewportCommand::Visible(true));
        ctx.send_viewport_cmd(ViewportCommand::Minimized(false));
        ctx.send_viewport_cmd(ViewportCommand::Focus);
        ctx.request_repaint();
    }

    pub fn ui(&mut self, ctx: &Context, close_steamvr: bool) -> bool {
        if !self.open {
            return false;
        }
        let mut confirmed = false;
        let mut cancelled = false;
        let response = egui::Modal::new(egui::Id::new("confirm_dashboard_exit")).show(ctx, |ui| {
            ui.set_width(360.0);
            ui.heading(crate::i18n::tr("Exit SurfXR?"));
            ui.add_space(8.0);
            ui.label(crate::i18n::tr(
                "This will close SurfXR and disconnect desktop streaming.",
            ));
            if close_steamvr {
                ui.label(crate::i18n::tr("SteamVR will also close with Dashboard."));
            }
            ui.add_space(16.0);
            ui.horizontal(|ui| {
                cancelled = ui.button(crate::i18n::tr("Cancel")).clicked();
                confirmed = warning_button(ui, "Confirm exit").clicked();
            });
        });
        if cancelled || response.should_close() {
            self.open = false;
            return false;
        }
        if confirmed {
            self.open = false;
            ctx.request_repaint();
        }
        confirmed
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui::{Event, Modifiers, PointerButton, Pos2, RawInput, Rect, epaint::Shape};

    fn frame(
        ctx: &Context,
        dialog: &mut ExitConfirmation,
        events: Vec<Event>,
    ) -> (egui::FullOutput, bool) {
        let mut confirmed = false;
        let output = ctx.run_ui(
            RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, egui::vec2(800.0, 600.0))),
                events,
                ..Default::default()
            },
            |ui| {
                confirmed = dialog.ui(ui.ctx(), true);
            },
        );
        (output, confirmed)
    }

    fn click(ctx: &Context, dialog: &mut ExitConfirmation, label: &str) -> bool {
        for _ in 0..3 {
            assert!(!frame(ctx, dialog, vec![]).1);
        }
        let output = frame(ctx, dialog, vec![]).0;
        let pos = output
            .shapes
            .iter()
            .find_map(|shape| match &shape.shape {
                Shape::Text(text) if text.galley.text() == label => {
                    Some(text.pos + text.galley.size() / 2.0)
                }
                _ => None,
            })
            .expect("Visible dialog button");
        assert!(
            !frame(
                ctx,
                dialog,
                vec![
                    Event::PointerMoved(pos),
                    Event::PointerButton {
                        pos,
                        button: PointerButton::Primary,
                        pressed: true,
                        modifiers: Modifiers::NONE,
                    }
                ]
            )
            .1
        );
        frame(
            ctx,
            dialog,
            vec![Event::PointerButton {
                pos,
                button: PointerButton::Primary,
                pressed: false,
                modifiers: Modifiers::NONE,
            }],
        )
        .1
    }

    #[test]
    fn requesting_and_cancelling_never_exit_but_confirming_does() {
        crate::i18n::set_language(crate::i18n::Language::English);
        let ctx = Context::default();
        let mut dialog = ExitConfirmation::default();
        dialog.request(&ctx);
        assert!(!click(&ctx, &mut dialog, "Cancel"));
        assert!(!dialog.open);
        dialog.request(&ctx);
        assert!(click(&ctx, &mut dialog, "Confirm exit"));
        assert!(!dialog.open);
        assert!(!frame(&ctx, &mut dialog, vec![]).1);
    }

    #[test]
    fn escape_cancels_confirmation() {
        let ctx = Context::default();
        let mut dialog = ExitConfirmation::default();
        dialog.request(&ctx);
        frame(&ctx, &mut dialog, vec![]);
        assert!(
            !frame(
                &ctx,
                &mut dialog,
                vec![Event::Key {
                    key: egui::Key::Escape,
                    physical_key: None,
                    pressed: true,
                    repeat: false,
                    modifiers: Modifiers::NONE,
                }]
            )
            .1
        );
        assert!(!dialog.open);
    }
}
