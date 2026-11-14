fn main() {
    println!("cargo:rerun-if-env-changed=SURFXR_RELEASE_VERSION");
    let version = std::env::var("SURFXR_RELEASE_VERSION")
        .ok().filter(|value| !value.is_empty())
        .unwrap_or_else(|| std::env::var("CARGO_PKG_VERSION").unwrap());
    semver::Version::parse(&version).expect("SURFXR_RELEASE_VERSION must be SemVer");
    println!("cargo:rustc-env=SURFXR_PRODUCT_VERSION={version}");
}
