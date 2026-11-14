//! Windows notification-area owner. Its message loop lives independently of the panel.
mod panel;
#[cfg(test)]
mod startup_tests;

use eframe::egui::{Context, ViewportCommand};
use std::{
    ptr,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread,
};
use windows_sys::Win32::{
    Foundation::*,
    System::{LibraryLoader::GetModuleHandleW, Threading::CreateMutexW},
    UI::{Shell::*, WindowsAndMessaging::*},
};

const CLASS: &str = "SURFXR.Dashboard.Tray";
const CALLBACK: u32 = WM_APP + 1;
const SHOW: u32 = WM_APP + 2;
const OPEN: usize = 1;
const EXIT: usize = 2;
fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}

#[derive(Default)]
struct Events {
    context: Mutex<Option<Context>>,
    show: AtomicBool,
    exit: AtomicBool,
    language: Mutex<crate::i18n::Language>,
}
impl Events {
    fn wake(&self, exit: bool) {
        if exit {
            self.exit.store(true, Ordering::Release);
        } else {
            self.show.store(true, Ordering::Release);
        }
        if let Some(ctx) = self.context.lock().unwrap().as_ref() {
            ctx.request_repaint();
        }
    }
}

struct WindowState {
    events: Arc<Events>,
    icon: NOTIFYICONDATAW,
    taskbar_created: u32,
}

unsafe extern "system" fn window_proc(hwnd: HWND, message: u32, w: WPARAM, l: LPARAM) -> LRESULT {
    unsafe {
        let state = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *mut WindowState;
        if !state.is_null() {
            let state = &mut *state;
            if message == state.taskbar_created {
                if Shell_NotifyIconW(NIM_ADD, &state.icon) == 0 {
                    state.events.wake(false);
                }
                return 0;
            }
            match message {
                SHOW => {
                    state.events.wake(false);
                    return 0;
                }
                CALLBACK => {
                    match l as u32 {
                        WM_LBUTTONUP | WM_LBUTTONDBLCLK => state.events.wake(false),
                        WM_RBUTTONUP | WM_CONTEXTMENU => {
                            crate::i18n::set_language(*state.events.language.lock().unwrap());
                            let menu = CreatePopupMenu();
                            let open = wide(&crate::i18n::tr("Open settings"));
                            let exit = wide(&crate::i18n::tr("Exit completely"));
                            AppendMenuW(menu, MF_STRING, OPEN, open.as_ptr());
                            AppendMenuW(menu, MF_STRING, EXIT, exit.as_ptr());
                            let mut point = POINT::default();
                            GetCursorPos(&mut point);
                            SetForegroundWindow(hwnd);
                            let selected = TrackPopupMenu(
                                menu,
                                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                point.x,
                                point.y,
                                0,
                                hwnd,
                                ptr::null(),
                            );
                            DestroyMenu(menu);
                            PostMessageW(hwnd, WM_NULL, 0, 0);
                            if selected == OPEN as i32 {
                                state.events.wake(false);
                            }
                            if selected == EXIT as i32 {
                                state.events.wake(true);
                            }
                        }
                        _ => {}
                    }
                    return 0;
                }
                WM_CLOSE => {
                    DestroyWindow(hwnd);
                    return 0;
                }
                WM_DESTROY => {
                    Shell_NotifyIconW(NIM_DELETE, &state.icon);
                    PostQuitMessage(0);
                    return 0;
                }
                _ => {}
            }
        }
        DefWindowProcW(hwnd, message, w, l)
    }
}

pub enum Startup {
    Existing,
    New(InstanceGuard, Option<Tray>),
}
// The Dashboard remains a singleton even when its notification icon/window
// cannot be created. Keep this guard through DataSources shutdown, independently
// of the optional tray UI and its message thread.
pub struct InstanceGuard(HANDLE);
impl Drop for InstanceGuard {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.0); }
    }
}
pub struct Tray {
    hidden_panel: Option<panel::StartupGuard>,
    events: Arc<Events>,
    hwnd: isize,
    thread: Option<thread::JoinHandle<()>>,
}
impl Tray {
    // Must run before session loading or acquiring broker/runtime ownership.
    pub fn start() -> std::io::Result<Startup> {
        Self::start_named(CLASS, !std::env::args().any(|arg| arg == "--tray"))
    }

