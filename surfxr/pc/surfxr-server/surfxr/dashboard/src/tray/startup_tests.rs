//! Isolated native eframe regression: no DataSources, service, session or network.
use super::*;
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::{
    sync::atomic::AtomicUsize,
    time::{Duration, Instant},
};
use winit::platform::windows::EventLoopBuilderExtWindows;

unsafe extern "system" fn observe_visibility(
    hwnd: HWND,
    message: u32,
    w: WPARAM,
    l: LPARAM,
    id: usize,
    data: usize,
) -> LRESULT {
    unsafe {
        if message == WM_WINDOWPOSCHANGED && l != 0 {
            if (*(l as *const WINDOWPOS)).flags & SWP_SHOWWINDOW != 0 {
                (*(data as *const AtomicUsize)).fetch_add(1, Ordering::SeqCst);
            }
        } else if message == WM_NCDESTROY {
            RemoveWindowSubclass(hwnd, Some(observe_visibility), id);
        }
        DefSubclassProc(hwnd, message, w, l)
    }
}

struct Probe {
    tray: Tray,
    hwnd: HWND,
    identity: String,
    shows: Arc<AtomicUsize>,
    completed: Arc<AtomicBool>,
    show_panel: bool,
    started: Instant,
    next_check: Instant,
    phase: usize,
    exiting: bool,
}

impl eframe::App for Probe {
    fn logic(&mut self, ctx: &Context, _: &mut eframe::Frame) {
        self.exiting |= self.tray.update(ctx, self.exiting);
        if self.exiting {
            ctx.send_viewport_cmd(ViewportCommand::Close);
            return;
        }
        assert!(
            self.started.elapsed() < Duration::from_secs(8),
            "native tray test timed out"
        );
        ctx.request_repaint_after(Duration::from_millis(20));
        if Instant::now() < self.next_check {
            return;
        }
        self.next_check = Instant::now() + Duration::from_millis(300);
        let visible = unsafe { IsWindowVisible(self.hwnd) } != 0;
        match self.phase {
            0 => {
                assert_eq!(
                    visible, self.show_panel,
                    "unexpected startup panel visibility"
                );
                assert_eq!(
                    self.shows.load(Ordering::SeqCst),
                    usize::from(self.show_panel),
                    "unexpected native show (including transient flashes)"
                );
                assert!(matches!(
                    Tray::start_named(&self.identity, false).unwrap(),
                    Startup::Existing
                ));
            }
            1 => {
                assert_eq!(
                    visible, self.show_panel,
                    "duplicate logon startup changed visibility"
                );
                assert_eq!(
                    self.shows.load(Ordering::SeqCst),
                    usize::from(self.show_panel)
                );
                assert!(matches!(
                    Tray::start_named(&self.identity, true).unwrap(),
                    Startup::Existing
                ));
            }
            2 => {
                assert!(visible, "manual launch did not reveal the resident panel");
                unsafe {
                    PostMessageW(self.hwnd, WM_CLOSE, 0, 0);
                }
            }
            3 => {
                assert!(!visible, "window close did not return to tray");
                unsafe {
                    PostMessageW(self.tray.hwnd as HWND, CALLBACK, 0, WM_LBUTTONUP as LPARAM);
                }
            }
            4 => {
                assert!(visible, "tray click did not reopen the panel");
                self.completed.store(true, Ordering::SeqCst);
                self.tray.events.wake(true);
            }
            _ => panic!("tray exit was not processed"),
        }
        self.phase += 1;
    }

    fn ui(&mut self, ui: &mut eframe::egui::Ui, _: &mut eframe::Frame) {
        ui.label("SurfXR isolated tray startup test");
    }
}

impl Drop for Probe {
    fn drop(&mut self) {
        // Keep the observation counter alive until its callback is detached.
        unsafe {
            RemoveWindowSubclass(self.hwnd, Some(observe_visibility), 0);
        }
    }
}

#[test]
#[ignore = "requires an interactive Windows desktop and a native eframe renderer"]
fn native_startup_stays_hidden_until_explicit_activation() {
    for show_panel in [false, true] {
        check_startup(show_panel);
    }
}

fn check_startup(show_panel: bool) {
    let identity = format!(
        "SURFXR.Tray.Startup.Test.{}.{}",
        std::process::id(),
        show_panel
    );
    let Startup::New(_instance, Some(mut tray)) = Tray::start_named(&identity, false).unwrap() else {
        panic!("isolated tray creation failed");
    };
    let completed = Arc::new(AtomicBool::new(false));
    let result = completed.clone();
    eframe::run_native(
        "SurfXR isolated tray startup test",
        eframe::NativeOptions {
            viewport: eframe::egui::ViewportBuilder::default()
                .with_visible(show_panel)
                .with_active(show_panel)
                .with_inner_size((360.0, 120.0)),
            event_loop_builder: Some(Box::new(|builder| {
                builder.with_any_thread(true);
            })),
            ..Default::default()
        },
        Box::new(move |context| {
            tray.bind_panel(context, show_panel)?;
            let RawWindowHandle::Win32(handle) = context.window_handle()?.as_raw() else {
                panic!("expected native Windows panel");
            };
            let hwnd = handle.hwnd.get() as HWND;
            let shows = Arc::new(AtomicUsize::new(0));
            assert_ne!(
                unsafe {
                    SetWindowSubclass(
                        hwnd,
                        Some(observe_visibility),
                        0,
                        Arc::as_ptr(&shows) as usize,
                    )
                },
                0
            );
            let now = Instant::now();
            Ok(Box::new(Probe {
                tray,
                hwnd,
                identity,
                shows,
                completed,
                show_panel,
                started: now,
                next_check: now + Duration::from_millis(300),
                phase: 0,
                exiting: false,
            }))
        }),
    )
    .unwrap();
    assert!(
        result.load(Ordering::SeqCst),
        "native startup checks did not complete"
    );
}
