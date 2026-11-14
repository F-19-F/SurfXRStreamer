//! UI adapter for the existing release installer. No alternate transport or installation logic.
use eframe::egui::Ui;
use std::{path::{Path, PathBuf}, process::{Command, Stdio}, sync::mpsc};

use crate::dashboard::ServerRequest;
use surfxr_packets::ClientConnectionsAction;

pub struct FrameInstaller {
    target: String,
    release: String,
    direct: bool,
    result: String,
    plan: bool,
    uninstall: bool,
    confirm_uninstall: bool,
    pending: Option<mpsc::Receiver<Result<String, String>>>,
    awaiting_trust: Option<(String, std::time::Instant)>,
    transcript: String,
}

fn script_near(exe: &Path) -> Option<PathBuf> {
    let folder = exe.parent()?;
    let bundled = folder.join("frame-installer/Install-Frame.ps1");
    if bundled.is_file() { return Some(bundled); }
    // Developer builds use the same scripts/ entry as manual script installation.
    folder.ancestors().find_map(|root| {
        let script = root.join("scripts/Install-Frame.ps1");
        (root.join("scripts/Build-PC.ps1").is_file() && script.is_file()).then_some(script)
    })
}

fn valid_target(target: &str) -> bool {
    let parts: Vec<_> = target.split('@').collect();
    parts.len() == 2 && parts.iter().all(|s| !s.is_empty() && !s.starts_with('-') &&
        s.bytes().all(|c| c.is_ascii_alphanumeric() || b"_.-".contains(&c)))
}

