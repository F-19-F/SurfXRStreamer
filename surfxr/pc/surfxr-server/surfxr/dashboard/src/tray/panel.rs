//! Keep eframe's first-frame ShowWindow from revealing a tray-only startup.
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::io;
use windows_sys::Win32::{
    Foundation::*,
    UI::{Shell::*, WindowsAndMessaging::*},
};

pub struct StartupGuard(HWND);

impl StartupGuard {
    // Called on the panel's UI thread, before eframe renders its first frame.
    pub fn new(context: &impl HasWindowHandle) -> io::Result<Self> {
        let handle = context.window_handle().map_err(io::Error::other)?;
        let RawWindowHandle::Win32(handle) = handle.as_raw() else {
            return Err(io::Error::other("Dashboard requires a Win32 window"));
        };
        let hwnd = handle.hwnd.get() as HWND;
        if unsafe { SetWindowSubclass(hwnd, Some(keep_hidden), 0, 0) } == 0 {
            return Err(io::Error::other(
                "Cannot guard Dashboard startup visibility",
            ));
        }
        Ok(Self(hwnd))
    }
}

unsafe extern "system" fn keep_hidden(
    hwnd: HWND,
    message: u32,
    w: WPARAM,
    l: LPARAM,
    id: usize,
    _: usize,
) -> LRESULT {
    unsafe {
        if message == WM_WINDOWPOSCHANGING && l != 0 {
            let position = &mut *(l as *mut WINDOWPOS);
            if position.flags & SWP_SHOWWINDOW != 0 {
                position.flags &= !SWP_SHOWWINDOW;
            }
        } else if message == WM_STYLECHANGING && w == GWL_STYLE as isize as usize && l != 0 {
            // winit also reapplies WS_VISIBLE after ShowWindow when updating styles.
            (*(l as *mut STYLESTRUCT)).styleNew &= !WS_VISIBLE;
        } else if message == WM_NCDESTROY {
            RemoveWindowSubclass(hwnd, Some(keep_hidden), id);
        }
        DefSubclassProc(hwnd, message, w, l)
    }
}

impl Drop for StartupGuard {
    fn drop(&mut self) {
        // Released on the UI thread before an explicit show, or during teardown.
        unsafe {
            RemoveWindowSubclass(self.0, Some(keep_hidden), 0);
        }
    }
}
