//! One parity shard per sixteen data shards, computed before prefix overlap.
pub(super) const GROUP: usize = 16;
pub(super) const LENGTH_BYTES: usize = 4;

pub(super) struct VideoParity {
    bytes: Vec<u8>,
}
impl VideoParity {
    pub fn new(packet_size: usize) -> Self { Self { bytes: vec![0; packet_size] } }
    pub fn accumulate(&mut self, data: &[u8], length: usize, packet: u32, count: usize, part: usize) -> Option<&[u8]> {
        if part % GROUP == 0 { self.bytes.fill(0); }
        for (parity, value) in self.bytes[18..].iter_mut().zip(data) { *parity ^= value; }
        if part % GROUP != GROUP - 1 && part + 1 != count { return None; }
        self.bytes[..2].copy_from_slice(&3u16.to_le_bytes());
        self.bytes[2..6].copy_from_slice(&packet.to_le_bytes());
        self.bytes[6..10].copy_from_slice(&(count as u32).to_le_bytes());
        self.bytes[10..14].copy_from_slice(&((count + part / GROUP) as u32).to_le_bytes());
        self.bytes[14..18].copy_from_slice(&(length as u32).to_le_bytes());
        Some(&self.bytes)
    }
}
