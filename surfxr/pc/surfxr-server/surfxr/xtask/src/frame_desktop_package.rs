//! Bundle the existing native builder output; never pair or install a service.
pub fn copy(layout: &surfxr_filesystem::Layout) {
    let explicit = std::env::var_os("SURFXR_FRAME_DESKTOP_DIR");
    let path = explicit
        .clone()
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            surfxr_filesystem::build_dir().join("native/Release")
        });
    if !cfg!(windows) {
        return;
    }
    copy_installer(layout.dashboard_exe().parent().unwrap());
    if !path.join("surfxr-desktop.exe").is_file() {
        assert!(
            explicit.is_none(),
            "SURFXR_FRAME_DESKTOP_DIR must contain surfxr-desktop.exe and its DLLs"
        );
        eprintln!(
            "Optional Steam Frame desktop host was not built; Dashboard will show it as unavailable"
        );
        return;
    }
    let destination = layout.dashboard_exe().parent().unwrap().to_owned();
    for entry in std::fs::read_dir(path).unwrap() {
        let entry = entry.unwrap();
        let name = entry.file_name();
        let source = entry.path();
        if source.is_file()
            && (name == "surfxr-desktop.exe" || name == "surfxr-daemon.exe"
                || name == "FFmpeg.LICENSE.txt"
                || source.extension().is_some_and(|e| e == "dll"))
        {
            std::fs::copy(&source, destination.join(name)).unwrap();
        }
    }
    if destination.join("surfxr-daemon.exe").is_file(){
        for entry in std::fs::read_dir(layout.openvr_driver_lib_dir()).unwrap(){
            let entry=entry.unwrap();let source=entry.path();
            if source.is_file()&&source.extension().is_some_and(|suffix|suffix=="dll"){
                std::fs::copy(source,destination.join(entry.file_name())).unwrap();
            }
        }
    }
}

// PC-side adapter follows the Dashboard build, independently of the selected
// ARM64 archive. Never modify that archive's manifest or native payload.
fn copy_installer(destination: &std::path::Path) {
    let workspace = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).ancestors().nth(5).unwrap();
    for name in ["Install-Frame.ps1", "launch/Start-Frame.ps1",
        "launch/frame_release.ps1", "launch/frame_release_commands.ps1"] {
        let target = destination.join("frame-installer").join(name);
        std::fs::create_dir_all(target.parent().unwrap()).unwrap();
        std::fs::copy(workspace.join("scripts").join(name), target).unwrap();
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn dashboard_bundles_its_current_adapter_without_changing_the_frame_package() {
        let root = std::env::temp_dir().join(format!("surfxr-installer-package-{}", std::process::id()));
        std::fs::create_dir_all(root.join("frame-client")).unwrap();
        std::fs::write(root.join("frame-client/Install-Frame.ps1"), "old release adapter").unwrap();
        super::copy_installer(&root);
        assert_eq!(std::fs::read_to_string(root.join("frame-client/Install-Frame.ps1")).unwrap(), "old release adapter");
        let entry = std::fs::read_to_string(root.join("frame-installer/Install-Frame.ps1")).unwrap();
        assert!(entry.contains("PairOnInstall") && entry.contains("DashboardResult"));
        assert!(root.join("frame-installer/launch/frame_release_commands.ps1").is_file());
        std::fs::remove_dir_all(root).unwrap();
    }
}
