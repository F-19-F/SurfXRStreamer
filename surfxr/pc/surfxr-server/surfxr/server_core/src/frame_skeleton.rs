//! Fixed, bounded parent-relative OpenVR bones inside negotiated ReservedBuffer.
use surfxr_common::{
    Pose,
    glam::{Quat, Vec3},
};

pub struct FrameSkeleton {
    pub hand: u32,
    pub sampled_ns: u64,
    pub bones: Option<Box<[Pose; 31]>>,
}

pub fn decode(bytes: &[u8]) -> Option<FrameSkeleton> {
    if bytes.len() < 24 {
        return None;
    }
    let u32_at = |i| u32::from_le_bytes(bytes[i..i + 4].try_into().unwrap());
    if u32_at(0) != 0x5644534b || u32_at(4) != 1 {
        return None;
    }
    let hand = u32_at(8);
    let active = u32_at(12);
    let sampled_ns = u64::from_le_bytes(bytes[16..24].try_into().unwrap());
    if hand > 1
        || active > 1
        || bytes.len() != 24 + active as usize * 31 * 28
        || (active == 0) != (sampled_ns == 0)
    {
        return None;
    }
    let bones = if active == 1 {
        let mut bones = Box::new([Pose::IDENTITY; 31]);
        for (i, bone) in bones.iter_mut().enumerate() {
            let values: [f32; 7] = std::array::from_fn(|j| {
                let offset = 24 + i * 28 + j * 4;
                f32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
            });
            if !values.iter().all(|v| v.is_finite()) || values[..3].iter().any(|v| v.abs() > 2.0) {
                return None;
            }
            let orientation = Quat::from_array(values[3..].try_into().unwrap());
            if !(0.5..=1.5).contains(&orientation.length_squared()) {
                return None;
            }
            *bone = Pose {
                position: Vec3::from_array(values[..3].try_into().unwrap()),
                orientation,
            };
        }
        Some(bones)
    } else {
        None
    };
    Some(FrameSkeleton {
        hand,
        sampled_ns,
        bones,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn strict_native_bones_and_release() {
        let mut bytes = Vec::new();
        for v in [0x5644534bu32, 1, 1, 1] {
            bytes.extend(v.to_le_bytes());
        }
        bytes.extend(1000u64.to_le_bytes());
        for _ in 0..31 {
            for v in [0.0f32, 0.01, 0.0, 0.0, 0.0, 0.0, 1.0] {
                bytes.extend(v.to_le_bytes());
            }
        }
        let value = decode(&bytes).unwrap();
        assert_eq!(value.hand, 1);
        assert_eq!(value.bones.unwrap()[30].position.y, 0.01);
        assert!(decode(&bytes[..bytes.len() - 1]).is_none());
        bytes[24..28].copy_from_slice(&f32::NAN.to_le_bytes());
        assert!(decode(&bytes).is_none());
        bytes.truncate(24);
        bytes[12..16].copy_from_slice(&0u32.to_le_bytes());
        bytes[16..24].fill(0);
        assert!(decode(&bytes).unwrap().bones.is_none());
        bytes.push(0);
        assert!(decode(&bytes).is_none());
    }
}