    fn start_named(identity: &str, activate_existing: bool) -> std::io::Result<Startup> {
        unsafe {
            let name = wide(&format!("Local\\{identity}"));
            let instance = CreateMutexW(ptr::null(), 0, name.as_ptr());
            if instance.is_null() {
                return Err(std::io::Error::from_raw_os_error(GetLastError() as i32));
            }
            let existing = GetLastError() == ERROR_ALREADY_EXISTS;
            let instance = InstanceGuard(instance);
            if existing {
                let class = wide(identity);
                for _ in 0..40 {
                    let hwnd = FindWindowW(class.as_ptr(), ptr::null());
                    if !hwnd.is_null() {
                        if activate_existing {
                            PostMessageW(hwnd, SHOW, 0, 0);
                        }
                        break;
                    }
                    thread::sleep(std::time::Duration::from_millis(50));
                }
                return Ok(Startup::Existing);
            }
            let events = Arc::new(Events::default());
            let thread_events = events.clone();
            let language = crate::i18n::language();
            let (sender, receiver) = mpsc::sync_channel(1);
            let identity = identity.to_owned();
            let thread = thread::spawn(move || {
                crate::i18n::set_language(language);
                let class = wide(&identity);
                let module = GetModuleHandleW(ptr::null());
                let wc = WNDCLASSW {
                    lpfnWndProc: Some(window_proc),
                    hInstance: module,
                    lpszClassName: class.as_ptr(),
                    ..Default::default()
                };
                RegisterClassW(&wc);
                let hwnd = CreateWindowExW(
                    0,
                    class.as_ptr(),
                    class.as_ptr(),
                    0,
                    0,
                    0,
                    0,
                    0,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    module,
                    ptr::null(),
                );
                if hwnd.is_null() {
                    sender.send(0).ok();
                    return;
                }
                // winres embeds the application icon as resource 1.
                let icon = LoadImageW(
                    module,
                    1usize as *const u16,
                    IMAGE_ICON,
                    32,
                    32,
                    LR_DEFAULTCOLOR,
                ) as HICON;
                let owned_icon = !icon.is_null();
                let icon = if icon.is_null() {
                    LoadIconW(ptr::null_mut(), IDI_APPLICATION)
                } else {
                    icon
                };
                let mut state = Box::new(WindowState {
                    events: thread_events,
                    icon: NOTIFYICONDATAW {
                        cbSize: std::mem::size_of::<NOTIFYICONDATAW>() as u32,
                        hWnd: hwnd,
                        uID: 1,
                        uFlags: NIF_MESSAGE | NIF_ICON | NIF_TIP,
                        uCallbackMessage: CALLBACK,
                        hIcon: icon,
                        ..Default::default()
                    },
                    taskbar_created: RegisterWindowMessageW(wide("TaskbarCreated").as_ptr()),
                });
                let tip = wide("SurfXR");
                state.icon.szTip[..tip.len()].copy_from_slice(&tip);
                SetWindowLongPtrW(
                    hwnd,
                    GWLP_USERDATA,
                    (&mut *state as *mut WindowState) as isize,
                );
                if Shell_NotifyIconW(NIM_ADD, &state.icon) == 0 {
                    DestroyWindow(hwnd);
                    sender.send(0).ok();
                } else {
                    sender.send(hwnd as isize).ok();
                    let mut message = MSG::default();
                    while GetMessageW(&mut message, ptr::null_mut(), 0, 0) > 0 {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                if owned_icon {
                    DestroyIcon(icon);
                }
                UnregisterClassW(class.as_ptr(), module);
            });
            let hwnd = receiver.recv().unwrap_or(0);
            if hwnd == 0 {
                thread.join().ok();
                return Ok(Startup::New(instance, None));
            }
            Ok(Startup::New(instance, Some(Self {
                hidden_panel: None,
                events,
                hwnd,
                thread: Some(thread),
            })))
        }
    }

    pub fn bind_panel(
        &mut self, context: &eframe::CreationContext<'_>, show_panel: bool,
    ) -> std::io::Result<()> {
        if !show_panel {
            self.hidden_panel = Some(panel::StartupGuard::new(context)?);
        }
        self.attach(&context.egui_ctx);
        Ok(())
    }

    pub fn attach(&self, ctx: &Context) {
        *self.events.language.lock().unwrap() = crate::i18n::language();
        let mut context = self.events.context.lock().unwrap();
        if context.is_none() {
            *context = Some(ctx.clone());
        }
    }

    pub fn update(&mut self, ctx: &Context, exiting: bool) -> bool {
        self.attach(ctx);
        let request_exit = self.poll(ctx);
        if ctx.input(|input| input.viewport().close_requested()) && !exiting && !request_exit {
            ctx.send_viewport_cmd(ViewportCommand::CancelClose);
            ctx.send_viewport_cmd(ViewportCommand::Visible(false));
        }
        request_exit
    }

    pub fn poll(&mut self, ctx: &Context) -> bool {
        if self.events.show.swap(false, Ordering::AcqRel) {
            // Only user activation (or loss of the tray) may reveal the panel.
            if self.hidden_panel.take().is_some() {
                // Reset winit's cached visibility after the suppressed first-frame show.
                ctx.send_viewport_cmd(ViewportCommand::Visible(false));
            }
            ctx.send_viewport_cmd(ViewportCommand::Visible(true));
            ctx.send_viewport_cmd(ViewportCommand::Minimized(false));
            ctx.send_viewport_cmd(ViewportCommand::Focus);
        }
        self.events.exit.swap(false, Ordering::AcqRel)
    }
}
impl Drop for Tray {
    fn drop(&mut self) {
        unsafe {
            PostMessageW(self.hwnd as HWND, WM_CLOSE, 0, 0);
            if let Some(thread) = self.thread.take() {
                thread.join().ok();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};

    #[test]
    fn isolated_tray_reuses_owner_delivers_commands_and_removes_its_window() {
        // Unique test identity: never signal or replace the user's real Dashboard.
        let identity = format!("SURFXR.Tray.Test.{}", std::process::id());
        let Startup::New(instance, Some(mut tray)) = Tray::start_named(&identity, true).unwrap() else {
            panic!("tray creation failed");
        };
        let ctx = Context::default();
        tray.attach(&ctx);
        assert!(matches!(
            Tray::start_named(&identity, true).unwrap(),
            Startup::Existing
        ));
        let deadline = Instant::now() + Duration::from_secs(2);
        while !tray.events.show.load(Ordering::Acquire) && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(10));
        }
        assert!(tray.events.show.load(Ordering::Acquire));
        let output = ctx.run_ui(Default::default(), |ui| {
            assert!(!tray.poll(ui.ctx()));
        });
        let commands = &output.viewport_output[&eframe::egui::ViewportId::ROOT].commands;
        assert!(commands.contains(&ViewportCommand::Visible(true)));
        assert!(commands.contains(&ViewportCommand::Focus));
        assert!(!tray.events.show.load(Ordering::Acquire));
        let mut closing = eframe::egui::RawInput::default();
        closing
            .viewports
            .get_mut(&eframe::egui::ViewportId::ROOT)
            .unwrap()
            .events
            .push(eframe::egui::ViewportEvent::Close);
        let mut request_exit = false;
        let output = ctx.run_ui(closing.clone(), |ui| {
            request_exit = tray.update(ui.ctx(), false);
        });
        let commands = &output.viewport_output[&eframe::egui::ViewportId::ROOT].commands;
        assert!(!request_exit);
        assert!(commands.contains(&ViewportCommand::CancelClose));
        assert!(commands.contains(&ViewportCommand::Visible(false)));
        tray.events.wake(true);
        let output = ctx.run_ui(closing.clone(), |ui| {
            request_exit = tray.update(ui.ctx(), false);
        });
        assert!(request_exit); // Tray exit proceeds directly without reopening the panel.
        let commands = &output.viewport_output[&eframe::egui::ViewportId::ROOT].commands;
        assert!(!commands.contains(&ViewportCommand::CancelClose));
        assert!(!commands.contains(&ViewportCommand::Visible(true)));
        assert!(!commands.contains(&ViewportCommand::Focus));
        let output = ctx.run_ui(closing, |ui| {
            assert!(!tray.update(ui.ctx(), true));
        });
        assert!(
            !output.viewport_output[&eframe::egui::ViewportId::ROOT]
                .commands
                .contains(&ViewportCommand::CancelClose)
        );
        assert!(!tray.poll(&ctx));
        drop(tray);
        assert!(unsafe { FindWindowW(wide(&identity).as_ptr(), ptr::null()) }.is_null());
        drop(instance);
        let Startup::New(restarted_instance, Some(restarted)) = Tray::start_named(&identity, true).unwrap() else {
            panic!("owner not released");
        };
        drop(restarted);
        drop(restarted_instance);
    }
}
