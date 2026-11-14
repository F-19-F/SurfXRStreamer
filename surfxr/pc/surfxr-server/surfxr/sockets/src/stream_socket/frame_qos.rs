//! Optional OS hints for negotiated Frame UDP sockets, with scoped ownership.
use std::net::UdpSocket;

#[cfg(windows)]
use std::{ffi::c_void, os::windows::io::AsRawSocket};

#[cfg(windows)]
#[link(name = "qwave")]
unsafe extern "system" {
    fn QOSCreateHandle(version: *const [u16; 2], handle: *mut *mut c_void) -> i32;
    fn QOSAddSocketToFlow(
        handle: *mut c_void,
        socket: usize,
        destination: *const c_void,
        traffic: i32,
        flags: u32,
        flow: *mut u32,
    ) -> i32;
    fn QOSCloseHandle(handle: *mut c_void) -> i32;
    fn QOSQueryFlow(
        handle: *mut c_void,
        flow: u32,
        operation: i32,
        size: *mut u32,
        buffer: *mut c_void,
        flags: u32,
        overlapped: *mut c_void,
    ) -> i32;
}

pub struct FrameQos {
    #[cfg(windows)]
    handle: usize,
    pub applied: bool,
    pub error: u32,
    pub dscp: u32,
    pub l2: u32,
}

impl FrameQos {
    pub fn new(socket: &UdpSocket, priority: bool) -> Self {
        let mut result = Self {
            #[cfg(windows)]
            handle: 0,
            applied: false,
            error: 0,
            dscp: 255,
            l2: 255,
        };
        #[cfg(windows)]
        unsafe {
            let mut handle = std::ptr::null_mut();
            let mut flow = 0;
            if QOSCreateHandle(&[1, 0], &mut handle) == 0 {
                result.error = std::io::Error::last_os_error().raw_os_error().unwrap_or(0) as u32;
            } else {
                result.handle = handle as usize;
                // Connected socket: no destination pointer or bandwidth probing.
                // Voice stays above AudioVideo. Neither flow shapes encoder rate.
                result.applied = QOSAddSocketToFlow(
                    handle,
                    socket.as_raw_socket() as usize,
                    std::ptr::null(),
                    if priority { 4 } else { 3 },
                    2, // QOS_NON_ADAPTIVE_FLOW (qos2.h)
                    &mut flow,
                ) != 0;
                if !result.applied {
                    result.error =
                        std::io::Error::last_os_error().raw_os_error().unwrap_or(0) as u32;
                } else {
                    let mut priority = [0u32; 4]; // QOS_PACKET_PRIORITY
                    let mut size = 16;
                    if QOSQueryFlow(
                        handle,
                        flow,
                        1,
                        &mut size,
                        priority.as_mut_ptr().cast(),
                        0,
                        std::ptr::null_mut(),
                    ) != 0
                    {
                        result.dscp = priority[0];
                        result.l2 = priority[2];
                    }
                }
            }
        }
        #[cfg(not(windows))]
        {
            use socket2::SockRef;
            match SockRef::from(socket).set_tos_v4(if priority { 46 << 2 } else { 40 << 2 }) {
                Ok(()) => {
                    result.applied = true;
                    result.dscp = if priority { 46 } else { 40 };
                }
                Err(error) => result.error = error.raw_os_error().unwrap_or(0) as u32,
            }
        }
        result
    }
}

impl Drop for FrameQos {
    fn drop(&mut self) {
        #[cfg(windows)]
        if self.handle != 0 {
            unsafe {
                QOSCloseHandle(self.handle as *mut c_void);
            }
        }
    }
}

// Registration must be created and reverted on the owning worker thread.
pub struct FrameUdpThread {
    #[cfg(windows)]
    handle: usize,
    _thread_bound: std::marker::PhantomData<std::rc::Rc<()>>,
}

#[cfg(windows)]
#[link(name = "avrt")]
unsafe extern "system" {
    fn AvSetMmThreadCharacteristicsW(task: *const u16, index: *mut u32) -> *mut c_void;
    fn AvSetMmThreadPriority(handle: *mut c_void, priority: i32) -> i32;
    fn AvRevertMmThreadCharacteristics(handle: *mut c_void) -> i32;
}

impl FrameUdpThread {
    pub fn new(priority: bool) -> Self {
        #[cfg(windows)]
        let handle = {
            let task: Vec<u16> = if priority { "Audio" } else { "Playback" }
                .encode_utf16()
                .chain([0])
                .collect();
            let mut index = 0;
            let handle = unsafe { AvSetMmThreadCharacteristicsW(task.as_ptr(), &mut index) };
            let error = if handle.is_null() {
                std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
            } else {
                0
            };
            let boosted = !handle.is_null() && unsafe { AvSetMmThreadPriority(handle, 1) } != 0;
            surfxr_common::info!(
                "Frame UDP scheduling: priority={priority}, registered={}, boosted={boosted}, error={error}",
                !handle.is_null()
            );
            handle as usize
        };
        #[cfg(not(windows))]
        let _ = priority;
        Self {
            #[cfg(windows)]
            handle,
            _thread_bound: std::marker::PhantomData,
        }
    }
}

impl Drop for FrameUdpThread {
    fn drop(&mut self) {
        #[cfg(windows)]
        if self.handle != 0 {
            unsafe {
                AvRevertMmThreadCharacteristics(self.handle as *mut c_void);
            }
        }
    }
}
