use crate::{
    BuildPlatform,
    build::{self, Profile},
    command,
    dependencies,
};
use surfxr_filesystem as afs;
use std::path::Path;
use xshell::{Shell, cmd};

pub fn generate_licenses(licenses_output: &Path) {
    let sh = Shell::new().unwrap();
    cmd!(sh, "cargo install cargo-about --version 0.8.4 --locked")
        .run()
        .unwrap();
    let licenses_template = afs::crate_dir("xtask").join("licenses_template.hbs");
    cmd!(
        sh,
        "cargo about generate {licenses_template} -o {licenses_output}"
    )
    .run()
    .unwrap();
}

pub fn check_licenses() {
    generate_licenses(&std::env::temp_dir().join("dependencies.html"));
}

pub fn include_base_licenses(root_path: &Path) {
    let sh = Shell::new().unwrap();

    let licenses_dir = root_path.join("licenses");
    sh.create_dir(&licenses_dir).unwrap();
    sh.copy_file(
        afs::repository_dir().join("LICENSE"),
        licenses_dir.join("SURFXR.txt"),
    )
    .unwrap();

    sh.copy_file(afs::workspace_dir().join("LICENSE"), licenses_dir.join("ALVR.txt")).unwrap();
    sh.copy_file(afs::repository_dir().join("NOTICE.md"), licenses_dir.join("NOTICE.md")).unwrap();
    generate_licenses(&licenses_dir.join("dependencies.html"));
}

pub fn include_licenses(root_path: &Path, gpl: bool) {
    let sh = Shell::new().unwrap();

    // Add licenses
    include_base_licenses(root_path);

    let licenses_dir = root_path.join("licenses");
    sh.copy_file(
        afs::repository_dir().join("LICENSE"),
        licenses_dir.join("SURFXR.txt"),
    )
    .unwrap();
    sh.copy_file(
        afs::crate_dir("server_openvr").join("LICENSE-Valve"),
        licenses_dir.join("Valve.txt"),
    )
    .unwrap();
    if gpl {
        sh.copy_file(
            afs::deps_dir().join("windows/ffmpeg/LICENSE.txt"),
            licenses_dir.join("FFmpeg.txt"),
        )
        .ok();
    }
    if cfg!(windows) {
        sh.copy_file(
            afs::deps_dir().join("windows/libvpl/surfxr_build/share/vpl/licensing/license.txt"),
            licenses_dir.join("libvpl.txt"),
        )
        .unwrap();
    }
}

pub fn package_streamer(
    platform: Option<BuildPlatform>,
    skip_admin_priv: bool,
    enable_nvenc: bool,
    gpl: bool,
    root: Option<String>,
    no_rebuild: bool,
    portable: bool,
    setup: bool,
) {
    let sh = Shell::new().unwrap();

    assert!(!(portable || setup) || cfg!(windows), "Portable/Setup packaging requires Windows");
    if !no_rebuild {
        dependencies::prepare_server_deps(platform, skip_admin_priv, enable_nvenc);
        build::build_streamer(Profile::Distribution, gpl, root, true, false, false);
    }

    include_licenses(&afs::streamer_build_dir(), gpl);

    if portable || setup {
        let script = afs::repository_dir().join("scripts/package/portable_windows.ps1");
        let source = afs::streamer_build_dir();
        let flags = if setup { vec!["-Setup"] } else { vec![] };
        cmd!(sh, "powershell -NoProfile -ExecutionPolicy Bypass -File {script} -Source {source} {flags...}")
            .run().unwrap();
    } else if cfg!(windows) {
        command::zip(&sh, &afs::streamer_build_dir()).unwrap();
    } else {
        command::targz(&sh, &afs::streamer_build_dir()).unwrap();
    }
}
