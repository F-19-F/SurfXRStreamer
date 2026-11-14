use crate::openvrpaths;
use surfxr_packets::FirewallRulesAction;
use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

fn netsh_add_rule_command_string(rule_name: &str, program_path: &Path) -> String {
    format!(
        "netsh advfirewall firewall add rule name=\"{}\" dir=in program=\"{}\" action=allow",
        rule_name,
        program_path.to_string_lossy()
    )
}

fn netsh_delete_rule_command_string(rule_name: &str) -> String {
    format!("netsh advfirewall firewall delete rule name=\"{rule_name}\"")
}

// Errors:
// 1: firewall rule is already set
// 126: pkexec request dismissed
// other: command failed
pub fn firewall_rules(
    action: FirewallRulesAction,
    filesystem_layout: &surfxr_filesystem::Layout,
) -> Result<(), i32> {
    let exit_status = if cfg!(target_os = "linux") {
        let action = if matches!(action, FirewallRulesAction::Add) {
            "add"
        } else {
            "remove"
        };
        // run as normal user since we use pkexec to sudo
        Command::new("bash")
            .arg(
                PathBuf::from("../").join(
                    filesystem_layout
                        .firewall_script_dir
                        .join("surfxr_fw_config.sh"),
                ),
            )
            .arg(action)
            .status()
            .map_err(|_| -1)?
    } else {
        let script_path = env::temp_dir().join("surfxr_firewall_rules.bat");
        let mut firewall_rules_script_content = if matches!(action, FirewallRulesAction::Add) {
            format!(
                "{}\n{}\n{}",
                netsh_add_rule_command_string(
                    "SteamVR SURFXR vrserver",
                    &openvrpaths::steamvr_root_dir()
                        .map_err(|_| -1)?
                        .join("bin")
                        .join("win64")
                        .join("vrserver.exe")
                ),
                netsh_add_rule_command_string("SurfXR Dashboard", &filesystem_layout.dashboard_exe()),
                netsh_add_rule_command_string(
                    "SteamVR SURFXR vrserver",
                    &openvrpaths::steamvr_root_dir()
                        .map_err(|_| -1)?
                        .join("bin")
                        .join("win32")
                        .join("vrserver.exe")
                ),
            )
        } else {
            format!("{}\n{}",netsh_delete_rule_command_string("SteamVR SURFXR vrserver"),
                netsh_delete_rule_command_string("SurfXR Dashboard"))
        };
        // Reuse the existing explicit/elevated rule action for both host paths.
        // The service uses its installed Program Files executable, not sources.
        let desktop=filesystem_layout.dashboard_exe().with_file_name("surfxr-desktop.exe");
        let daemon=filesystem_layout.dashboard_exe().with_file_name("surfxr-daemon.exe");
        if matches!(action,FirewallRulesAction::Add) {
            if desktop.is_file(){firewall_rules_script_content.push_str(&format!("\n{}",netsh_add_rule_command_string("SURFXR Frame Desktop",&desktop)));}
            if daemon.is_file(){firewall_rules_script_content.push_str(&format!("\n{}",netsh_add_rule_command_string("SURFXR Frame Daemon",&daemon)));}
            if let Some(root)=env::var_os("ProgramFiles") {
                let service=PathBuf::from(&root).join("SurfXRDesktop").join("surfxr-desktop.exe");
                firewall_rules_script_content.push_str(&format!("\n{}",netsh_add_rule_command_string("SURFXR Frame Desktop SYSTEM",&service)));
                let daemon=PathBuf::from(root).join("SurfXRDesktop").join("surfxr-daemon.exe");
                firewall_rules_script_content.push_str(&format!("\n{}",netsh_add_rule_command_string("SURFXR Frame Daemon SYSTEM",&daemon)));
            }
        }else{
            for name in ["SURFXR Frame Desktop","SURFXR Frame Desktop SYSTEM","SURFXR Frame Daemon","SURFXR Frame Daemon SYSTEM"] {
                firewall_rules_script_content.push_str(&format!("\n{}",netsh_delete_rule_command_string(name)));
            }
        }
        fs::write(&script_path, firewall_rules_script_content).map_err(|_| -1)?;

        // run with admin privileges
        runas::Command::new(script_path)
            .gui(true) // UAC, if available
            .status()
            .map_err(|_| -1)?
    };

    if exit_status.success() {
        Ok(())
    } else {
        Err(exit_status.code().unwrap())
    }
}
