//! UI health is separate from SURFXR driver/media readiness. Never restart Steam.
use surfxr_common::parking_lot::Mutex;
use std::{
    thread,
    time::{Duration, Instant},
};
struct Health {
    generation: u64,
    runtime: Option<(u32, u64)>,
    result: u8,
}
static HEALTH: Mutex<Health> = Mutex::new(Health {
    generation: 0,
    runtime: None,
    result: 0,
});

pub(crate) fn missing() -> bool {
    HEALTH.lock().result == 2
}

fn publish(generation: u64, result: u8) {
    let mut health = HEALTH.lock();
    if health.generation == generation {
        health.result = result;
    }
}

pub(crate) fn poll() {
    // Called by the existing UI at most once per second (no per-frame process scan).
    static NEXT: Mutex<Option<Instant>> = Mutex::new(None);
    let mut next = NEXT.lock();
    if next.is_some_and(|at| Instant::now() < at) {
        return;
    }
    *next = Some(Instant::now() + Duration::from_secs(1));
    drop(next);
    let identity = super::steamvr_identity();
    let mut health = HEALTH.lock();
    if health.runtime == identity {
        return;
    }
    health.runtime = identity;
    health.generation += 1;
    let generation = health.generation;
    health.result = 0;
    drop(health);
    if identity.is_none() {
        return;
    }
    thread::spawn(move || {
        let deadline = Instant::now() + Duration::from_secs(45);
        let mut missing_observed = false;
        loop {
            if HEALTH.lock().generation != generation {
                return;
            }
            let result = super::ui_probe::query_child();
            if HEALTH.lock().generation != generation || super::steamvr_identity() != identity {
                return;
            }
            match result {
                Ok(true) => {
                    publish(generation, 1);
                    return;
                }
                Ok(false) => missing_observed = true,
                Err(_) => {}
            }
            if Instant::now() >= deadline {
                if missing_observed {
                    publish(generation, 2);
                    surfxr_common::warn!(
                        "Steam's VR menu is unavailable. Restart with Steam; if it remains unavailable, exit SteamVR and restart the Steam client."
                    );
                }
                return;
            }
            thread::sleep(Duration::from_secs(2));
        }
    });
}
