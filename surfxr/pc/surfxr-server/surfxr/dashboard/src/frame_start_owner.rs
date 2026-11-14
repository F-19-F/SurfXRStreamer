//! One Dashboard-owned launch attempt. Readiness is driver evidence, never a
//! process-presence timer. Media/XR readiness belongs to the headset consumer.
use std::time::{Duration,Instant};

pub(super) const IDLE:u8=0;
pub(super) const STARTING:u8=1;
pub(super) const READY:u8=2;
pub(super) const FAILED:u8=3;
const WATCHDOG:Duration=Duration::from_secs(120);
#[derive(Clone,Copy,Default)]
pub(crate) struct RuntimeEvidence {
    pub driver_ready:bool,
    pub process:Option<(u32,u64)>,
    pub pending:bool,
}
#[derive(Clone,Copy,Default)]
pub(super) struct Snapshot {pub phase:u8,pub reason:u8,pub attempt:u64}
pub(super) struct Owner {
    pub snapshot:Snapshot,
    started:Option<Instant>,
    saw_process:bool,
    restarting:bool,
}
impl Owner {
    pub fn new()->Self{Self{snapshot:Snapshot{phase:IDLE,..Default::default()},started:None,saw_process:false,restarting:false}}
    pub fn begin(&mut self,now:Instant)->bool{
        if matches!(self.snapshot.phase,STARTING|READY){return false;}
        self.snapshot.attempt=self.snapshot.attempt.saturating_add(1);
        self.snapshot.phase=STARTING;self.snapshot.reason=0;self.started=Some(now);
        self.saw_process=false;self.restarting=false;true
    }
    pub fn launch_failed(&mut self){
        if self.snapshot.phase==READY{return;}
        self.snapshot.phase=FAILED;self.snapshot.reason=2;self.started=None;
    }
    pub fn refresh_ready(&mut self,now:Instant){
        if self.snapshot.phase==READY{self.snapshot.phase=STARTING;self.snapshot.reason=0;self.started=Some(now);}
    }
    pub fn launch_failed_for(&mut self,attempt:u64){if attempt==self.snapshot.attempt{self.launch_failed();}}
    pub fn observe_for(&mut self,attempt:u64,now:Instant,evidence:RuntimeEvidence,launching:bool){
        if attempt==self.snapshot.attempt{self.observe(now,evidence,launching);}
    }
    pub fn tick(&mut self,now:Instant){
        if self.snapshot.phase==STARTING&&self.started.is_some_and(|at|now.saturating_duration_since(at)>=WATCHDOG){
            self.snapshot.phase=FAILED;self.snapshot.reason=3;self.started=None;
        }
    }
    pub fn observe(&mut self,now:Instant,evidence:RuntimeEvidence,launching:bool){
        if evidence.driver_ready {
            self.snapshot.phase=READY;self.snapshot.reason=0;self.started=None;
            self.saw_process=evidence.process.is_some();self.restarting=evidence.pending;return;
        }
        if self.snapshot.phase==READY {
            // Driver-authorized configuration restarts briefly remove its API.
            // Keep this attempt pending; neither relaunch nor reset its identity.
            if evidence.process.is_none()&&!evidence.pending&&!self.restarting {
                self.snapshot.phase=FAILED;self.snapshot.reason=4;self.started=None;return;
            }
            self.snapshot.phase=STARTING;self.started=Some(now);self.restarting|=evidence.pending;
        }
        if self.snapshot.phase!=STARTING{return;}
        self.restarting |= evidence.pending;
        if evidence.process.is_some(){self.saw_process=true;}
        if self.saw_process&&evidence.process.is_none()&&!evidence.pending&&!self.restarting&&!launching {
            self.snapshot.phase=FAILED;self.snapshot.reason=4;self.started=None;
        }else{self.tick(now);}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn startup_uses_driver_readiness_and_restart_keeps_attempt(){
        let now=Instant::now();let mut owner=Owner::new();assert!(owner.begin(now));assert!(!owner.begin(now));
        let process=Some((1,2));owner.observe(now,RuntimeEvidence{process,..Default::default()},false);
        assert_eq!(owner.snapshot.phase,STARTING,"PID presence is not startup completion");
        owner.observe(now+Duration::from_secs(5),RuntimeEvidence{driver_ready:true,process,pending:false},false);
        assert_eq!(owner.snapshot.phase,READY);let attempt=owner.snapshot.attempt;
        owner.observe(now+Duration::from_secs(6),RuntimeEvidence{pending:true,..Default::default()},false);
        assert_eq!(owner.snapshot.phase,STARTING);assert_eq!(owner.snapshot.attempt,attempt);
        owner.observe(now+Duration::from_secs(7),RuntimeEvidence::default(),false);assert_eq!(owner.snapshot.phase,STARTING);
        owner.observe(now+Duration::from_secs(20),RuntimeEvidence{driver_ready:true,process,pending:false},false);
        assert_eq!(owner.snapshot.phase,READY);assert_eq!(owner.snapshot.attempt,attempt);
    }
    #[test]
    fn restart_during_initial_start_survives_process_and_ticket_gap() {
        let now=Instant::now();let mut owner=Owner::new();owner.begin(now);
        let attempt=owner.snapshot.attempt;
        owner.observe(now,RuntimeEvidence{process:Some((1,2)),pending:true,driver_ready:false},false);
        owner.observe(now+Duration::from_secs(2),RuntimeEvidence::default(),false);
        assert_eq!(owner.snapshot.phase,STARTING);
        owner.observe(now+Duration::from_secs(8),RuntimeEvidence{process:Some((3,4)),driver_ready:true,pending:false},false);
        assert_eq!((owner.snapshot.phase,owner.snapshot.attempt),(READY,attempt));
        // A later ordinary stop must still be reported as an exit.
        owner.observe(now+Duration::from_secs(9),RuntimeEvidence::default(),false);
        assert_eq!((owner.snapshot.phase,owner.snapshot.reason),(FAILED,4));
    }
    #[test]
    fn explicit_failure_runtime_exit_and_hung_start_remain_distinct(){
        let now=Instant::now();let mut owner=Owner::new();owner.begin(now);owner.launch_failed();
        assert_eq!((owner.snapshot.phase,owner.snapshot.reason),(FAILED,2));assert!(owner.begin(now));
        owner.observe(now,RuntimeEvidence{process:Some((1,2)),..Default::default()},false);
        owner.observe(now+Duration::from_secs(1),RuntimeEvidence::default(),false);assert_eq!(owner.snapshot.reason,4);
        assert!(owner.begin(now));owner.observe(now+WATCHDOG,RuntimeEvidence::default(),false);
        assert_eq!((owner.snapshot.phase,owner.snapshot.reason),(FAILED,3));
        assert!(owner.begin(now));let attempt=owner.snapshot.attempt;
        owner.observe(now,RuntimeEvidence{driver_ready:true,process:Some((1,2)),pending:false},false);
        owner.observe(now,RuntimeEvidence::default(),false);assert_eq!(owner.snapshot.reason,4);
        assert_eq!(owner.snapshot.attempt,attempt);
    }
    #[test]
    fn late_launch_and_probe_cannot_change_a_new_attempt_or_confirmed_ready(){
        let now=Instant::now();let mut owner=Owner::new();owner.begin(now);let old=owner.snapshot.attempt;
        owner.launch_failed_for(old);assert!(owner.begin(now));let current=owner.snapshot.attempt;
        let ready=RuntimeEvidence{driver_ready:true,process:Some((1,2)),pending:false};
        owner.observe_for(old,now,ready,false);owner.launch_failed_for(old);
        assert_eq!((owner.snapshot.phase,owner.snapshot.attempt),(STARTING,current));
        owner.observe_for(current,now,ready,false);owner.launch_failed_for(current);
        assert_eq!(owner.snapshot.phase,READY);assert!(!owner.begin(now));
        owner.refresh_ready(now);assert_eq!((owner.snapshot.phase,owner.snapshot.attempt),(STARTING,current));
        owner.observe_for(current,now,ready,false);assert_eq!(owner.snapshot.phase,READY);
    }
}
