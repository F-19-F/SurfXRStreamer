//! Companion Frame discovery/start datagrams. Discovery never launches SteamVR.
pub fn frame_discovery_reply(query: &[u8], port: u16) -> Option<Vec<u8>> {
    if query.len() != 16 || &query[..8] != b"ALVRFD01" || surfxr_common::protocol_id() != "21-dev14" {
        return None;
    }
    let name = std::env::var("COMPUTERNAME")
        .or_else(|_| std::env::var("HOSTNAME"))
        .unwrap_or_else(|_| "SURFXR PC".into());
    let name: Vec<_> = name
        .bytes()
        .filter(|b| (32..127).contains(b))
        .take(63)
        .collect();
    let mut reply = b"ALVRPC01".to_vec();
    reply.extend_from_slice(&query[8..]);
    reply.extend_from_slice(b"21-dev14");
    reply.extend_from_slice(&port.to_le_bytes());
    reply.push(name.len() as u8);
    reply.extend(name);
    Some(reply)
}

pub fn frame_start_hostname(query: &[u8]) -> Option<&str> {
    hostname(query,b"ALVRFS01")
}
fn hostname<'a>(query:&'a[u8],magic:&[u8;8])->Option<&'a str> {
    if query.len() < 18
        || query.len() > 80
        || &query[..8] != magic
        || query.len() != 17 + query[16] as usize
    {
        return None;
    }
    let hostname = std::str::from_utf8(&query[17..]).ok()?;
    if !hostname
        .bytes()
        .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
    {
        return None;
    }
    Some(hostname)
}

pub fn frame_start_status_hostname(query:&[u8])->Option<&str>{hostname(query,b"ALVRFQ01")}
pub fn frame_start_status_reply(query:&[u8],phase:u8,reason:u8,attempt:u64)->Option<Vec<u8>>{
    frame_start_status_hostname(query)?;if phase>3||reason>4{return None;}
    let mut reply=b"ALVRPQ01".to_vec();reply.extend_from_slice(&query[8..16]);reply.extend([1,phase,reason]);
    reply.extend(attempt.to_le_bytes());Some(reply)
}

pub fn frame_start_reply(query: &[u8], accepted: bool) -> Option<Vec<u8>> {
    frame_start_hostname(query)?;
    let mut reply = b"ALVRPS01".to_vec();
    reply.extend_from_slice(&query[8..16]);
    reply.push(u8::from(accepted));
    Some(reply)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bounded_discovery_and_start_are_distinct_and_nonce_associated() {
        let mut query = b"ALVRFD01".to_vec();
        query.extend_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
        let reply = frame_discovery_reply(&query, 12345).unwrap();
        assert_eq!(
            &reply[..24],
            b"ALVRPC01\x01\x02\x03\x04\x05\x06\x07\x0821-dev14"
        );
        assert_eq!(u16::from_le_bytes([reply[24], reply[25]]), 12345);
        assert!(reply.len() <= 90);
        assert!(frame_start_hostname(&query).is_none());
        query[..8].copy_from_slice(b"ALVRFS01");
        query.push(5);
        query.extend_from_slice(b"frame");
        assert_eq!(frame_start_hostname(&query), Some("frame"));
        assert_eq!(
            frame_start_reply(&query, true).unwrap(),
            b"ALVRPS01\x01\x02\x03\x04\x05\x06\x07\x08\x01"
        );
        assert!(frame_discovery_reply(&query, 12345).is_none());
        query[..8].copy_from_slice(b"ALVRFQ01");assert!(frame_start_hostname(&query).is_none());
        assert_eq!(frame_start_status_hostname(&query),Some("frame"));
        let status=frame_start_status_reply(&query,1,0,42).unwrap();assert_eq!(status.len(),27);
        assert_eq!(&status[..16],b"ALVRPQ01\x01\x02\x03\x04\x05\x06\x07\x08");
        assert_eq!(&status[16..19],&[1,1,0]);assert_eq!(u64::from_le_bytes(status[19..].try_into().unwrap()),42);
        assert!(frame_start_status_reply(&query,4,0,0).is_none());query[16]=64;
        assert!(frame_start_status_hostname(&query).is_none());
    }
}
