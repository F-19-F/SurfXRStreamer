use surfxr_filesystem::{self as afs, Layout};
use std::{
    fmt::{self, Display, Formatter},
    fs,
    vec,
};
use xshell::{Shell, cmd};
#[path = "frame_desktop_package.rs"]
mod frame_desktop_package;

#[derive(Clone, Copy)]
pub enum Profile {
    Debug,
    Release,
    Distribution,
}

impl Display for Profile {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        let string = match self {
            Profile::Distribution => "distribution",
            Profile::Release => "release",
            Profile::Debug => "debug",
        };
        write!(f, "{string}")
    }
}

pub fn build_streamer(
    profile: Profile,
    gpl: bool,
    root: Option<String>,
    reproducible: bool,
    profiling: bool,
    keep_config: bool,
) {
    let sh = Shell::new().unwrap();

    let build_layout = Layout::new(&afs::streamer_build_dir());

    let mut common_flags = vec![];
    match profile {
        Profile::Distribution => {
            common_flags.push("--profile");
            common_flags.push("distribution");
        }
        Profile::Release => common_flags.push("--release"),
        Profile::Debug => (),
    }
    if reproducible {
        common_flags.push("--locked");
    }

    let artifacts_dir = if cfg!(all(windows, target_arch = "aarch64")) {
        // Fix for cross compilation
        const TARGET: &str = "x86_64-pc-windows-msvc";

        common_flags.push("--target");
        common_flags.push(TARGET);

        afs::target_dir().join(TARGET).join(profile.to_string())
    } else {
        afs::target_dir().join(profile.to_string())
    };

    let common_flags_ref = &common_flags;

    let maybe_config = if keep_config {
        fs::read_to_string(build_layout.session()).ok()
    } else {
        None
    };

    sh.remove_path(afs::streamer_build_dir()).ok();
    sh.create_dir(build_layout.openvr_driver_lib_dir()).unwrap();
    sh.create_dir(&build_layout.executables_dir).unwrap();

    if let Some(config) = maybe_config {
        fs::write(build_layout.session(), config).ok();
    }

    if let Some(root) = root {
        sh.set_var("SURFXR_ROOT_DIR", root);
    }

    // build server
    {
        let gpl_flag = if gpl {
            vec!["--features", "gpl"]
        } else {
            vec![]
        };

        let profiling_flag = if profiling {
            vec!["--features", "surfxr_server_core/trace-performance"]
        } else {
            vec![]
        };

        let _push_guard = sh.push_dir(afs::crate_dir("server_openvr"));
        cmd!(
            sh,
            "cargo build {common_flags_ref...} {gpl_flag...} {profiling_flag...}"
        )
        .run()
        .unwrap();

        sh.copy_file(
            artifacts_dir.join(afs::dynlib_fname("surfxr_server_openvr")),
            build_layout.openvr_driver_lib(),
        )
        .unwrap();

        if cfg!(windows) {
            sh.copy_file(artifacts_dir.join("surfxr_frame_worker.exe"),
                build_layout.openvr_driver_lib_dir().join("surfxr_frame_worker.exe")).unwrap();
            sh.copy_file(
                artifacts_dir.join("surfxr_server_openvr.pdb"),
                build_layout
                    .openvr_driver_lib_dir()
                    .join("surfxr_server_openvr.pdb"),
            )
            .unwrap();
        }
    }

    // Build dashboard
    {
        let _push_guard = sh.push_dir(afs::crate_dir("dashboard"));
        cmd!(sh, "cargo build {common_flags_ref...}").run().unwrap();

        sh.copy_file(
            artifacts_dir.join(afs::exec_fname("surfxr_dashboard")),
            build_layout.dashboard_exe(),
        )
        .unwrap();
    }

    // copy dependencies
    if cfg!(windows) {
        sh.copy_file(
            afs::workspace_dir().join("openvr/bin/win64/openvr_api.dll"),
            build_layout.openvr_driver_lib_dir(),
        )
        .unwrap();

        // copy ffmpeg binaries
        if gpl {
            let bin_dir = &build_layout.openvr_driver_lib_dir();
            sh.create_dir(bin_dir).unwrap();
            for lib_path in sh
                .read_dir(afs::deps_dir().join("windows/ffmpeg/bin"))
                .unwrap()
                .into_iter()
                .filter(|path| path.file_name().unwrap().to_string_lossy().contains(".dll"))
            {
                sh.copy_file(lib_path.clone(), bin_dir).unwrap();
            }
        }

        // copy libvpl.dll
        sh.copy_file(
            afs::deps_dir().join("windows/libvpl/surfxr_build/bin/libvpl.dll"),
            build_layout.openvr_driver_lib_dir(),
        )
        .unwrap();
    } else if cfg!(target_os = "linux") {
        let firewall_script = afs::crate_dir("xtask").join("firewall/surfxr_fw_config.sh");
        let firewalld = afs::crate_dir("xtask").join("firewall/surfxr-firewalld.xml");
        let ufw = afs::crate_dir("xtask").join("firewall/ufw-surfxr");

        // copy linux specific firewalls
        sh.create_dir(build_layout.firewall_script()).unwrap();
        sh.copy_file(firewall_script, build_layout.firewall_script())
            .unwrap();
        sh.copy_file(firewalld, build_layout.firewalld_config())
            .unwrap();
        sh.copy_file(ufw, build_layout.ufw_config()).unwrap();
    }

    frame_desktop_package::copy(&build_layout);
    // copy static resources
    {
        // copy driver manifest
        sh.copy_file(
            afs::crate_dir("xtask").join("resources/driver.vrdrivermanifest"),
            build_layout.openvr_driver_manifest(),
        )
        .unwrap();
    }
}
