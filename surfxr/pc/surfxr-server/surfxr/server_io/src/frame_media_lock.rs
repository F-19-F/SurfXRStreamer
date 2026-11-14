//! Own the Windows media lease until the encoder and video sender have stopped.
#[cfg(windows)]
mod windows {
    use std::{ffi::c_void, ptr};
    #[repr(C)]
    struct Attributes {
        size: u32,
        descriptor: *mut c_void,
        inherit: i32,
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn CreateSemaphoreExW(
            attrs: *const Attributes,
            initial: i32,
            maximum: i32,
            name: *const u16,
            flags: u32,
            access: u32,
        ) -> *mut c_void;
        fn WaitForSingleObject(handle: *mut c_void, timeout: u32) -> u32;
        fn ReleaseSemaphore(handle: *mut c_void, count: i32, previous: *mut i32) -> i32;
        fn CloseHandle(handle: *mut c_void) -> i32;
        fn LocalFree(memory: *mut c_void) -> *mut c_void;
    }
    #[link(name = "advapi32")]
    unsafe extern "system" {
        fn ConvertStringSecurityDescriptorToSecurityDescriptorW(
            text: *const u16,
            version: u32,
            output: *mut *mut c_void,
            size: *mut u32,
        ) -> i32;
    }
    pub struct FrameMediaLease(*mut c_void);
    // Semaphore ownership may span the pipeline and encoder event thread.
    unsafe impl Send for FrameMediaLease {}
    unsafe impl Sync for FrameMediaLease {}
    impl FrameMediaLease {
        pub fn acquire() -> std::io::Result<Self> {
            let name: Vec<_> = "Global\\SURFXRFrameMedia-v2\0".encode_utf16().collect();
            // SYSTEM owns the secure-desktop worker; authenticated local users
            // may synchronize and release their own acquisition, never change ACLs.
            let acl: Vec<_> = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100002;;;AU)\0"
                .encode_utf16()
                .collect();
            unsafe {
                let mut descriptor = ptr::null_mut();
                if ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    acl.as_ptr(),
                    1,
                    &mut descriptor,
                    ptr::null_mut(),
                ) == 0
                {
                    return Err(std::io::Error::last_os_error());
                }
                let attrs = Attributes {
                    size: size_of::<Attributes>() as u32,
                    descriptor,
                    inherit: 0,
                };
                let handle = CreateSemaphoreExW(&attrs, 1, 1, name.as_ptr(), 0, 0x00100002);
                LocalFree(descriptor);
                if handle.is_null() {
                    return Err(std::io::Error::last_os_error());
                }
                match WaitForSingleObject(handle, 8000) {
                    0 => Ok(Self(handle)),
                    _ => {
                        CloseHandle(handle);
                        Err(std::io::Error::other(
                            "Another desktop/VR encoder still owns the media lease",
                        ))
                    }
                }
            }
        }
    }
    impl Drop for FrameMediaLease {
        fn drop(&mut self) {
            unsafe {
                ReleaseSemaphore(self.0, 1, ptr::null_mut());
                CloseHandle(self.0);
            }
        }
    }
}
#[cfg(windows)]
pub use windows::FrameMediaLease;
#[cfg(not(windows))]
pub struct FrameMediaLease;
#[cfg(not(windows))]
impl FrameMediaLease {
    pub fn acquire() -> std::io::Result<Self> {
        Ok(Self)
    }
}
