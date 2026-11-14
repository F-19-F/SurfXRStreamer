use semver::Version;
use std::{
    collections::hash_map::DefaultHasher,
    hash::{Hash, Hasher},
    sync::LazyLock,
};

pub static SURFXR_VERSION: LazyLock<Version> =
    LazyLock::new(|| Version::parse(env!("SURFXR_PRODUCT_VERSION")).unwrap());

/// Upstream wire revision; product versions never select packet encodings.
pub const WIRE_VERSION: &str = "21.0.0-dev14";

// Consistent across architectures, might not be consistent across different compiler versions.
pub fn hash_string(string: &str) -> u64 {
    let mut hasher = DefaultHasher::new();
    string.hash(&mut hasher);
    hasher.finish()
}

pub fn is_nightly() -> bool {
    SURFXR_VERSION.build.contains("nightly")
}

pub fn is_stable() -> bool {
    SURFXR_VERSION.pre.is_empty() && !is_nightly()
}

// Semver compatible versions will produce the same protocol ID. Protocol IDs are not ordered
// As a convention, encode/decode the protocol ID bytes as little endian.
// Only makor and
pub fn protocol_id() -> String {
    "21-dev14".to_owned()
}

pub fn protocol_id_u64() -> u64 {
    hash_string(&protocol_id())
}

// deprecated
pub fn is_version_compatible(other_version: &Version) -> bool {
    let protocol_string = if other_version.pre.is_empty() {
        other_version.major.to_string()
    } else {
        format!("{}-{}", other_version.major, other_version.pre)
    };

    protocol_id_u64() == hash_string(&protocol_string)
}

#[cfg(test)]
mod tests {
    #[test]
    fn independent_product_release_keeps_deployed_wire_identity() {
        assert_eq!(super::SURFXR_VERSION.to_string(), env!("SURFXR_PRODUCT_VERSION"));
        assert_eq!(super::protocol_id(), "21-dev14");
        assert_eq!(super::WIRE_VERSION, "21.0.0-dev14");
    }
}
