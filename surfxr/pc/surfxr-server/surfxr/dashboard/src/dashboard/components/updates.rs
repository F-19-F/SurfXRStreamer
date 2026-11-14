//! User-requested release checks, independent of streaming and saved sessions.
use eframe::egui::Ui;
use serde::Deserialize;
use std::{sync::mpsc, time::Duration};
use surfxr_common::{semver::Version, SURFXR_VERSION};

const RELEASES: &str = "https://github.com/F-19-F/SurfXRStreamer/releases";
const LATEST: &str = "https://api.github.com/repos/F-19-F/SurfXRStreamer/releases/latest";

#[derive(Debug, PartialEq)]
enum Status {
    NoRelease,
    Current(String),
    Available { version: String, notes: String },
}

#[derive(Deserialize)]
struct Release {
    tag_name: String,
    draft: bool,
    prerelease: bool,
    body: Option<String>,
}

fn interpret(status: u16, body: &str, current: &Version) -> Result<Status, String> {
    match status {
        404 => return Ok(Status::NoRelease),
        403 | 429 => return Err("GitHub request limit reached. Try again later.".into()),
        200 => (),
        _ => return Err(format!("GitHub HTTP {status}")),
    }
    let release: Release = serde_json::from_str(body)
        .map_err(|_| "GitHub returned invalid release information.")?;
    let version = Version::parse(release.tag_name.strip_prefix('v').unwrap_or(&release.tag_name))
        .map_err(|_| "The release tag must use a version such as v1.0.0.")?;
    if release.draft || release.prerelease || !version.pre.is_empty() {
        return Err("The latest release is not a stable version.".into());
    }
    // SemVer build metadata does not change update precedence.
    if version.cmp_precedence(current).is_gt() {
        Ok(Status::Available {
            version: version.to_string(),
            notes: release.body.unwrap_or_default().chars().take(16000).collect(),
        })
    } else {
        Ok(Status::Current(version.to_string()))
    }
}

fn check() -> Result<Status, String> {
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(15)))
        .http_status_as_error(false)
        .build().into();
    let mut response = agent.get(LATEST)
        .header("User-Agent", concat!("SurfXR/", env!("CARGO_PKG_VERSION")))
        .header("Accept", "application/vnd.github+json")
        .call().map_err(|error| error.to_string())?;
    let status = response.status().as_u16();
    let body = response.body_mut().with_config().limit(1024 * 1024)
        .read_to_string().map_err(|error| error.to_string())?;
    interpret(status, &body, &SURFXR_VERSION)
}

#[derive(Default)]
pub struct Updates {
    pending: Option<mpsc::Receiver<Result<Status, String>>>,
    result: Option<Result<Status, String>>,
}

impl Updates {
    pub fn ui(&mut self, ui: &mut Ui) {
        if let Some(receiver) = &self.pending {
            match receiver.try_recv() {
                Ok(result) => { self.result = Some(result); self.pending = None; }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.result = Some(Err("Update check stopped unexpectedly. Try again.".into()));
                    self.pending = None;
                }
                Err(mpsc::TryRecvError::Empty) => (),
            }
        }
        ui.horizontal(|ui| {
            if ui.add_enabled(self.pending.is_none(), eframe::egui::Button::new(crate::i18n::tr("Check for updates"))).clicked() {
                let (sender, receiver) = mpsc::channel();
                self.pending = Some(receiver);
                self.result = None;
                let context = ui.ctx().clone();
                std::thread::spawn(move || { let _ = sender.send(check()); context.request_repaint(); });
            }
            ui.hyperlink_to(crate::i18n::tr("Open SurfXR releases"), RELEASES);
            if self.pending.is_some() { ui.spinner(); }
        });
        if let Some(result) = &self.result {
            match result {
                Ok(Status::NoRelease) => { ui.label(crate::i18n::tr("No stable release is available from this repository yet.")); }
                Ok(Status::Current(version)) => {
                    ui.label(format!("{} ({version})", crate::i18n::tr("No newer stable version is available.")));
                }
                Ok(Status::Available { version, notes }) => {
                    ui.label(format!("{} {version}", crate::i18n::tr("New version available:")));
                    ui.label(crate::i18n::tr("Download matching PC and Frame packages from the release page."));
                    eframe::egui::ScrollArea::vertical().id_salt("release_notes").max_height(160.0)
                        .show(ui, |ui| { ui.label(notes); });
                }
                Err(error) => { ui.label(format!("{} {}", crate::i18n::tr("Update check failed:"), crate::i18n::tr(error))); }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn release(tag: &str, draft: bool, prerelease: bool) -> String {
        serde_json::json!({"tag_name":tag,"draft":draft,"prerelease":prerelease,"body":"Release notes"}).to_string()
    }
    #[test]
    fn versions_use_semver_not_text_order_and_do_not_downgrade() {
        let current = Version::parse("1.9.0").unwrap();
        assert!(matches!(interpret(200, &release("v1.10.0", false, false), &current), Ok(Status::Available { .. })));
        for tag in ["1.8.0", "v1.9.0", "1.9.0+build.2"] {
            assert!(matches!(interpret(200, &release(tag, false, false), &current), Ok(Status::Current(_))));
        }
    }
    #[test]
    fn missing_release_failure_and_unusable_metadata_are_distinct() {
        let current = Version::parse("1.0.0").unwrap();
        assert_eq!(interpret(404, "{}", &current), Ok(Status::NoRelease));
        for status in [403, 429, 500] { assert!(interpret(status, "{}", &current).is_err()); }
        for body in ["bad JSON".into(), "{}".into(), release("latest", false, false),
            release("v2.0.0-beta.1", false, false), release("2.0.0", true, false), release("2.0.0", false, true)] {
            assert!(interpret(200, &body, &current).is_err());
        }
    }
    #[test]
    #[ignore = "Explicit read-only check against the configured GitHub release source"]
    fn live_release_source() { println!("{:#?}", check().unwrap()); }
}
