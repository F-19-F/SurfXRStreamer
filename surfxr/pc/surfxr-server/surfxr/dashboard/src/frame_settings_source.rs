//! Authenticated daemon requests reach the existing session owner only.
use super::*;
use surfxr_session::frame_settings::{FrameSettingsRequest,FrameSettingsSnapshot};

pub(super) fn request(source:&Mutex<SessionSource>,context:&egui::Context,
    events:&mpsc::Sender<PolledEvent>,port:u16,bytes:&[u8])->Option<Vec<u8>> {
    let request=FrameSettingsRequest::from_bytes(bytes)?;
    let mut guard=source.lock();
    if let SessionSource::Local(manager)=&mut *guard {
        let (snapshot,_)=manager.frame_request(&request).ok()?;
        if request.action & 0x7f !=0 {report_session_local(context,events,manager);}
        // Local ownership means SteamVR has already stopped.
        return Some(snapshot.bytes(request.action & 0x80 !=0));
    }
    drop(guard);
    let agent:ureq::Agent=ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_millis(400))).build().into();
    let snapshot:FrameSettingsSnapshot=agent
        .post(format!("http://127.0.0.1:{port}/api/session/frame-settings"))
        .header("X-SURFXR","true").send_json(&request).ok()?
        .into_body().read_json().ok()?;
    Some(snapshot.bytes(request.action & 0x80 !=0))
}
