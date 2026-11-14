//! Scope the existing qWAVE implementation to the daemon's physical sockets.
use std::{ffi::c_void, mem::ManuallyDrop, net::UdpSocket, os::windows::io::FromRawSocket};
#[unsafe(no_mangle)]
pub extern "C" fn FrameDaemonSocketQos(socket: u64, priority: bool) -> *mut c_void {
    if socket == 0 || socket == u64::MAX {
        return std::ptr::null_mut();
    }
    let socket = ManuallyDrop::new(unsafe { UdpSocket::from_raw_socket(socket) });
    Box::into_raw(Box::new(surfxr_sockets::FrameQos::new(&socket, priority))).cast()
}
#[unsafe(no_mangle)]
pub extern "C" fn FrameDaemonSocketQosStop(flow: *mut c_void) {
    if !flow.is_null() {
        drop(unsafe { Box::from_raw(flow.cast::<surfxr_sockets::FrameQos>()) });
    }
}
