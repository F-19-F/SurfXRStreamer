use super::*;

#[test]
fn quick_diagnostics_exposes_startup_and_updates_log_level() {
    let mut app = Harness::new();
    app.size = egui::vec2(1200.0, 1600.0);
    let before = configuration(app.session.session());
    app.click("Startup & diagnostics");
    #[cfg(windows)]
    app.position("Start Dashboard in the tray when I sign in");
    app.position("Log level");
    assert_eq!(configuration(app.session.session()), before);
    app.click_in_row("Log level", "Warning");
    app.click("Debug");
    assert_eq!(app.session.settings().extra.logging.log_level, surfxr_common::LogSeverity::Debug);
    assert_eq!(app.changes, 1);
}
