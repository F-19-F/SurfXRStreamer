use crate::ServerSessionManager;
use surfxr_common::{anyhow::{Result, bail}, settings_schema::Switch};
use surfxr_packets::{PathSegment, PathValuePair};
use surfxr_session::{BitrateMode, FrameSize, Settings, frame_settings::*};
use serde_json::{Value, json};

pub fn video_identity(settings: &Settings) -> Value {
    json!([settings.video.preferred_fps, settings.video.transcoding_view_resolution,
        settings.video.emulated_headset_view_resolution])
}
impl ServerSessionManager {
    pub fn frame_snapshot(&self, sequence: u64) -> FrameSettingsSnapshot {
        let video=&self.settings().video;
        let quality=TIERS.iter().position(|&(w,h)| {
            [&video.transcoding_view_resolution,&video.emulated_headset_view_resolution]
                .iter().all(|size| matches!(size,FrameSize::Absolute{width,height:Some(height)} if *width==w && *height==h))
        }).map_or(-2., |index|index as f64-1.);
        let bitrate=match &video.bitrate.mode {
            BitrateMode::ConstantMbps(value)=>*value as f64,
            BitrateMode::Adaptive{max_throughput_mbps,..}=>match max_throughput_mbps {
                Switch::Enabled(value)=>*value as f64, Switch::Disabled=>0.,
            },
        };
        FrameSettingsSnapshot {sequence,revision:self.session().frame_settings_sync.revision,
            fps:video.preferred_fps as f64,quality,bitrate,prefer_pc_mouse:self.session().frame_settings_sync.prefer_pc_mouse}
    }
    // Returns true only for a newly accepted exit, never for a replay.
    pub fn frame_request(&mut self, request:&FrameSettingsRequest) -> Result<(FrameSettingsSnapshot,bool)> {
        let mut normalized=request.clone();normalized.action &= 0x7f;
        let r=&normalized;
        if r.action==0 { return Ok((self.frame_snapshot(0),false)); }
        if r.token==0 || r.sequence==0 || !r.value.is_finite() { bail!("Invalid SurfXR setting request"); }
        if self.session().frame_settings_sync.receipts.iter().any(|&(token,seq)|token==r.token && seq>=r.sequence) {
            return Ok((self.frame_snapshot(r.sequence),false));
        }
        let mut values=Vec::new();
        let mut set=|path:&str,value:Value|values.push(PathValuePair {
            path:path.split('.').map(|name|PathSegment::Name(name.into())).collect(),value});
        match r.action {
            1 if RATES.iter().any(|&fps|fps as f64==r.value)=>set("session_settings.video.preferred_fps",json!(r.value)),
            2 if r.value.fract()==0. && (-1. ..=7.).contains(&r.value)=>{
                let (w,h)=TIERS[(r.value as i32+1) as usize];
                for field in ["transcoding_view_resolution","emulated_headset_view_resolution"] {
                    let base=format!("session_settings.video.{field}");
                    set(&format!("{base}.variant"),json!("Absolute"));
                    set(&format!("{base}.Absolute.width"),json!(w));
                    set(&format!("{base}.Absolute.height.set"),json!(true));
                    set(&format!("{base}.Absolute.height.content"),json!(h));
                }
            }
            3 if r.value.fract()==0. && (5. ..=1000.).contains(&r.value)=>{
                if matches!(self.settings().video.bitrate.mode,BitrateMode::ConstantMbps(_)) {
                    set("session_settings.video.bitrate.mode.ConstantMbps",json!(r.value as u64));
                } else {
                    set("session_settings.video.bitrate.mode.Adaptive.max_throughput_mbps.enabled",json!(true));
                    set("session_settings.video.bitrate.mode.Adaptive.max_throughput_mbps.content",json!(r.value as u64));
                    // A lower cap cannot leave a conflicting adaptive minimum.
                    if let BitrateMode::Adaptive{min_throughput_mbps:Switch::Enabled(min),..}=&self.settings().video.bitrate.mode {
                        if *min>r.value as u64 {set("session_settings.video.bitrate.mode.Adaptive.min_throughput_mbps.content",json!(r.value as u64));}
                    }
                }
            }
            4 if r.value==0. =>{},
            5 if r.value==0. || r.value==1. =>set("frame_settings_sync.prefer_pc_mouse",json!(r.value==1.)),
            _=>bail!("Invalid SurfXR setting value"),
        }
        let mut receipts=self.session().frame_settings_sync.receipts.clone();
        receipts.retain(|&(token,_)|token!=r.token);
        if receipts.len()>=64 {receipts.remove(0);}
        receipts.push((r.token,r.sequence));
        set("frame_settings_sync.receipts",json!(receipts));
        self.set_session_values(values)?;
        Ok((self.frame_snapshot(r.sequence),r.action==4))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn request(sequence:u64,action:u8,value:f64)->FrameSettingsRequest {FrameSettingsRequest{token:123,sequence,action,value}}
    #[test]
    fn active_edits_merge_and_replays_cannot_overwrite_later_pc_edits() {
        let mut manager=ServerSessionManager::new(None);
        manager.frame_request(&request(1,1,90.)).unwrap();
        manager.frame_request(&request(2,2,1.)).unwrap();
        manager.set_session_values(vec![PathValuePair{path:["session_settings","video","preferred_fps"].into_iter().map(|p|PathSegment::Name(p.into())).collect(),value:json!(120)}]).unwrap();
        let (snapshot,_)=manager.frame_request(&request(1,1,90.)).unwrap();
        assert_eq!((snapshot.fps,snapshot.quality),(120.,1.));
        let revision=snapshot.revision;
        assert_eq!(manager.frame_request(&request(0,0,0.)).unwrap().0.revision,revision);
        assert!(manager.frame_request(&request(3,1,123.)).is_err());
        assert_eq!(manager.frame_snapshot(0).revision,revision);
        assert!(manager.frame_request(&request(3,4,0.)).unwrap().1);
        assert!(!manager.frame_request(&request(3,4,0.)).unwrap().1);
    }
    #[test]
    fn receipts_survive_owner_transfer_and_bitrate_preserves_other_fields() {
        let mut a=ServerSessionManager::new(None);
        a.frame_request(&request(1,3,300.)).unwrap();
        let mut b=ServerSessionManager::new(None);
        *b.session_mut()=serde_json::from_value(serde_json::to_value(a.session()).unwrap()).unwrap();
        b.frame_request(&request(2,3,400.)).unwrap();
        assert_eq!(b.frame_request(&request(1,3,300.)).unwrap().0.bitrate,400.);
        assert_eq!(b.frame_snapshot(0).fps,120.);
    }
    #[test]
    fn pointer_policy_uses_accepted_revisions_and_keeps_legacy_wire() {
        let mut a=ServerSessionManager::new(None);
        let initial=a.frame_snapshot(0);
        assert!(!initial.prefer_pc_mouse);
        assert_eq!(initial.bytes(false).len(),40);
        assert_eq!(initial.bytes(true).len(),48);
        let before=initial.revision;
        let (accepted,_)=a.frame_request(&request(1,0x85,1.)).unwrap();
        assert!(accepted.prefer_pc_mouse && accepted.revision>before);
        a.set_session_values(vec![PathValuePair {
            path:vec!["frame_settings_sync".into(),"prefer_pc_mouse".into()],value:json!(false),
        }]).unwrap();
        assert!(!a.frame_request(&request(1,0x85,1.)).unwrap().0.prefer_pc_mouse);
        assert!(a.frame_request(&request(2,0x85,2.)).is_err());
        let read=a.frame_request(&request(0,0x80,0.)).unwrap().0;
        assert_eq!(read.bytes(false),read.bytes(true)[..40]);
        let mut old=serde_json::to_value(a.session()).unwrap();
        old["frame_settings_sync"].as_object_mut().unwrap().remove("prefer_pc_mouse");
        let restored:surfxr_session::SessionConfig=serde_json::from_value(old).unwrap();
        assert!(!restored.frame_settings_sync.prefer_pc_mouse);
        a.frame_request(&request(2,0x85,1.)).unwrap();
        let mut b=ServerSessionManager::new(None);
        *b.session_mut()=serde_json::from_value(serde_json::to_value(a.session()).unwrap()).unwrap();
        assert!(b.frame_snapshot(0).prefer_pc_mouse);
    }

}
