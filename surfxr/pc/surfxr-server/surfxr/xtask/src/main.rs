mod build;
mod ci;
mod command;
mod dependencies;
mod format;
mod packaging;
mod version;

use build::Profile;
use surfxr_filesystem as afs;
use pico_args::Arguments;
use std::{fs, process, time::Instant};
use xshell::{Shell, cmd};

const HELP_STR: &str = r#"
SurfXR PC development (Steam Frame client uses Build-PC.ps1 / Start-Frame.ps1)
cargo xtask <prepare-deps|build-streamer|run-streamer|
             package-streamer|format|check-format|clean|bump|clippy|check-licenses>
  --platform windows|linux|macos  Select PC dependency platform
  --release --profiling --gpl --keep-config
  --portable --setup --no-rebuild  Package the existing complete PC/Frame payload
  --root <path> --version <semver> --nightly --ci --no-nvidia
"#;

enum BuildPlatform { Windows, Linux, Macos }

pub fn print_help_and_exit(message: &str) -> ! {
    eprintln!("{message}\n{HELP_STR}");
    process::exit(1);
}

fn clean() {
    fs::remove_dir_all(afs::build_dir()).ok();
    fs::remove_dir_all(afs::deps_dir()).ok();
    if afs::target_dir() == afs::repository_dir().join("build/cargo") {
        fs::remove_dir_all(afs::target_dir()).ok();
    }
}

fn main() {
    let begin = Instant::now();
    let mut args = Arguments::from_env();
    if args.contains(["-h", "--help"]) { println!("{HELP_STR}"); return; }
    let action = args.subcommand().unwrap().unwrap_or_else(|| print_help_and_exit("Missing action"));
    let profile = if args.contains("--release") { Profile::Release } else { Profile::Debug };
    let no_nvidia = args.contains("--no-nvidia");
    let profiling = args.contains("--profiling");
    let gpl = args.contains("--gpl");
    let nightly = args.contains("--nightly");
    let no_rebuild = args.contains("--no-rebuild");
    let portable = args.contains("--portable");
    let setup = args.contains("--setup");
    let ci = args.contains("--ci");
    let keep = args.contains("--keep-config");
    let platform: Option<String> = args.opt_value_from_str("--platform").unwrap();
    let platform = platform.as_deref().map(|s| match s {
        "windows" => BuildPlatform::Windows,
        "linux" => BuildPlatform::Linux,
        "macos" => BuildPlatform::Macos,
        _ => print_help_and_exit("Unsupported PC platform"),
    });
    let version: Option<String> = args.opt_value_from_str("--version").unwrap();
    let root: Option<String> = args.opt_value_from_str("--root").unwrap();
    if !args.finish().is_empty() { print_help_and_exit("Unknown arguments"); }
    match action.as_str() {
        "prepare-deps" => dependencies::prepare_server_deps(platform, ci, !no_nvidia),
        "build-streamer" => build::build_streamer(profile, gpl, root, false, profiling, keep),
        "run-streamer" => {
            if !no_rebuild { build::build_streamer(profile, gpl, root, false, profiling, keep); }
            let sh = Shell::new().unwrap();
            let exe = afs::Layout::new(&afs::streamer_build_dir()).dashboard_exe();
            cmd!(sh, "{exe}").run().unwrap();
        }
        "package-streamer" => packaging::package_streamer(platform, ci, !no_nvidia, gpl, root, no_rebuild, portable, setup),
        "format" => format::format(),
        "check-format" => format::check_format(),
        "clean" => clean(),
        "bump" => version::bump_version(version, nightly),
        "clippy" => ci::clippy_ci(),
        "check-licenses" => packaging::check_licenses(),
        _ => print_help_and_exit("Unknown action"),
    }
    println!("Done in {}s", begin.elapsed().as_secs());
}