fn installer_command(script: &Path, release: &Path, target: &str, direct: bool, plan: bool, uninstall: bool)
    -> Result<Command, String>
{
    if !valid_target(target) { return Err("Use user@hostname or user@IPv4".into()); }
    if !script.is_file() { return Err("Installer script is missing".into()); }
    if !release.join("release-manifest.json").is_file() {
        return Err("Choose the extracted SurfXR Frame package containing release-manifest.json".into());
    }
    #[cfg(windows)]
    let shell = PathBuf::from(std::env::var_os("SystemRoot").ok_or("SystemRoot is missing")?)
        .join("System32/WindowsPowerShell/v1.0/powershell.exe");
    #[cfg(not(windows))]
    let shell = PathBuf::from("pwsh");
    let mut command = Command::new(shell);
    // A Dashboard launched by PowerShell 7 may inherit its module path. Let
    // Windows PowerShell initialize its own modules (including Get-FileHash).
    #[cfg(windows)]
    command.env_remove("PSModulePath");
    command.args(["-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]).arg(script)
        .args(["-Action", if uninstall { "uninstall" } else { "install" }, "-FrameSsh", target, "-Link", if direct { "direct" } else { "lan" }])
        .arg("-ReleaseDirectory").arg(release);
    if !uninstall { command.arg("-PairOnInstall"); }
    if plan { command.arg("-Plan"); }
    if plan { command.stdout(Stdio::piped()).stderr(Stdio::piped()); }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // The user explicitly opens this interactive installer; SSH owns password/host-key prompts.
        command.creation_flags(if plan { 0x08000000 } else { 0x00000010 });
    }
    Ok(command)
}

impl FrameInstaller {
    pub fn new() -> Self {
        let release = std::env::current_exe().ok().and_then(|exe| exe.parent().map(|p| p.join("frame-client")))
            .unwrap_or_default();
        Self { target: "steamos@frame".into(), release: release.to_string_lossy().into_owned(),
            direct: false, result: String::new(), plan: false, uninstall: false, confirm_uninstall: false, pending: None, awaiting_trust: None, transcript: String::new() }
    }

    pub fn poll(&mut self) -> Vec<ServerRequest> {
        if self.awaiting_trust.as_ref().is_some_and(|(_, start)| start.elapsed().as_secs() >= 10) {
            self.awaiting_trust = None;
            self.result = crate::i18n::tr("Installation completed, but pairing was not confirmed. Retry installation or trust this Frame in Devices.");
        }
        let Some(receiver) = &self.pending else { return vec![]; };
        let result = match receiver.try_recv() {
            Ok(result) => result,
            Err(mpsc::TryRecvError::Empty) => return vec![],
            Err(_) => Err("Installer worker stopped unexpectedly".into()),
        };
        self.pending = None;
        match result {
            Ok(output) if self.plan || self.uninstall => { self.result = output; vec![] }
            Ok(output) => match pairing_receipt(&output) {
                Ok((hostname, ip)) => {
                    self.result = crate::i18n::tr("Installation complete. Saving Frame pairing...");
                    self.transcript = output.lines().filter(|line| !line.starts_with("SURFXR_PAIR_ID ")).collect::<Vec<_>>().join("\n");
                    self.awaiting_trust = Some((hostname.clone(), std::time::Instant::now()));
                    vec![ServerRequest::UpdateClientList { hostname: hostname.clone(),
                        action: ClientConnectionsAction::AddIfMissing { trusted: true, manual_ips: vec![ip] } },
                        ServerRequest::UpdateClientList { hostname, action: ClientConnectionsAction::Trust },
                        ServerRequest::GetSession]
                }
                Err(error) => { self.result = format!("{error}\n{output}"); vec![] }
            },
            Err(error) => { self.result = error; vec![] }
        }
    }
    pub fn busy(&self) -> bool { self.pending.is_some() || self.awaiting_trust.is_some() }
    pub fn update_session(&mut self, session: &surfxr_session::SessionConfig) {
        if self.awaiting_trust.as_ref().is_some_and(|(name, _)|
            session.client_connections.get(name).is_some_and(|client| client.trusted)) {
            self.result = crate::i18n::tr("Frame is installed and paired. Choose Desktop or SteamVR on Frame.");
            self.awaiting_trust = None;
        }
    }
    pub fn ui(&mut self, ui: &mut Ui) {
        if self.pending.is_some() || self.awaiting_trust.is_some() { ui.ctx().request_repaint_after(std::time::Duration::from_millis(250)); }
        ui.heading(crate::i18n::tr("Steam Frame client"));
        for step in [
            "1. Enable developer mode and SSH on Steam Frame. Connect Frame and this PC to the same network or direct link.",
            "2. Enter the Frame SSH login and select the extracted SurfXR ARM64 release folder below.",
            "3. Preview the plan, then install. Complete SSH host-key and password prompts in the installer window.",
            "4. Installation pairs this Frame with this Dashboard. Choose Desktop or SteamVR on Frame.",
        ] { ui.label(crate::i18n::tr(step)); }
        ui.add_space(8.0);
        ui.add_enabled_ui(!self.busy(), |ui| {
            ui.label(crate::i18n::tr("Frame SSH (user@host)"));
            ui.text_edit_singleline(&mut self.target);
            ui.label(crate::i18n::tr("Extracted Frame release folder"));
            ui.text_edit_singleline(&mut self.release);
            ui.checkbox(&mut self.direct, crate::i18n::tr("Use direct link (otherwise LAN)"));
            ui.horizontal(|ui| {
                if ui.button(crate::i18n::tr("Preview installation plan")).clicked() { self.start(true, false); }
                if ui.button(crate::i18n::tr("Install / update Steam Frame")).clicked() { self.start(false, false); }
                if ui.button(crate::i18n::tr("Uninstall Steam Frame client")).clicked() { self.confirm_uninstall = true; }
            });
            if self.confirm_uninstall {
                ui.label(crate::i18n::tr("Uninstall the client from this Frame? Saved connections and settings will be kept."));
                ui.label(self.target.trim());
                ui.horizontal(|ui| {
                    if ui.button(crate::i18n::tr("Cancel")).clicked() { self.confirm_uninstall = false; }
                    if ui.button(crate::i18n::tr("Confirm uninstall")).clicked() { self.start(false, true); }
                });
            }
        });
        if self.pending.is_some() { ui.spinner(); ui.label(crate::i18n::tr(if self.plan {
            "Generating command preview locally. No connection to Frame."
        } else { "Installer is running. Complete prompts in its window." })); }
        if !self.result.is_empty() {
            ui.group(|ui| {
                if ui.button(crate::i18n::tr("Copy result")).clicked() { ui.ctx().copy_text(format!("{}\n{}", self.result, self.transcript)); }
                eframe::egui::ScrollArea::both().id_salt("frame_install_result").max_height(260.0).show(ui, |ui| {
                    ui.add(eframe::egui::Label::new(eframe::egui::RichText::new(&self.result).monospace())
                        .selectable(true).wrap_mode(eframe::egui::TextWrapMode::Extend));
                    if !self.transcript.is_empty() { ui.add(eframe::egui::Label::new(&self.transcript).selectable(true).wrap()); }
                });
            });
        }
        ui.label(crate::i18n::tr("Manual installation remains available through Install-Frame.ps1 and Install-SteamFrame.cmd in the release package."));
        ui.separator();
    }

    fn start(&mut self, plan: bool, uninstall: bool) {
        let Some(script) = std::env::current_exe().ok().as_deref().and_then(script_near) else {
            self.result = crate::i18n::tr("Dashboard installer adapter is missing. Update the complete PC installation, including frame-installer."); return;
        };
        let mut command = match installer_command(&script, Path::new(&self.release), self.target.trim(), self.direct, plan, uninstall) {
            Ok(command) => command, Err(e) => { self.result = e; return; }
        };
        let (sender, receiver) = mpsc::channel();
        self.plan = plan;
        self.uninstall = uninstall;
        self.confirm_uninstall = false;
        self.pending = Some(receiver);
        self.result.clear();
        self.transcript.clear();
        self.awaiting_trust = None;
        std::thread::spawn(move || {
            let result = if plan { capture(&mut command) } else { interactive_install(&mut command) };
            let _ = sender.send(result);
        });
    }
}

fn interactive_install(command: &mut Command) -> Result<String, String> {
    use std::io::{Seek, SeekFrom};
    let path = std::env::temp_dir().join(format!("surfxr-install-{:032x}.log", rand::random::<u128>()));
    // Reserve before launching, and remove on every completed success/failure path.
    std::fs::OpenOptions::new().create_new(true).write(true).open(&path).map_err(|e| e.to_string())?;
    let result = (|| {
        let status = command.arg("-DashboardResult").arg(&path).status().map_err(|e| e.to_string())?;
        let mut file = std::fs::File::open(&path).map_err(|e| e.to_string())?;
        let length = file.metadata().map_err(|e| e.to_string())?.len();
        file.seek(SeekFrom::Start(length.saturating_sub(128 * 1024))).map_err(|e| e.to_string())?;
        let text = bounded_output(file);
        if status.success() { Ok(text) } else if text.trim().is_empty() {
            Err(format!("Installer exited: {status}\n{}", crate::i18n::tr("Installer returned no diagnostics. Check that frame-installer was updated together with Dashboard; inspect the installer window for startup errors.")))
        } else { Err(format!("Installer exited: {status}\n{text}")) }
    })();
    let _ = std::fs::remove_file(path);
    result
}

fn capture(command: &mut Command) -> Result<String, String> {
    let mut child = command.spawn().map_err(|e| e.to_string())?;
    let stdout = child.stdout.take().map(|out| std::thread::spawn(move || bounded_output(out)));
    let stderr = child.stderr.take().map(|out| std::thread::spawn(move || bounded_output(out)));
    let status = child.wait().map_err(|e| e.to_string())?;
    let text: String = [stdout, stderr].into_iter().flatten().map(|thread|
        thread.join().unwrap_or_else(|_| "Installer output reader failed".into())).collect();
    if status.success() { Ok(text) } else { Err(format!("Installer exited: {status}\n{text}")) }
}
fn bounded_output(mut reader: impl std::io::Read) -> String {
    const LIMIT: usize = 128 * 1024;
    let mut tail = Vec::new();
    let mut block = [0; 4096];
    loop {
        match reader.read(&mut block) {
            Ok(0) => break,
            Ok(count) => {
                tail.extend_from_slice(&block[..count]);
                if tail.len() > LIMIT { tail.drain(..tail.len() - LIMIT); }
            }
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(error) => return format!("{}\nInstaller output: {error}", String::from_utf8_lossy(&tail)),
        }
    }
    String::from_utf8_lossy(&tail).into_owned()
}


fn pairing_receipt(output: &str) -> Result<(String, std::net::IpAddr), String> {
    let records: Vec<_> = output.lines().filter_map(|line| line.strip_prefix("SURFXR_PAIR_ID ")).collect();
    if records.len() != 1 { return Err("Installation did not return a unique Frame pairing identity. Retry installation.".into()); }
    let value: serde_json::Value = serde_json::from_str(records[0]).map_err(|e| e.to_string())?;
    let name = value["hostname"].as_str().ok_or("Missing Frame hostname")?;
    if name.is_empty() || name.len() > 63 || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b)) {
        return Err("Invalid Frame hostname".into());
    }
    let ip: std::net::IpAddr = value["ip"].as_str().ok_or("Missing Frame IP")?.parse().map_err(|_| "Invalid Frame IP")?;
    if ip.is_loopback() || ip.is_unspecified() { return Err("Invalid Frame IP".into()); }
    Ok((name.into(), ip))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn production_plan_is_captured_without_ssh_or_pairing() {
        let root = std::env::temp_dir().join(format!("surfxr-plan-{:032x}", rand::random::<u128>()));
        std::fs::create_dir(&root).unwrap();
        std::fs::create_dir_all(root.join("payload/build")).unwrap();
        let mut files = serde_json::Map::new();
        for name in ["surfxr-viewer", "surfxr-dashboard", "surfxr-discover", "surfxr-client", "surfxr-runtime-bridge"] {
            let relative = format!("payload/build/{name}");
            std::fs::write(root.join(&relative), "").unwrap();
            files.insert(relative, serde_json::json!("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        }
        std::fs::write(root.join("release-manifest.json"), serde_json::to_vec(&serde_json::json!({
            "format_version":2, "architecture":"aarch64", "frame_python_required":false, "files":files
        })).unwrap()).unwrap();
        let script = Path::new(env!("CARGO_MANIFEST_DIR")).ancestors().nth(5).unwrap().join("scripts/Install-Frame.ps1");
        let mut command = installer_command(&script, &root, "fixture@nonexistent.invalid", true, true, false).unwrap();
        let output = capture(&mut command).unwrap();
        assert!(output.contains("Release install via fixture@nonexistent.invalid"));
        assert!(output.contains("streaming remains idle"));
        assert!(output.contains("--deploy install"));
        assert!(output.contains("umask 077;"));
        assert!(output.contains("name=$(hostname"));
        assert!(output.contains("\"link\":\"direct\""));
        assert!(!output.contains("SURFXR_PAIR_ID"));
        std::fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn installer_pairs_only_its_verified_receipt_and_waits_for_saved_trust() {
        let receipt = "Installation complete\nSURFXR_PAIR_ID {\"hostname\":\"frame-fixture\",\"ip\":\"192.0.2.1\"}\n";
        assert!(pairing_receipt("no receipt").is_err());
        assert!(pairing_receipt(&format!("{receipt}{receipt}")).is_err());
        assert!(pairing_receipt(&receipt.replace("192.0.2.1", "127.0.0.1")).is_err());
        assert!(pairing_receipt(&receipt.replace("frame-fixture", "frame;command")).is_err());
        let mut installer = FrameInstaller::new();
        let (send, recv) = mpsc::channel();
        installer.pending = Some(recv);
        send.send(Ok(receipt.into())).unwrap();
        let requests = installer.poll();
        assert_eq!(requests.len(), 3);
        let mut manager = surfxr_server_io::ServerSessionManager::new(None);
        installer.update_session(manager.session());
        assert!(installer.awaiting_trust.is_some());
        for request in requests {
            if let ServerRequest::UpdateClientList { hostname, action } = request {
                manager.update_client_connections(hostname, action);
            }
        }
        assert_eq!(manager.session().client_connections.len(), 1);
        installer.update_session(manager.session());
        assert!(installer.awaiting_trust.is_none());
        assert!(manager.session().client_connections["frame-fixture"].trusted);
        assert!(!installer.transcript.contains("SURFXR_PAIR_ID"));
    }
    #[test]
    fn plan_output_persists_and_failure_never_pairs() {
        let mut installer = FrameInstaller::new();
        installer.plan = true;
        let (send, recv) = mpsc::channel();
        installer.pending = Some(recv);
        send.send(Ok("A readable installation plan".into())).unwrap();
        assert!(installer.poll().is_empty());
        for _ in 0..5 { assert!(installer.poll().is_empty()); }
        assert_eq!(installer.result, "A readable installation plan");
        installer.plan = false;
        let (send, recv) = mpsc::channel();
        installer.pending = Some(recv);
        send.send(Err("SSH cancelled".into())).unwrap();
        assert!(installer.poll().is_empty());
        assert!(installer.awaiting_trust.is_none());
        assert_eq!(installer.result, "SSH cancelled");
    }
    #[test]
    fn rejects_ssh_options_and_shell_syntax() {
        for target in ["-oProxyCommand=x", "a@-host", "a@host;cmd", "a@b@c", "a@b c", "@host"] {
            assert!(!valid_target(target), "{target}");
        }
        assert!(valid_target("steamos@192.168.1.2"));
        assert!(valid_target("steamos@frame"));
    }
    #[test]
    fn plan_and_install_forward_paths_as_individual_arguments() {
        let root = std::env::temp_dir().join(format!("surfxr installer {}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let script = root.join("Install-Frame.ps1");
        std::fs::write(&script, "# fixture; never executed").unwrap();
        std::fs::write(root.join("release-manifest.json"), "{}").unwrap();
        for plan in [true, false] {
            let cmd = installer_command(&script, &root, "steamos@frame", false, plan, false).unwrap();
            let argv: Vec<_> = cmd.get_args().map(|a| a.to_string_lossy().into_owned()).collect();
            assert!(argv.windows(2).any(|v| v[0] == "-File" && v[1] == script.to_string_lossy()));
            assert!(argv.windows(2).any(|v| v == ["-Link", "lan"]));
            assert!(argv.windows(2).any(|v| v[0] == "-ReleaseDirectory" && v[1] == root.to_string_lossy()));
            assert_eq!(argv.iter().any(|v| v == "-Plan"), plan);
            assert!(argv.iter().any(|v| v == "-PairOnInstall"));
        }
        std::fs::remove_file(script).unwrap();
        std::fs::remove_file(root.join("release-manifest.json")).unwrap();
        std::fs::remove_dir(root).unwrap();
    }

    #[test]
    fn removal_keeps_settings_and_never_requests_or_waits_for_pairing() {
        let mut installer = FrameInstaller::new();
        assert!(!installer.direct, "Dashboard defaults to ordinary LAN");
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).ancestors().nth(5).unwrap();
        let mut command = installer_command(&root.join("scripts/Install-Frame.ps1"), root, "fixture@offline", false, true, true);
        assert!(command.is_err(), "Removal still requires a verified release package");
        let fixture = root.join("build/tmp/dashboard-removal");
        std::fs::create_dir_all(&fixture).unwrap();
        std::fs::write(fixture.join("release-manifest.json"), "{}").unwrap();
        command = installer_command(&root.join("scripts/Install-Frame.ps1"), &fixture, "fixture@offline", false, true, true);
        let command = command.unwrap();
        let argv: Vec<_> = command.get_args().map(|arg| arg.to_string_lossy()).collect();
        assert!(argv.windows(2).any(|args| args == ["-Action", "uninstall"]));
        assert!(!argv.iter().any(|arg| arg == "-PairOnInstall"));
        installer.uninstall = true;
        let (sender, receiver) = mpsc::channel();
        installer.pending = Some(receiver);
        sender.send(Ok("Client removed; settings kept".into())).unwrap();
        assert!(installer.poll().is_empty());
        assert!(!installer.busy());
        assert_eq!(installer.result, "Client removed; settings kept");
        std::fs::remove_file(fixture.join("release-manifest.json")).unwrap();
        std::fs::remove_dir(fixture).unwrap();
    }

    #[test]
    fn current_dashboard_adapter_is_independent_of_the_older_frame_archive() {
        let root = std::env::temp_dir().join(format!("surfxr-script-owner-{:032x}", rand::random::<u128>()));
        let exe = root.join("SurfXR Dashboard.exe");
        std::fs::create_dir_all(root.join("frame-client")).unwrap();
        std::fs::write(root.join("frame-client/Install-Frame.ps1"), "param([switch]$Plan)").unwrap();
        assert_ne!(script_near(&exe), Some(root.join("frame-client/Install-Frame.ps1")),
            "Never feed new arguments to an unversioned archive adapter");
        std::fs::create_dir_all(root.join("frame-installer")).unwrap();
        let adapter = root.join("frame-installer/Install-Frame.ps1");
        std::fs::write(&adapter, "# current PC adapter").unwrap();
        assert_eq!(script_near(&exe), Some(adapter));
        std::fs::remove_dir_all(root).unwrap();
    }

}
