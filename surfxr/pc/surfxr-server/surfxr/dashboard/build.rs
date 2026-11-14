#[cfg(windows)]
fn main() {
    use std::{env, fs, path::PathBuf};
    let header = PathBuf::from("../../openvr/headers/openvr_capi.h");
    println!("cargo:rerun-if-changed={}", header.display());
    let source = fs::read_to_string(&header).expect("read bundled OpenVR SDK");
    let declaration = source
        .lines()
        .find(|line| line.contains("* IVROverlay_Version ="))
        .expect("OpenVR SDK must declare its overlay ABI");
    let version = declaration
        .split('"')
        .nth(1)
        .expect("SDK overlay interface identifier");
    assert!(
        version
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'_')
    );
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let bindings = bindgen::Builder::default()
        .header(header.to_string_lossy())
        .allowlist_type("VR_IVROverlay_FnTable|EVRApplicationType")
        .allowlist_var(".*VisibleInDashboard|.*VRApplication_Background")
        .prepend_enum_name(false)
        .layout_tests(false)
        .generate_comments(false)
        .generate()
        .expect("generate OpenVR overlay SDK bindings");
    bindings
        .write_to_file(output.join("overlay_sdk.rs"))
        .unwrap();
    fs::write(
        output.join("overlay_interface.rs"),
        format!("pub const INTERFACE: &[u8] = b\"FnTable:{version}\\0\";\n"),
    )
    .unwrap();
    let mut resource = winres::WindowsResource::new();
    resource.set("ProductName", "SurfXR");
    resource.set("FileDescription", "SurfXR Dashboard");
    resource.set_icon("resources/dashboard.ico");
    resource.compile().unwrap();
}

#[cfg(not(windows))]
fn main() {}
